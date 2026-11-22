/*
 * Copyright (c) 2026 Daniel Tillett
 * All rights reserved.
 *
 * This file is part of Klin (derived from KLone), and as such it is subject
 * to the license stated in the LICENSE file which you have received as part
 * of this distribution.
 */

/* Unit tests linked against the real Klin libraries (CTest: "unit"). Unlike
 * test/regressions.c, nothing is stubbed: header parsing runs on the real
 * buffered io, the translator on real parser input, the timer. */

#include "klin_conf.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <u/libu.h>
#include <klin/emb.h>
#include <klin/field.h>
#include <klin/header.h>
#include <klin/dypage.h>
#include <klin/io.h>
#include <klin/ioprv.h>
#include <klin/rsfilter.h>
#include <klin/timer.h>
#include <klin/translat.h>
#include <klin/json.h>
#include <klin/utils.h>
#include <math.h>
#include <klin/crypto.h>
#include <klin/klog.h>
#include <klin/klogprv.h>
#include <klin/context.h>
#include <klin/vhost.h>
#include "../../src/libhttp/http_s.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "../../src/klind/server_s.h"
#ifdef SSL_OPENSSL
#include <klin/auth.h>
#endif
#ifdef HAVE_LIBZ
#include <klin/cgzip.h>
#include <zlib.h>
#endif

/* the syslog facility LibU logs with: the program defines it */
int facility = LOG_LOCAL0;
static context_t test_context;
context_t *ctx = &test_context;

static int checks, failures;

#define CHECK(expr) do {                                                    \
        ++checks;                                                           \
        if(!(expr)) {                                                       \
            ++failures;                                                     \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__,         \
                    __LINE__, #expr);                                       \
        }                                                                   \
    } while(0)

/* Fill the read buffer while leaving raw input for the next refill. */
static ssize_t expand_byte(codec_t *codec, char *dst, size_t *dcount,
        const char *src, size_t sz)
{
    (void) codec;
    (void) sz;
    memset(dst, *src, *dcount);
    return 1;
}

static void test_io_read_codecs(void)
{
    char data[8192], out[4096];
    io_t *io = NULL, *dup = NULL;
    codec_t *codec = u_zalloc(sizeof *codec);

    memset(data, 'a', 4096);
    memset(data + 4096, 'b', 4096);
    data[4096] = 'c';
    CHECK(codec && io_mem_create(data, sizeof data, 0, &io) == 0);
    if(!codec || !io)
        goto done;

    CHECK(io_read(io, out, 1) == 1 && out[0] == 'a' && io->ubuf == NULL);
    CHECK(io_dup(io, &dup) == 0 && dup == io);
    codec->transform = expand_byte;
    CHECK(io_codec_add_tail(dup, codec) == 0);
    if(!io_has_codecs(io))
        goto done;
    codec = NULL;

    CHECK(io_read(dup, out, 4095) == 4095 &&
            memcmp(out, data + 1, 4095) == 0 && io->ubuf == NULL);
    CHECK(io_read(dup, out, sizeof out) == sizeof out && out[4095] == 'c' &&
            io->ubuf != NULL && io->ucount == 4095);
    CHECK(io_codecs_discard(io) == 0);
    CHECK(io_read(dup, out, sizeof out) == 4095 &&
            memcmp(out, data + 4097, 4095) == 0 && io->ucount == 0);

done:
    codec_free(codec);
    if(dup)
        io_free(dup);
    if(io)
        io_free(io);
}

#ifdef HAVE_LIBZ
static void test_deflate_windows(void)
{
    char data[32768], decoded[sizeof data + 1];
    unsigned char encoded[sizeof data + 128];
    uint32_t random = 1;
    size_t i;
    int j;

    /* The second half can only match the first using a distance over 8 KB. */
    for(i = 0; i < sizeof data / 2; ++i)
    {
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        data[i] = (char) (random >> 24);
        data[i + sizeof data / 2] = data[i];
    }

    for(j = 0; j < 2; ++j)
    {
        z_stream z = {0};
        codec_t *codec = NULL;
        size_t n = sizeof decoded;
        int rc;

        rc = deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                j ? MAX_WBITS : -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        CHECK(rc == Z_OK);
        if(rc != Z_OK)
            continue;
        z.next_in = (Bytef *)data;
        z.avail_in = sizeof data;
        z.next_out = encoded;
        z.avail_out = sizeof encoded;
        CHECK(deflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out < 18000);
        CHECK(codec_gzip_create(j ? ZLIB_UNCOMPRESS : GZIP_UNCOMPRESS, &codec) == 0);
        if(codec)
        {
            CHECK(codec->transform(codec, decoded, &n, (char *)encoded,
                        z.total_out) == (ssize_t) z.total_out &&
                    n == sizeof data && memcmp(decoded, data, n) == 0);
            codec_free(codec);
        }
        deflateEnd(&z);
    }
}
#endif

/* load 'text' into a new header with header_load_ex(); return its result */
static int load(const char *text, size_t len, header_t **ph)
{
    io_t *io = NULL;
    int rc;

    if(io_mem_create((char *) text, len, 0, &io) || header_create(ph))
        return ~0;
    rc = header_load_ex(*ph, io, HLM_ADD);
    io_free(io);
    return rc;
}

static void test_header_fields(void)
{
    static const char text[] = "Host: x\r\nCookie: a=1\r\nConnection: "
        "keep-alive, Close\r\nCookie: b=2\r\n\r\n";
    header_t *h = NULL;
    field_t *f;
    int n = 0;

    CHECK(load(text, strlen(text), &h) == 0);
    CHECK(header_field_count(h) == 4);
    CHECK(header_count_field(h, "cookie") == 2);
    CHECK(header_count_field(h, "missing") == 0);

    /* header_next_field: by name, in order; all fields with NULL */
    f = header_next_field(h, "COOKIE", NULL);
    CHECK(f && !strcmp(field_get_value(f), "a=1"));
    f = header_next_field(h, "cookie", f);
    CHECK(f && !strcmp(field_get_value(f), "b=2"));
    CHECK(header_next_field(h, "cookie", f) == NULL);
    for(f = NULL; (f = header_next_field(h, NULL, f)) != NULL; )
        ++n;
    CHECK(n == 4);

    /* header_get_fieldn resumes from its cursor, also backwards */
    CHECK(!strcmp(field_get_name(header_get_fieldn(h, 3)), "Cookie"));
    CHECK(!strcmp(field_get_name(header_get_fieldn(h, 0)), "Host"));
    CHECK(header_get_fieldn(h, 4) == NULL);

    CHECK(header_has_token(h, "connection", "close"));
    CHECK(!header_has_token(h, "connection", "upgrade"));
    header_free(h);
}

/* "X: aaa...\r\n" lines of 'len' bytes (with the CRLF), 'n' of them, then
   the empty line */
static char *make_lines(size_t len, size_t n, size_t *plen)
{
    size_t i, total = len * n + 2;
    char *buf = malloc(total), *p = buf;

    for(i = 0; i < n; ++i, p += len)
    {
        memset(p, 'a', len);
        memcpy(p, "X: ", 3);
        memcpy(p + len - 2, "\r\n", 2);
    }
    memcpy(p, "\r\n", 2);
    *plen = total;
    return buf;
}

static void test_header_limits(void)
{
    header_t *h;
    size_t len;
    char *buf;

    /* lines longer than the io buffer (4 KB), read across refills: 8192
       bytes with the LF is the longest accepted */
    buf = make_lines(8192, 1, &len);
    h = NULL;
    CHECK(load(buf, len, &h) == 0 && header_field_count(h) == 1 &&
            strlen(field_get_value(header_get_fieldn(h, 0))) == 8192 - 5);
    header_free(h);
    free(buf);

    buf = make_lines(8193, 1, &len);
    h = NULL;
    CHECK(load(buf, len, &h) != 0);
    header_free(h);
    free(buf);

    /* at most 256 fields */
    buf = make_lines(10, 256, &len);
    h = NULL;
    CHECK(load(buf, len, &h) == 0 && header_field_count(h) == 256);
    header_free(h);
    free(buf);

    buf = make_lines(10, 257, &len);
    h = NULL;
    CHECK(load(buf, len, &h) != 0);
    header_free(h);
    free(buf);

    /* at most 64 KB in all */
    buf = make_lines(8000, 9, &len);
    h = NULL;
    CHECK(load(buf, len, &h) != 0);
    header_free(h);
    free(buf);

    /* a header cut short (no empty line) is an error */
    h = NULL;
    CHECK(load("X: 1\r\n", 6, &h) != 0);
    header_free(h);
}

static void test_field_reuse(void)
{
    char longer[300];
    field_t *f = NULL, *g = NULL;

    CHECK(field_create("Name", "short", &f) == 0);

    /* new values taken from the field's own buffer */
    CHECK(field_set(f, field_get_name(f), field_get_name(f)) == 0);
    CHECK(!strcmp(field_get_name(f), "Name") &&
            !strcmp(field_get_value(f), "Name"));

    /* a value that does not fit the buffer */
    memset(longer, 'v', sizeof longer - 1);
    longer[sizeof longer - 1] = '\0';
    CHECK(field_set(f, "Name", longer) == 0);
    CHECK(!strcmp(field_get_value(f), longer));
    CHECK(field_set(f, "Other", field_get_value(f) + 290) == 0);
    CHECK(!strcmp(field_get_name(f), "Other") &&
            !strcmp(field_get_value(f), "vvvvvvvvv"));
    field_free(f);

    /* freed fields are reused: the next one must start clean */
    CHECK(field_create("A", "1", &f) == 0);
    field_free(f);
    CHECK(field_create(NULL, NULL, &g) == 0);
    CHECK(field_get_name(g) == NULL && field_get_value(g) == NULL);
    field_free(g);
}

static int fired;

static int on_alarm(talarm_t *al, void *arg)
{
    (void) al;
    fired = *(int *) arg;
    return 0;
}

/* an alarm that closes the descriptor being waited for (as a timeout does) */
static int on_close(talarm_t *al, void *arg)
{
    int *pfd = arg;

    (void) al;
    close(*pfd);
    *pfd = -1;
    return 0;
}

static void test_timer(void)
{
    talarm_t *a = NULL, *b = NULL, *c = NULL;
    int one = 1, two = 2, p[2];
    time_t t0, dt;

    /* alarms run while waiting: a deleted one never, the others in order,
       the last closing the descriptor, which ends the wait */
    fired = 0;
    CHECK(timerm_add(1, on_alarm, &one, &a) == 0);
    CHECK(timerm_add(2, on_alarm, &two, &b) == 0);
    CHECK(timerm_armed());
    CHECK(timerm_del(a) == 0);      /* the earliest one */
    CHECK(pipe(p) == 0);
    CHECK(timerm_add(3, on_close, &p[0], &c) == 0);
    t0 = time(NULL);
    CHECK(timerm_wait(&p[0], TIMERM_WAIT_READ) != 0);
    dt = time(NULL) - t0;
    CHECK(p[0] == -1 && fired == 2);
    CHECK(dt >= 2 && dt <= 5);
    CHECK(!timerm_armed());
    CHECK(timerm_del(b) == 0);      /* fired: off the list, still freed here */
    CHECK(timerm_del(c) == 0);
    close(p[1]);

    /* a ready descriptor does not wait for the alarm, which stays set */
    fired = 0;
    CHECK(pipe(p) == 0);
    CHECK(write(p[1], "x", 1) == 1);
    CHECK(timerm_add(5, on_alarm, &one, &a) == 0);
    t0 = time(NULL);
    CHECK(timerm_wait(&p[0], TIMERM_WAIT_READ) == 0);
    CHECK(time(NULL) - t0 <= 1 && fired == 0);
    CHECK(timerm_armed());
    CHECK(timerm_del(a) == 0);
    CHECK(!timerm_armed() && timerm_run() == -1);
    close(p[0]);
    close(p[1]);
}

static void test_timed_fd_write(void)
{
    int fd[2], flags;
    io_t *out = NULL;
    char got = 0;

    if(socketpair(AF_UNIX, SOCK_STREAM, 0, fd))
    {
        CHECK(!"create timed socket pair");
        return;
    }
    if(io_fd_create(fd[0], IO_FD_CLOSE, &out))
    {
        CHECK(!"create timed socket");
        close(fd[0]);
        close(fd[1]);
        return;
    }
    flags = fcntl(fd[0], F_GETFL);
    CHECK(flags >= 0 && !(flags & O_NONBLOCK));
    io_set_write_timeout(out, 1);
    CHECK(io_write(out, "x", 1) == 1 && io_flush(out) == 0);
    /* the blocking mode is restored (compare that bit only: after a write
       macOS also reports a kernel flag, FWASWRITTEN, in F_GETFL) */
    CHECK((fcntl(fd[0], F_GETFL) & O_NONBLOCK) == (flags & O_NONBLOCK));
    io_free(out);
    CHECK(read(fd[1], &got, 1) == 1 && got == 'x');
    close(fd[1]);
}

/* an armed alarm (a CGI's timeout) still runs while a write with no
   deadline of its own waits for a peer that stopped reading: here it
   closes the peer, and the write fails instead of blocking for ever */
static void test_alarm_during_write(void)
{
    enum { BIG = 8 * 1024 * 1024 };
    talarm_t *a = NULL;
    io_t *out = NULL;
    char *big = NULL;
    int fd[2];
    time_t t0;
    void (*old)(int);

    if(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) ||
            io_fd_create(fd[0], IO_FD_CLOSE, &out) ||
            (big = u_zalloc(BIG)) == NULL)
    {
        CHECK(!"set up the write");
        return;
    }
    old = signal(SIGPIPE, SIG_IGN);
    CHECK(timerm_add(1, on_close, &fd[1], &a) == 0);
    t0 = time(NULL);
    CHECK(io_write(out, big, BIG) < BIG || io_flush(out) != 0);
    CHECK(fd[1] == -1 && time(NULL) - t0 <= 5);
    CHECK(timerm_del(a) == 0);
    (void) signal(SIGPIPE, old);
    io_free(out);
    u_free(big);
}

/* translate 'page' as 'uri'; return the generated C (to free) or NULL */
static char *translate_page(const char *page, const char *uri)
{
    char path[] = "/tmp/klin-unit-XXXXXX", *c = NULL;
    trans_info_t ti;
    io_t *in = NULL, *out = NULL;
    FILE *fp;
    long sz;
    int fd;

    memset(&ti, 0, sizeof ti);
    (void) u_strlcpy(ti.uri, uri, sizeof ti.uri);
    (void) u_strlcpy(ti.file_in, "page.kl1", sizeof ti.file_in);

    if((fd = mkstemp(path)) < 0)
        return NULL;
    if(io_mem_create((char *) page, strlen(page), 0, &in) ||
            io_fd_create(fd, IO_FD_CLOSE, &out) ||
            translate_script_to_c(in, out, &ti))
        goto end;
    io_free(out);
    out = NULL;

    if((fp = fopen(path, "rb")) != NULL)
    {
        fseek(fp, 0, SEEK_END);
        sz = ftell(fp);
        rewind(fp);
        if(sz > 0 && (c = calloc(1, (size_t) sz + 1)) != NULL &&
                fread(c, 1, (size_t) sz, fp) != (size_t) sz)
        {
            free(c);
            c = NULL;
        }
        fclose(fp);
    }
end:
    if(in)
        io_free(in);
    if(out)
        io_free(out);
    unlink(path);
    return c;
}

static void test_translate(void)
{
    char *c;

    c = translate_page("<b><%= \"x\" %></b><% int i = 1; (void) i; %>"
            "<%! static int helper; %>\n", "/dir/page.kl1");
    CHECK(c != NULL);
    if(c)
    {
        CHECK(strstr(c, "SCRIPT_NAME = \"page.kl1\";") != NULL);
        /* <%= expr %> prints the expression (spacing is the parser's) */
        CHECK(strstr(c, "io_printf(out, \"%s\",") != NULL &&
                strstr(strstr(c, "io_printf(out, \"%s\","), "\"x\"") != NULL);
        CHECK(strstr(c, "int i = 1; (void) i;") != NULL);
        CHECK(strstr(c, "static int helper;") != NULL);
        CHECK(strstr(c, "e.res.filename = \"/dir/page.kl1\";") != NULL);
        free(c);
    }

    /* a URI is written as an escaped C string literal ("?\?=": a trigraph
       in this source otherwise) */
    c = translate_page("<p>", "/a\"b\\c?\?=\001.kl1");
    CHECK(c != NULL);
    if(c)
    {
        CHECK(strstr(c, "SCRIPT_NAME = \"a\\\"b\\\\c\\?\\?=\\001.kl1\";")
                != NULL);
        free(c);
    }
}

#ifdef SSL_OPENSSL
/* HTTP authentication hashes: the RFC 7616 3.9.1 examples, and PBKDF2
   password hashes */
static void test_auth(void)
{
    static const char nonce[] = "7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v",
        cnonce[] = "f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ";
    char ha1[AUTH_HEX_SZ], r[AUTH_HEX_SZ], stored[AUTH_PASSWORD_SZ],
         other[AUTH_PASSWORD_SZ];

    CHECK(auth_digest_ha1(AUTH_ALG_SHA256, "Mufasa", "http-auth@example.org",
                "Circle of Life", ha1) == 0);
    CHECK(!strcmp(ha1, "7987c64c30e25f1b74be53f966b49b90"
                "f2808aa92faf9a00262392d7b4794232"));
    CHECK(auth_digest_response(AUTH_ALG_SHA256, ha1, nonce, "00000001",
                cnonce, "GET", "/dir/index.html", r) == 0);
    CHECK(!strcmp(r, "753927fa0e85d155564e2e272a28d180"
                "2ca10daf4496794697cf8db5856cb6c1"));

    CHECK(auth_digest_ha1(AUTH_ALG_MD5, "Mufasa", "http-auth@example.org",
                "Circle of Life", ha1) == 0);
    CHECK(auth_digest_response(AUTH_ALG_MD5, ha1, nonce, "00000001",
                cnonce, "GET", "/dir/index.html", r) == 0);
    CHECK(!strcmp(r, "8ca523f5e9506fed4657c9700eebdbec"));

    CHECK(auth_alg_from_name("sha-256") == AUTH_ALG_SHA256);
    CHECK(auth_alg_from_name("MD5") == AUTH_ALG_MD5);
    CHECK(auth_alg_from_name("SHA-512-256") == 0);
    CHECK(auth_alg_from_name(NULL) == 0);

    /* salted: the same password hashes differently, both verify */
    CHECK(auth_password_hash("s3cret pw", 1000, stored, sizeof stored) == 0);
    CHECK(auth_password_hash("s3cret pw", 1000, other, sizeof other) == 0);
    CHECK(!strncmp(stored, "pbkdf2-sha256$1000$", 19));
    CHECK(strcmp(stored, other) != 0);
    CHECK(auth_password_verify("s3cret pw", stored) == 0);
    CHECK(auth_password_verify("s3cret pw", other) == 0);
    CHECK(auth_password_verify("s3cret pX", stored) != 0);
    CHECK(auth_password_verify("", stored) != 0);

    /* malformed stored values never verify (and plaintext is not a hash) */
    CHECK(auth_password_verify("s3cret pw", "s3cret pw") != 0);
    CHECK(auth_password_verify("x", "pbkdf2-sha256$0$AAAA$AAAA") != 0);
    CHECK(auth_password_verify("x", "pbkdf2-sha256$10$AAAA") != 0);
    CHECK(auth_password_verify("x", "pbkdf2-sha256$x$$") != 0);
    stored[strlen(stored) - 3] ^= 1;    /* a changed hash */
    CHECK(auth_password_verify("s3cret pw", stored) != 0);

    /* a hash that does not fit the buffer is an error */
    CHECK(auth_password_hash("pw", 1000, stored, 20) != 0);
}
#endif

/* parse 'text': 0 if valid (then *pj is set), ~0 if refused */
static int parse(const char *text, json_t **pj)
{
    char err[128];

    return json_parse(text, strlen(text), pj, err, sizeof err);
}

static int refused(const char *text)
{
    json_t *j = NULL;
    int rc = parse(text, &j);

    json_free(j);
    return rc != 0;
}

static void test_json_parse(void)
{
    static const char doc[] = " {\"name\": \"Klin\", \"n\": -12, \"pi\": 3.25e0,"
        " \"big\": 123456789012345678901, \"ok\": true, \"no\": false,"
        " \"none\": null, \"list\": [1, [2, {}], \"x\"],"
        " \"esc\": \"a\\\"b\\\\c\\/d\\n\\u00e9\\u2603\\ud83d\\ude00\" } ";
    json_t *j = NULL, *list;
    char err[128];
    int64_t v;

    CHECK(parse(doc, &j) == 0);
    CHECK(json_type(j) == JSON_OBJECT && json_count(j) == 9);
    CHECK(!strcmp(json_string(json_get(j, "name")), "Klin"));
    CHECK(json_int64(json_get(j, "n"), &v) == 0 && v == -12);
    CHECK(json_number(json_get(j, "pi")) == 3.25);
    CHECK(json_int64(json_get(j, "pi"), &v) != 0);      /* not an integer */
    CHECK(json_int64(json_get(j, "big"), &v) != 0);     /* out of range */
    CHECK(json_number(json_get(j, "big")) > 1e20);
    CHECK(json_bool(json_get(j, "ok")) && !json_bool(json_get(j, "no")));
    CHECK(json_type(json_get(j, "none")) == JSON_NULL &&
            json_get(j, "none") != NULL && json_get(j, "missing") == NULL);
    list = json_get(j, "list");
    CHECK(json_count(list) == 3 && json_count(json_at(list, 1)) == 2);
    CHECK(json_type(json_at(json_at(list, 1), 1)) == JSON_OBJECT);
    CHECK(json_at(list, 3) == NULL && json_string(json_at(list, 0)) == NULL);
    CHECK(!strcmp(json_string(json_get(j, "esc")),
                "a\"b\\c/d\n\xc3\xa9\xe2\x98\x83\xf0\x9f\x98\x80"));
    CHECK(!strcmp(json_key_at(j, 0), "name"));
    /* lookups through missing values are safe */
    CHECK(json_string(json_get(json_get(j, "nope"), "deeper")) == NULL);
    json_free(j);

    /* -0 keeps its sign: a double, not the integer 0 (found by fuzzing:
       -0.0 was written "-0", which read back as 0) */
    CHECK(parse("-0", &j) == 0 && json_int64(j, &v) != 0 &&
            json_number(j) == 0 && signbit(json_number(j)));
    json_free(j);
    CHECK(parse("0", &j) == 0 && json_int64(j, &v) == 0 && v == 0);
    json_free(j);

    CHECK(parse("\"a\\u0000b\"", &j) == 0 && json_string_len(j) == 3);
    json_free(j);
    CHECK(parse("0", &j) == 0 && json_int64(j, &v) == 0 && v == 0);
    json_free(j);
    CHECK(parse("-9223372036854775808", &j) == 0 && json_int64(j, &v) == 0 &&
            v == INT64_MIN);
    json_free(j);

    /* refused */
    CHECK(refused(""));
    CHECK(refused("   "));
    CHECK(refused("[1,]"));
    CHECK(refused("{\"a\":1,}"));
    CHECK(refused("[1 2]"));
    CHECK(refused("{'a':1}"));
    CHECK(refused("{a:1}"));
    CHECK(refused("[1] // comment"));
    CHECK(refused("[1] x"));
    CHECK(refused("01"));
    CHECK(refused("1."));
    CHECK(refused(".5"));
    CHECK(refused("+1"));
    CHECK(refused("1e"));
    CHECK(refused("NaN"));
    CHECK(refused("Infinity"));
    CHECK(refused("1e999"));
    CHECK(refused("tru"));
    CHECK(refused("nul"));
    CHECK(refused("\"tab\there\""));         /* raw control character */
    CHECK(refused("\"\\x41\""));
    CHECK(refused("\"\\u12\""));
    CHECK(refused("\"\\ud83d\""));          /* lone high surrogate */
    CHECK(refused("\"\\ude00\""));          /* lone low surrogate */
    CHECK(refused("\"\\ud83d\\u0041\""));
    CHECK(refused("\"\xc3\x28\""));         /* invalid UTF-8 */
    CHECK(refused("\"\xc0\xaf\""));         /* overlong */
    CHECK(refused("\"\xed\xa0\x80\""));     /* encoded surrogate */
    CHECK(refused("\"open"));
    CHECK(refused("{\"a\":1,\"a\":2}"));    /* duplicate key */
    CHECK(json_parse("[1]", 2, &j, err, sizeof err) != 0);  /* cut short */

    /* depth: JSON_MAX_DEPTH levels are fine, one more is not */
    {
        char deep[2 * JSON_MAX_DEPTH + 3];
        int i;

        for(i = 0; i < JSON_MAX_DEPTH; ++i)
        {
            deep[i] = '[';
            deep[JSON_MAX_DEPTH + i] = ']';
        }
        deep[2 * JSON_MAX_DEPTH] = '\0';
        CHECK(parse(deep, &j) == 0);
        json_free(j);
        memmove(deep + 1, deep, 2 * JSON_MAX_DEPTH + 1);
        deep[0] = '[';
        strcat(deep, "]");
        CHECK(refused(deep));
    }

    /* the error says what and where */
    CHECK(json_parse("[1,]", 4, &j, err, sizeof err) != 0 &&
            strstr(err, "offset 3") != NULL);
}

/* run 'fn' with a writer on a memory buffer: what it wrote, and whether
   the writer called it a complete document */
static char json_out[4096];

static int written(int (*fn)(json_writer_t *))
{
    json_writer_t *w = NULL;
    io_t *io = NULL;
    int rc;

    memset(json_out, 0, sizeof json_out);
    if(io_mem_create(json_out, sizeof json_out - 1, 0, &io) ||
            json_writer_create(io, &w))
        return -1;
    (void) fn(w);
    rc = json_writer_free(w);
    io_flush(io);
    io_free(io);
    return rc;
}

static int write_doc(json_writer_t *w)
{
    json_begin_object(w);
    json_key(w, "id");      json_int(w, -42);
    json_key(w, "name");    json_str(w, "<script>\"x\"&\\\n\xe2\x80\xa8");
    json_key(w, "bad");     json_strn(w, "a\xff" "b", 3);
    json_key(w, "list");    json_begin_array(w);
    json_double(w, 0.5);    json_boolean(w, 1);   json_null(w);
    json_begin_object(w);   json_end_object(w);
    json_end_array(w);
    json_key(w, "ctl");     json_str(w, "\x01");
    return json_end_object(w);
}

static int write_no_key(json_writer_t *w)
{
    json_begin_object(w);
    json_int(w, 1);         /* a value without a key */
    return json_end_object(w);
}

static int write_two(json_writer_t *w)
{
    json_int(w, 1);
    return json_int(w, 2);  /* a second top-level value */
}

static int write_open(json_writer_t *w)
{
    return json_begin_array(w);     /* never closed */
}

static int write_mismatch(json_writer_t *w)
{
    json_begin_array(w);
    return json_end_object(w);
}

static int write_nan(json_writer_t *w)
{
    return json_double(w, 0.0 / 0.0);
}

static int write_roundtrip(json_writer_t *w)
{
    json_t *j = NULL;
    int rc;

    if(parse("{\"a\":[1,2.5,\"s\\u00e9\",{\"b\":null}],\"c\":true}", &j))
        return ~0;
    rc = json_value(w, j);
    json_free(j);
    return rc;
}

static int write_nul_key(json_writer_t *w)
{
    json_t *j = NULL;
    int rc;

    /* keys that differ after a NUL stay apart */
    if(parse("{\"a\\u0000b\":1,\"a\":2}", &j))
        return ~0;
    rc = json_value(w, j);
    json_free(j);
    return rc;
}

static void test_json_write(void)
{
    CHECK(written(write_doc) == 0);
    CHECK(!strcmp(json_out, "{\"id\":-42,"
                "\"name\":\"\\u003cscript\\u003e\\\"x\\\"\\u0026\\\\\\n\\u2028\","
                "\"bad\":\"a\\ufffdb\","
                "\"list\":[0.5,true,null,{}],"
                "\"ctl\":\"\\u0001\"}"));
    CHECK(written(write_no_key) != 0);
    CHECK(written(write_two) != 0);
    CHECK(written(write_open) != 0);
    CHECK(written(write_mismatch) != 0);
    CHECK(written(write_nan) != 0);
    CHECK(written(write_roundtrip) == 0);
    CHECK(!strcmp(json_out,
                "{\"a\":[1,2.5,\"s\xc3\xa9\",{\"b\":null}],\"c\":true}"));
    CHECK(written(write_nul_key) == 0);
    CHECK(!strcmp(json_out, "{\"a\\u0000b\":1,\"a\":2}"));
}

#ifdef SSL_OPENSSL
/* the crypto interface (klin/crypto.h) against published vectors: SHA-256
   (FIPS 180-2 "abc"), HMAC-SHA256 (RFC 4231 case 2), PBKDF2-HMAC-SHA256
   (RFC 7914 section 11), AES-256-CBC (NIST SP 800-38A F.2.5) */
static int hex_is(const unsigned char *b, size_t n, const char *hex)
{
    char buf[2 * KC_MD_MAX + 1];
    size_t i;

    if(n * 2 >= sizeof buf || strlen(hex) != n * 2)
        return 0;
    for(i = 0; i < n; ++i)
        sprintf(buf + 2 * i, "%02x", b[i]);
    return !strcmp(buf, hex);
}

static void test_crypto(void)
{
    static const unsigned char key[32] = {
        0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae,
        0xf0, 0x85, 0x7d, 0x77, 0x81, 0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61,
        0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4 };
    static const unsigned char iv[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
        11, 12, 13, 14, 15 };
    static const unsigned char pt[16] = { 0x6b, 0xc1, 0xbe, 0xe2, 0x2e,
        0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a };
    unsigned char out[KC_MD_MAX], ct[64], back[64];
    size_t n, n2, total;
    kc_hmac_t *h = NULL;
    kc_cipher_t *c = NULL;
    char wiped[8] = "secret!";

    CHECK(kc_digest(KC_SHA256, "abc", 3, out, &n) == 0 && hex_is(out, n,
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(kc_md_size(KC_SHA256) == 32 && kc_md_size(KC_SHA1) == 20 &&
            kc_md_size(KC_MD5) == 16);
    CHECK(kc_md_from_name("SHA256") == KC_SHA256 &&
            kc_md_from_name("ripemd160") == KC_RIPEMD160 &&
            kc_md_from_name("sha512") == 0 && kc_md_from_name(NULL) == 0);

    CHECK(kc_hmac(KC_SHA256, "Jefe", 4, "what do ya want for nothing?", 28,
                out, &n) == 0 && hex_is(out, n,
                "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    /* a reused context: the same MAC for the message in parts, twice */
    CHECK(kc_hmac_new(KC_SHA256, "Jefe", 4, &h) == 0);
    if(h)
    {
        for(n2 = 0; n2 < 2; ++n2)
        {
            CHECK(kc_hmac_init(h) == 0 &&
                    kc_hmac_update(h, "what do ya ", 11) == 0 &&
                    kc_hmac_update(h, "want for nothing?", 17) == 0 &&
                    kc_hmac_final(h, out, &n) == 0 && hex_is(out, n,
                "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
        }
        kc_hmac_free(h);
    }

    CHECK(kc_pbkdf2_sha256("passwd", 6, (const unsigned char *) "salt", 4, 1,
                out, 32) == 0 && hex_is(out, 32,
                "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"));

    /* AES-256-CBC: the NIST block, then the PKCS #7 padding block */
    CHECK(kc_cipher_key_len(KC_AES_256_CBC) == 32 &&
            kc_cipher_iv_len(KC_AES_256_CBC) == 16 &&
            kc_cipher_block_size(KC_AES_256_CBC) == 16);
    CHECK(kc_cipher_new(KC_AES_256_CBC, 1, key, iv, &c) == 0);
    total = 0;
    if(c)
    {
        CHECK(kc_cipher_update(c, ct, &n, pt, sizeof pt) == 0);
        total = n;
        CHECK(kc_cipher_final(c, ct + total, &n) == 0);
        total += n;
        kc_cipher_free(c);
        c = NULL;
    }
    CHECK(total == 32 && hex_is(ct, 16, "f58c4c04d6e5f1ba779eabfb5f7bfbd6"));
    CHECK(kc_cipher_new(KC_AES_256_CBC, 0, key, iv, &c) == 0);
    if(c)
    {
        CHECK(kc_cipher_update(c, back, &n, ct, total) == 0);
        CHECK(kc_cipher_final(c, back + n, &n2) == 0);
        CHECK(n + n2 == sizeof pt && !memcmp(back, pt, sizeof pt));
        kc_cipher_free(c);
        c = NULL;
    }
    /* a wrong key: the padding check fails */
    back[0] = 0;
    CHECK(kc_cipher_new(KC_AES_256_CBC, 0, iv, key, &c) == 0);
    if(c)
    {
        (void) kc_cipher_update(c, back, &n, ct, total);
        CHECK(kc_cipher_final(c, back + n, &n2) != 0 ||
                memcmp(back, pt, sizeof pt));
        kc_cipher_free(c);
    }

    CHECK(kc_memeq("abc", "abc", 3) && !kc_memeq("abc", "abd", 3));
    CHECK(kc_random(out, sizeof out) == 0);
    kc_cleanse(wiped, sizeof wiped);
    CHECK(!memcmp(wiped, "\0\0\0\0\0\0\0\0", 8));

    /* AES-256-GCM: the GCM specification's test cases 13 and 14 (zero key
       and nonce; no plaintext, then one zero block), then forgeries */
    {
        unsigned char k[KC_AEAD_KEY] = { 0 }, nonce[KC_AEAD_NONCE] = { 0 },
                      zero[16] = { 0 }, sealed[16], tag[KC_AEAD_TAG],
                      back[16];

        CHECK(kc_aead_seal(k, nonce, NULL, 0, NULL, 0, NULL, tag) == 0 &&
                hex_is(tag, sizeof tag, "530f8afbc74536b9a963b4f1c4cb738b"));
        CHECK(kc_aead_seal(k, nonce, NULL, 0, zero, 16, sealed, tag) == 0 &&
                hex_is(sealed, 16, "cea7403d4d606b6e074ec5d3baf39d18") &&
                hex_is(tag, sizeof tag, "d0d1c8a799996bf0265b98b5d48ab919"));
        CHECK(kc_aead_open(k, nonce, NULL, 0, sealed, 16, tag, back) == 0 &&
                !memcmp(back, zero, 16));
        /* with additional data: it must match too */
        CHECK(kc_aead_seal(k, nonce, "sid|1", 5, zero, 16, sealed, tag) == 0);
        CHECK(kc_aead_open(k, nonce, "sid|1", 5, sealed, 16, tag, back) == 0);
        CHECK(kc_aead_open(k, nonce, "sid|2", 5, sealed, 16, tag, back) != 0);
        sealed[3] ^= 1;
        CHECK(kc_aead_open(k, nonce, "sid|1", 5, sealed, 16, tag, back) != 0);
        sealed[3] ^= 1;
        tag[0] ^= 0x80;
        CHECK(kc_aead_open(k, nonce, "sid|1", 5, sealed, 16, tag, back) != 0);
        tag[0] ^= 0x80;
        k[0] = 1;
        CHECK(kc_aead_open(k, nonce, "sid|1", 5, sealed, 16, tag, back) != 0);
    }

    /* base64 from a client: only the alphabet, '=' only at the end (a byte
       over 0x7f made LibU read out of its table: found by fuzzing) */
    {
        unsigned char raw[16];
        size_t rlen;

        rlen = sizeof raw;
        CHECK(klin_b64_decode("aGk=", 4, raw, &rlen) == 0 && rlen == 2 &&
                !memcmp(raw, "hi", 2));
        rlen = sizeof raw;
        CHECK(klin_b64_decode("aG\xc6k", 4, raw, &rlen) != 0);
        rlen = sizeof raw;
        CHECK(klin_b64_decode("a=Gk", 4, raw, &rlen) != 0);
        rlen = sizeof raw;
        CHECK(klin_b64_decode("aG===", 5, raw, &rlen) != 0);
        rlen = sizeof raw;
        CHECK(klin_b64_decode("aG k", 4, raw, &rlen) != 0);
        rlen = sizeof raw;
        CHECK(klin_b64_decode("aGk", 3, raw, &rlen) != 0);  /* unpadded */
        /* an exact fit, and one byte short (LibU wrote 3 bytes here) */
        memset(raw, 0x55, sizeof raw);
        rlen = 2;
        CHECK(klin_b64_decode("aGk=", 4, raw, &rlen) == 0 && rlen == 2 &&
                raw[2] == 0x55);
        rlen = 1;
        CHECK(klin_b64_decode("aGk=", 4, raw, &rlen) != 0);
    }

    /* u_cipher_encrypt/decrypt: *dcount is the size of dst, and data that
       may not fit is refused before anything is written (a session's
       KLIN_CIPHER_KEY longer than the key buffer overflowed it on the
       stack, inside OpenSSL: found by ClusterFuzzLite) */
    {
        unsigned char key[32], iv[16];
        char plain[48], sealed[64 + 16], out[16 + 32];
        size_t n, i;
        int clean = 1;

        memset(key, 0x4b, sizeof key);
        memset(iv, 0x49, sizeof iv);
        memset(plain, 'p', sizeof plain);
        n = sizeof sealed;
        CHECK(u_cipher_encrypt(KC_AES_256_CBC, key, iv, sealed, &n, plain,
                    sizeof plain) == 0 && n == 64);
        /* 64 bytes into a 16-byte buffer, followed by a guard area */
        memset(out, 0x55, sizeof out);
        n = 16;
        CHECK(u_cipher_decrypt(KC_AES_256_CBC, key, iv, out, &n, sealed, 64)
                != 0);
        for(i = 16; i < sizeof out; ++i)
            if(out[i] != 0x55)
                clean = 0;
        CHECK(clean);
        /* too small to encrypt into: refused too */
        n = 32;
        CHECK(u_cipher_encrypt(KC_AES_256_CBC, key, iv, sealed, &n, plain,
                    sizeof plain) != 0);
        /* enough room (the data and a block): decrypted */
        {
            char back[64 + 16];

            n = sizeof back;
            CHECK(u_cipher_decrypt(KC_AES_256_CBC, key, iv, back, &n, sealed,
                        64) == 0 && n == sizeof plain &&
                    !memcmp(back, plain, sizeof plain));
        }
    }
}
#endif

/* ctime(t) without its newline, the reference for klog_timestamp() */
static void ctime_ref(time_t t, char ref[KLOG_TS_SZ])
{
    char buf[26];
    const char *ct = ctime_r(&t, buf);

    snprintf(ref, KLOG_TS_SZ, "%.24s", ct ? ct : "?");
}

/* klog lines: their timestamps match ctime(3); log files are created mode
   0640 whatever the umask allows */
static void test_klog(void)
{
    static const time_t times[] = { 0, 86399, 1000000000, 1700000000 };
    char dir[] = "/tmp/klin-unit-XXXXXX", base[64], fn[80];
    char ts[KLOG_TS_SZ], ref[KLOG_TS_SZ], want[KLOG_LN_SZ + 1];
    char ln[KLOG_LN_SZ + 1], ref2[KLOG_TS_SZ];
    const char *sfx[] = { "0", "1", "head" };
    klog_t *kl = NULL;
    struct stat st;
    mode_t old;
    time_t now;
    FILE *fp;
    size_t i;

    for(i = 0; i < sizeof times / sizeof times[0]; ++i)
    {
        klog_timestamp(times[i], ts);
        ctime_ref(times[i], ref);
        CHECK(strcmp(ts, ref) == 0);
    }

    /* the memory log formats its lines with the same timestamp */
    CHECK(klog_new(KLOG_TYPE_MEM, KLOG_DEBUG, "unit", &kl) == 0);
    CHECK(kl && klog_open_mem(kl, 4) == 0);
    if(kl)
    {
        now = time(NULL);
        CHECK(klog(kl, KLOG_INFO, "hello %d", 1) == 0);
        CHECK(klog_getln(kl, 1, ln) == 0);
        ctime_ref(now, ref);
        ctime_ref(now + 1, ref2);
        snprintf(want, sizeof want, "[INF] %s <unit>: hello 1", ref);
        if(strcmp(ln, want) != 0)
            snprintf(want, sizeof want, "[INF] %s <unit>: hello 1", ref2);
        CHECK(strcmp(ln, want) == 0);
        klog_close(kl);
        kl = NULL;
    }

    /* the file log: three lines over two-line pages make page 1 too */
    CHECK(mkdtemp(dir) != NULL);
    snprintf(base, sizeof base, "%s/log", dir);
    old = umask(022);
    CHECK(klog_new(KLOG_TYPE_FILE, KLOG_DEBUG, "unit", &kl) == 0);
    CHECK(kl && klog_open_file(kl, base, 2, 2) == 0);
    if(kl)
    {
        for(i = 0; i < 3; ++i)
            CHECK(klog(kl, KLOG_INFO, "line %u", (unsigned) i) == 0);
        klog_close(kl);     /* writes log.head */
    }
    (void) umask(old);

    for(i = 0; i < sizeof sfx / sizeof sfx[0]; ++i)
    {
        snprintf(fn, sizeof fn, "%s.%s", base, sfx[i]);
        CHECK(stat(fn, &st) == 0 && (st.st_mode & 0777) == 0640);
    }

    snprintf(fn, sizeof fn, "%s.0", base);
    if((fp = fopen(fn, "r")) != NULL)
    {
        CHECK(fgets(ln, sizeof ln, fp) != NULL &&
                strncmp(ln, "[INF] ", 6) == 0 &&
                strlen(ln) > 6 + 24 && ln[6 + 24] == ' ' &&
                strcmp(ln + 6 + 24, " <unit>: line 0\n") == 0);
        fclose(fp);
    }
    else
        CHECK(!"log.0 opened");

    for(i = 0; i < sizeof sfx / sizeof sfx[0]; ++i)
    {
        snprintf(fn, sizeof fn, "%s.%s", base, sfx[i]);
        (void) unlink(fn);
    }
    (void) rmdir(dir);
}

struct vars_visit
{
    size_t count;
    size_t stop;
};

static int visit_var(var_t *v, void *arg)
{
    struct vars_visit *visit = arg;

    CHECK(var_get_name(v)[0] == (visit->count % 2 ? 'b' : 'a'));
    return ++visit->count == visit->stop;
}

static void test_large_vars(void)
{
    enum { N = 10000 };
    vars_t *vars = NULL;
    struct vars_visit visit = { 0, N + 1 };
    size_t i;
    char arg[32];
    const char *value;

    CHECK(vars_create(&vars) == 0);
    if(!vars)
        return;

    for(i = 0; i < N; ++i)
    {
        snprintf(arg, sizeof arg, "%c=%u", i % 2 ? 'b' : 'a', (unsigned)i);
        CHECK(vars_add_urlvar(vars, arg, NULL) == 0);
    }

    CHECK(vars_count(vars) == N);
    CHECK(vars_countn(vars, "A") == N / 2);
    value = vars_geti_value(vars, "A", N / 2 - 1);
    CHECK(value && !strcmp(value, "9998"));
    CHECK(vars_geti(vars, "a", N / 2) == NULL);
    vars_foreach(vars, visit_var, &visit);
    CHECK(visit.count == N);

    visit.count = 0;
    visit.stop = 7;
    vars_foreach(vars, visit_var, &visit);
    CHECK(visit.count == 7);
    vars_free(vars);
}

static int test_http_init(http_t *http, const char *max_args)
{
    vhost_t *vhost = NULL;

    if(u_config_create(&http->config) ||
            (max_args && u_config_add_key(http->config, "max_args", max_args)) ||
            vhost_list_create(&http->vhosts) || vhost_create(&vhost))
        return ~0;

    vhost->host = "localhost";
    vhost->config = http->config;
    return vhost_list_add(http->vhosts, vhost);
}

static void test_http_term(http_t *http)
{
    if(http->vhosts)
        vhost_list_free(http->vhosts);
    if(http->config)
        u_config_free(http->config);
}

static void check_request_args(http_t *http, const char *method,
        const char *query, const char *type, const char *body,
        int status, size_t get, size_t post, size_t uploads)
{
    request_t *rq = NULL;
    io_t *io = NULL;
    char headers[8192];
    int length;

    if(body)
        length = snprintf(headers, sizeof headers,
                "%s /?%s HTTP/1.1\r\nHost: localhost\r\nContent-Type: %s\r\n"
                "Content-Length: %zu\r\n\r\n%s", method, query, type,
                strlen(body), body);
    else
        length = snprintf(headers, sizeof headers,
                "%s /?%s HTTP/1.1\r\nHost: localhost\r\n\r\n", method, query);

    if(length < 0 || (size_t)length >= sizeof headers ||
            request_create(http, &rq) ||
            io_mem_create(headers, length, 0, &io))
    {
        CHECK(!"create request fixture");
        goto end;
    }
    request_bind(rq, io);
    if(request_parse_header(rq, NULL, NULL))
    {
        CHECK(!"parse request header");
        goto end;
    }
    CHECK(request_parse_data(rq) == status);
    CHECK(request_parse_data(rq) == status);
    CHECK(vars_count(request_get_args(rq)) == get + post);
    CHECK(vars_count(request_get_getargs(rq)) == get);
    CHECK(vars_count(request_get_postargs(rq)) == post);
    CHECK(vars_count(request_get_uploads(rq)) == uploads);
    CHECK(!timerm_armed());

end:
    if(rq)
        request_free(rq);
}

static void test_request_arg_limit(void)
{
    static const char urlencoded[] = "application/x-www-form-urlencoded";
    static const char multipart[] = "multipart/form-data; boundary=X";
    static const char form[] =
        "--X\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\nvalue\r\n"
        "--X\r\nContent-Disposition: form-data; name=\"u\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\nfile\r\n--X--\r\n";
    char many[3 * 1001 + 1];
    http_t http = { 0 }, limited = { 0 };
    size_t i;

    if(test_http_init(&http, NULL) || test_http_init(&limited, "2"))
    {
        CHECK(!"initialize request fixtures");
        goto end;
    }
    for(i = 0; i < 1001; ++i)
        memcpy(many + 3 * i, "a=&", 3);
    many[3 * 1001] = '\0';
    check_request_args(&http, "GET", many, NULL, NULL,
            HTTP_STATUS_BAD_REQUEST, 1000, 0, 0);
    check_request_args(&http, "POST", "", urlencoded, many,
            HTTP_STATUS_BAD_REQUEST, 0, 1000, 0);
    many[3 * 1000] = '\0';
    check_request_args(&http, "GET", many, NULL, NULL, 0, 1000, 0, 0);
    check_request_args(&http, "POST", "", urlencoded, many, 0, 0, 1000, 0);

    check_request_args(&limited, "POST", "a=1", urlencoded, "a=2",
            0, 1, 1, 0);
    check_request_args(&limited, "POST", "a=1", urlencoded, "a=2&a=3",
            HTTP_STATUS_BAD_REQUEST, 1, 1, 0);
    check_request_args(&limited, "POST", "", multipart, form, 0, 0, 1, 1);
    check_request_args(&limited, "POST", "a=1", multipart, form,
            HTTP_STATUS_BAD_REQUEST, 1, 1, 0);

end:
    test_http_term(&http);
    test_http_term(&limited);
}

static void check_large_form(http_t *http, const char *type,
    const char *body, size_t body_len, const char *value, size_t value_len)
{
    static const char query[] = "f=from+URI";
    char wire[20000];
    request_t *rq = NULL;
    io_t *io = NULL;
    vars_t *args, *post;
    var_t *v;
    const char *get;
    int n;

    n = snprintf(wire, sizeof wire, "POST /?%s HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Type: %s\r\nContent-Length: %zu\r\n\r\n", query, type,
            body_len);
    if(n < 0 || (size_t)n >= sizeof wire || body_len > sizeof wire - n ||
            request_create(http, &rq) ||
            io_mem_create(wire, n + body_len, 0, &io))
    {
        CHECK(!"create large form request");
        goto end;
    }
    memcpy(wire + n, body, body_len);
    request_bind(rq, io);
    if(request_parse_header(rq, NULL, NULL) || request_parse_data(rq))
    {
        CHECK(!"parse large form request");
        goto end;
    }
    CHECK(request_parse_data(rq) == 0);
    CHECK(request_get_query_string(rq) &&
            !strcmp(request_get_query_string(rq), query));
    get = vars_geti_value(request_get_getargs(rq), "f", 0);
    CHECK(get && !strcmp(get, "from URI"));

    args = request_get_args(rq);
    post = request_get_postargs(rq);
    CHECK(vars_count(args) == 2 && vars_count(post) == 1);
    v = vars_get(post, "f");
    CHECK(v && vars_geti(args, "f", 1) == v);
    CHECK(v && var_get_value_size(v) == value_len &&
            !memcmp(var_get_value(v), value, value_len) &&
            var_get_value(v)[value_len] == '\0');

end:
    if(rq)
        request_free(rq);
}

static void test_large_forms(void)
{
    static const char mp_head[] =
        "--xyz\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\n";
    static const char mp_tail[] = "\r\n--xyz--\r\n";
    enum { N = 9500 };
    char value[N], encoded[4 + N + 3], multipart[sizeof mp_head +
        N + sizeof mp_tail];
    http_t http = { 0 };

    if(test_http_init(&http, NULL))
    {
        CHECK(!"initialize large form fixture");
        goto end;
    }

    memset(value, 'x', sizeof value);
    value[0] = ' ';
    value[N - 1] = '&';
    memcpy(encoded, "%66=+", 5);
    memcpy(encoded + 5, value + 1, N - 2);
    memcpy(encoded + N + 3, "%26&", 4);
    check_large_form(&http, "application/x-www-form-urlencoded", encoded,
        sizeof encoded, value, sizeof value);

    /* The field spans several parser buffers and includes a binary NUL. */
    value[4096] = '\0';
    memcpy(multipart, mp_head, sizeof mp_head - 1);
    memcpy(multipart + sizeof mp_head - 1, value, sizeof value);
    memcpy(multipart + sizeof mp_head - 1 + sizeof value,
        mp_tail, sizeof mp_tail - 1);
    check_large_form(&http, "multipart/form-data; boundary=xyz", multipart,
        sizeof multipart - 2, value, sizeof value);

end:
    test_http_term(&http);
}

static void small_page(dypage_args_t *args)
{
    CHECK(io_write(response_io(args->rs), "x", 1) == 1);
}

static void replace_page_filter(dypage_args_t *args)
{
    codec_t *spare = NULL, *replacement = NULL;
    io_t *out = response_io(args->rs);

    /* Fill the spare slot so discarding the original filter frees it. */
    CHECK(response_filter_create(args->rq, args->rs, NULL, &spare) == 0);
    if(spare == NULL)
        return;
    codec_free(spare);
    CHECK(io_codecs_discard(out) == 0);

    CHECK(response_filter_create(args->rq, args->rs, args->ss,
                &replacement) == 0);
    if(replacement == NULL)
        return;
    CHECK(io_codec_add_tail(out, replacement) == 0);
    small_page(args);
}

static void test_output_after_dynamic_page(int replace)
{
    http_t http = { 0 };
    session_opt_t so = { .type = SESSION_TYPE_MEMORY, .name = "klin_sid" };
    dypage_args_t args = { .fun = replace ? replace_page_filter : small_page };
    io_t *out = NULL;
    char body[RFBUFSZ], wire[RFBUFSZ + 1024];
    ssize_t len;

    http.sess_opt = &so;
    if(test_http_init(&http, NULL) || request_create(&http, &args.rq) ||
            response_create(&http, &args.rs) ||
            io_mem_create(wire, sizeof wire, 0, &out))
    {
        CHECK(!"create dynamic page fixture");
        goto end;
    }
    response_bind(args.rs, out);
    CHECK(dypage_serve(&args) == 0 && args.ss == NULL);
    CHECK(io_has_codecs(out) && !response_header_sent(args.rs));

    /* This is where the request hook writes in http_serve_request(). */
    memset(body, 'y', sizeof body);
    CHECK(io_write(out, body, sizeof body) == sizeof body);
    CHECK(response_header_sent(args.rs));
    CHECK(io_flush(out) == 0);
    len = io_tell(out);
    CHECK(len > (ssize_t) sizeof body &&
            !memcmp(wire + len - sizeof body, body, sizeof body));

end:
    if(args.rq)
        request_free(args.rq);
    if(args.rs)
        response_free(args.rs);
    test_http_term(&http);
}

#ifdef SSL_OPENSSL
static void test_client_cookie_staging(void)
{
    http_t http = { 0 };
    session_opt_t so = { .type = SESSION_TYPE_CLIENT, .name = "invalid sid" };
    request_t *rq = NULL;
    response_t *rs = NULL;
    session_t *ss = NULL;
    header_t *h;
    char input[1], key[CIPHER_KEY_LEN] = { 0 }, out[CIPHER_KEY_LEN + 1];
    size_t size = sizeof out;
    io_t *in = NULL;

    if(test_http_init(&http, NULL) || request_create(&http, &rq) ||
            response_create(&http, &rs) ||
            io_mem_create(input, sizeof input, 0, &in))
    {
        CHECK(!"create client session fixture");
        goto end;
    }
    request_bind(rq, in);
    if(session_client_create(&so, rq, rs, &ss))
    {
        CHECK(!"create client session");
        goto end;
    }
    h = response_get_header(rs);
    CHECK(session_set_cipher_key(ss, key, sizeof key) == 0);
    CHECK(session_set(ss, "v", "a") == 0);
    /* The id field fails after the data and mtime have been prepared. */
    CHECK(session_save(ss) != 0 && header_count_field(h, "Set-Cookie") == 0);
    CHECK(session_get_cipher_key(ss, out, &size) == 0 &&
            size == sizeof key && !memcmp(out, key, size));
    CHECK(session_remove(ss) != 0 && !ss->removed);
    CHECK(header_count_field(h, "Set-Cookie") == 0);
    strcpy(so.name, "klin_sid");
    /* Commit must neither report success nor save in place of removal. */
    CHECK(session_prv_commit(ss) == 0 && ss->committed && ss->sent == 0);
    CHECK(header_count_field(h, "Set-Cookie") == 0);
    CHECK(session_save(ss) == 0 && header_count_field(h, "Set-Cookie") == 3);

end:
    if(ss)
    {
        ss->removed = 1;
        session_free(ss);
    }
    if(rq)
        request_free(rq);
    if(rs)
        response_free(rs);
    test_http_term(&http);
}
#endif

static int save_test_session(session_t *ss, const char *id, const char *value)
{
    strcpy(ss->filename, id);
    if(session_set(ss, "v", value))
        return ~0;
    return ss->save(ss);
}

static void test_memory_session_eviction(void)
{
    http_t http = { 0 };
    session_opt_t so = { 0 };
    request_t *rq = NULL;
    response_t *rs = NULL;
    session_t *ss = NULL;
    atom_t *atom;
    size_t small;

    if(test_http_init(&http, NULL) || request_create(&http, &rq) ||
            response_create(&http, &rs) || atoms_create(&so.atoms) ||
            session_mem_create(&so, rq, rs, &ss))
    {
        CHECK(!"initialize memory session fixture");
        goto end;
    }
    so.max_count = 2;
    CHECK(save_test_session(ss, "a", "x") == 0);
    if(atoms_get(so.atoms, "a", &atom))
    {
        CHECK(!"find first saved session");
        goto end;
    }
    atom->arg = (void *)1;
    small = atom->size;
    CHECK(small > 0);
    CHECK(save_test_session(ss, "b", "x") == 0);

    /* Replacing the oldest at the count limit keeps the other session. */
    CHECK(save_test_session(ss, "a", "y") == 0);
    CHECK(atoms_count(so.atoms) == 2 && atoms_size(so.atoms) == 2 * small);
    CHECK(atoms_get(so.atoms, "b", &atom) == 0);
    if(atoms_get(so.atoms, "a", &atom))
    {
        CHECK(!"find replaced session");
        goto end;
    }
    atom->arg = (void *)1;
    CHECK(save_test_session(ss, "c", "x") == 0);
    CHECK(atoms_get(so.atoms, "a", &atom) != 0);
    CHECK(atoms_get(so.atoms, "c", &atom) == 0);
    CHECK(atoms_count(so.atoms) == 2 && atoms_size(so.atoms) == 2 * small);

    so.max_count = 0;
    so.mem_limit = 2 * small;
    if(atoms_get(so.atoms, "b", &atom))
    {
        CHECK(!"find session to enlarge at the memory limit");
        goto end;
    }
    atom->arg = (void *)1;
    CHECK(save_test_session(ss, "b", "xxx") == 0);
    CHECK(atoms_get(so.atoms, "c", &atom) != 0);
    CHECK(atoms_get(so.atoms, "b", &atom) == 0);
    CHECK(atoms_count(so.atoms) == 1 && atoms_size(so.atoms) == small + 2);

    CHECK(save_test_session(ss, "d", "x") == 0);
    CHECK(atoms_get(so.atoms, "b", &atom) != 0);
    CHECK(atoms_get(so.atoms, "d", &atom) == 0);
    CHECK(atoms_count(so.atoms) == 1 && atoms_size(so.atoms) == small);
    CHECK(save_test_session(ss, "d", "too-large-for-the-session-store") != 0);
    CHECK(atoms_get(so.atoms, "d", &atom) == 0);
    CHECK(atoms_count(so.atoms) == 1 && atoms_size(so.atoms) == small);

    /* Idle sessions that are never requested again are swept on a save. */
    so.mem_limit = 0;
    so.max_age = 120;
    if(atoms_get(so.atoms, "d", &atom))
    {
        CHECK(!"find session to age");
        goto end;
    }
    atom->arg = (void *)(time(NULL) - 121);
    CHECK(save_test_session(ss, "live", "x") == 0);
    CHECK(atoms_get(so.atoms, "d", &atom) != 0);
    CHECK(atoms_count(so.atoms) == 1 && atoms_size(so.atoms) == small);
    CHECK(save_test_session(ss, "new", "x") == 0);
    CHECK(atoms_get(so.atoms, "live", &atom) == 0);
    CHECK(atoms_count(so.atoms) == 2 && atoms_size(so.atoms) == 2 * small);

end:
    if(ss)
    {
        ss->removed = 1;
        session_free(ss);
    }
    if(so.atoms)
        atoms_free(so.atoms);
    if(rq)
        request_free(rq);
    if(rs)
        response_free(rs);
    test_http_term(&http);
}

/* Run the store end of a worker's PPC connection in a separate process. */
static int serve_test_session_ppc(ppc_t *ppc, int fd)
{
    char data[PPC_MAX_DATA_SIZE];
    unsigned char cmd;
    ssize_t n;

    while((n = ppc_read(ppc, fd, &cmd, data, sizeof data)) > 0)
    {
        if(ppc_dispatch(ppc, fd, cmd, data, n))
            return EXIT_FAILURE;
    }
    return n == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void test_memory_session_ppc(void)
{
    http_t http = { 0 };
    server_t server = { 0 };
    backend_t backend = { 0 };
    session_opt_t so = { 0 };
    request_t *rq = NULL;
    response_t *rs = NULL;
    session_t *ss = NULL;
    int pair[2] = { -1, -1 }, status;
    pid_t pid = -1;
    const char *loaded;
    char large[6400], oversized[PPC_MAX_DATA_SIZE + 1];
    size_t stale_size;

    ctx->server = &server;
    if(test_http_init(&http, NULL) || request_create(&http, &rq) ||
            response_create(&http, &rs) || ppc_create(&server.ppc) ||
            session_mem_module_init(NULL, &so) ||
            session_mem_create(&so, rq, rs, &ss) ||
            save_test_session(ss, "id", "inherited") ||
            socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
    {
        CHECK(!"initialize memory session PPC fixture");
        goto end;
    }

    if((pid = fork()) < 0)
    {
        CHECK(!"fork memory session PPC peer");
        goto end;
    }
    if(pid == 0)
    {
        close(pair[0]);
        _exit(serve_test_session_ppc(server.ppc, pair[1]));
    }
    close(pair[1]);
    pair[1] = -1;
    ctx->pipc = pair[0];
    ctx->backend = &backend;
    backend.model = SERVER_MODEL_PREFORK;
    stale_size = atoms_size(so.atoms);

    memset(large, 'x', sizeof large - 1);
    large[sizeof large - 1] = '\0';
    CHECK(save_test_session(ss, "id", large) == 0);
    CHECK(atoms_count(so.atoms) == 1 && atoms_size(so.atoms) == stale_size);

    /* An oversized save leaves the previous record and PPC stream intact. */
    memset(oversized, 'y', sizeof oversized - 1);
    oversized[sizeof oversized - 1] = '\0';
    CHECK(save_test_session(ss, "id", oversized) != 0);
    CHECK(session_clean(ss) == 0);
    CHECK(ss->load(ss) == 0);
    loaded = session_get(ss, "v");
    CHECK(loaded && !strcmp(loaded, large));
    CHECK(ss->remove(ss) == 0);
    CHECK(atoms_count(so.atoms) == 1 && atoms_size(so.atoms) == stale_size);
    CHECK(session_clean(ss) == 0);
    CHECK(ss->load(ss) != 0);

    /* Fork workers still read and mutate their local copy. */
    backend.model = SERVER_MODEL_FORK;
    CHECK(save_test_session(ss, "id", "fork value") == 0);
    CHECK(session_clean(ss) == 0);
    CHECK(ss->load(ss) == 0);
    loaded = session_get(ss, "v");
    CHECK(loaded && !strcmp(loaded, "fork value"));
    CHECK(ss->remove(ss) == 0);
    CHECK(atoms_count(so.atoms) == 0);

end:
    ctx->pipc = 0;
    ctx->backend = NULL;
    ctx->server = NULL;
    if(pair[0] >= 0)
        close(pair[0]);
    if(pair[1] >= 0)
        close(pair[1]);
    if(pid > 0)
        CHECK(waitpid(pid, &status, 0) == pid &&
                WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
    if(ss)
    {
        ss->removed = 1;
        session_free(ss);
    }
    if(so.atoms)
        atoms_free(so.atoms);
    if(server.ppc)
        ppc_free(server.ppc);
    if(rq)
        request_free(rq);
    if(rs)
        response_free(rs);
    test_http_term(&http);
}

static void test_embedded_resources(void)
{
    enum { PAGES = 64 };
    embres_t pages[PAGES] = {0}, *res = NULL;
    char names[PAGES][16];
    int seen[PAGES] = {0};
    char **list = NULL;
    u_buf_t *buf = NULL;
    io_t *io = NULL;
    unsigned char contents[4096] = "embedded test";
    embfile_t file = {
        .res = { .filename = "/file", .type = ET_FILE },
        .size = sizeof(contents), .data = contents, .file_size = sizeof(contents)
    };
    int i, index;
#ifdef HAVE_LIBZ
    unsigned char compressed[sizeof(contents) + 128];
    unsigned long compressed_size = sizeof(compressed);
#endif

    CHECK(emb_init() == 0);
    CHECK(emb_list(&list) != 0 && list == NULL);
    for(i = 0; i < PAGES; i++)
    {
        snprintf(names[i], sizeof(names[i]), "/page%d", i);
        pages[i].filename = names[i];
        pages[i].type = ET_PAGE;
        CHECK(emb_register(&pages[i]) == 0);
    }
    CHECK(emb_register(&pages[0]) != 0);
    for(i = 0; i < PAGES; i++)
        CHECK(emb_lookup(names[i], &res) == 0 && res == &pages[i]);
    CHECK(emb_open(names[0], &io) != 0 && io == NULL);
    CHECK(emb_list(&list) == 0 && list != NULL);
    if(list)
    {
        for(i = 0; i < PAGES && list[i]; i++)
        {
            index = -1;
            CHECK(sscanf(list[i], "/page%d", &index) == 1);
            CHECK(index >= 0 && index < PAGES);
            if(index >= 0 && index < PAGES)
            {
                CHECK(seen[index]++ == 0);
                CHECK(list[i] != names[index] && !strcmp(list[i], names[index]));
            }
        }
        CHECK(i == PAGES && list[i] == NULL);
        emb_list_free(list);
    }
    for(i = 0; i < PAGES; i++)
        CHECK(emb_unregister(&pages[i]) == 0);
    CHECK(emb_lookup(names[0], &res) != 0);
    CHECK(emb_unregister(&pages[0]) != 0);

    CHECK(emb_register(&file.res) == 0);
    CHECK(emb_to_ubuf(file.res.filename, &buf) == 0 && buf != NULL);
    if(buf)
    {
        CHECK(u_buf_size(buf) == sizeof(contents));
        CHECK(u_buf_len(buf) == sizeof(contents)
                && !memcmp(u_buf_ptr(buf), contents, sizeof(contents)));
        u_buf_free(buf);
        buf = NULL;
    }
#ifdef HAVE_LIBZ
    CHECK(compress(compressed, &compressed_size, contents, sizeof(contents)) == Z_OK);
    file.comp = EMB_COMP_ZLIB;
    file.size = compressed_size;
    file.data = compressed;
    CHECK(emb_to_ubuf(file.res.filename, &buf) == 0 && buf != NULL);
    if(buf)
    {
        CHECK(u_buf_size(buf) == sizeof(contents));
        CHECK(u_buf_len(buf) == sizeof(contents)
                && !memcmp(u_buf_ptr(buf), contents, sizeof(contents)));
        u_buf_free(buf);
    }
#endif
    CHECK(emb_unregister(&file.res) == 0);
    CHECK(emb_term() == 0);
}

/* "0" parses whatever errno an earlier call left (LibU tested errno after
   strtol(3) without clearing it: a setting of 0 kept its default) */
static void test_atoi_zero(void)
{
    struct { const char *s; int v; } ok[] = { { "0", 0 }, { "214", 214 },
        { "-1", -1 } };
    size_t i;
    int v;

    for(i = 0; i < sizeof ok / sizeof ok[0]; ++i)
    {
        errno = EINVAL;
        v = 12345;
        CHECK(u_atoi(ok[i].s, &v) == 0 && v == ok[i].v);
    }
    errno = EINVAL;
    CHECK(u_atoi("x", &v) != 0);
}

int main(void)
{
    test_io_read_codecs();
    test_atoi_zero();
#ifdef HAVE_LIBZ
    test_deflate_windows();
#endif
    test_header_fields();
    test_header_limits();
    test_large_vars();
    test_request_arg_limit();
    test_large_forms();
    test_output_after_dynamic_page(0);
    test_output_after_dynamic_page(1);
#ifdef SSL_OPENSSL
    test_client_cookie_staging();
#endif
    test_memory_session_eviction();
    test_memory_session_ppc();
    test_field_reuse();
    test_timer();
    test_timed_fd_write();
    test_alarm_during_write();
    test_embedded_resources();
    test_translate();
    test_json_parse();
#ifdef SSL_OPENSSL
    test_crypto();
#endif
    test_json_write();
    test_klog();
#ifdef SSL_OPENSSL
    test_auth();
#endif

    printf("%d unit checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
