/*
 * Copyright (c) 2005-2012 by KoanLogic s.r.l. <http://www.koanlogic.com>
 * Copyright (c) 2026 Daniel Tillett
 * All rights reserved.
 *
 * This file is part of Klin (derived from KLone), and as such it is subject to the license stated
 * in the LICENSE file which you have received as part of this distribution.
 */

#include "klin_conf.h"
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <strings.h>
#ifdef SSL_ON
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#ifdef SSL_OPENSSL
#include <openssl/x509_vfy.h>
#endif
#include <u/libu.h>
#include <klin/tls.h>
#include <klin/utils.h>
#include <klin/tlsprv.h>
#include <klin/crypto.h>

/* TLS 1.2 ciphers: Mozilla's "intermediate" configuration (forward secret
   key exchange, AEAD ciphers only); doc/TLS */
static const char TLS_DEFAULT_CIPHERS[] =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
    "DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384:"
    "DHE-RSA-CHACHA20-POLY1305";

enum { TLS_TICKET_PERIOD = 12 * 60 * 60 };  /* a ticket key's 12 hours */

static int tls_sid = 1;
static int tls_inited = 0; 

/* private methods */
static int tls_context (SSL_CTX **);
static int tls_load_x509_creds (SSL_CTX *, tls_ctx_args_t *);
static int tls_sid_context (SSL_CTX *, int *);
#ifdef SSL_OPENSSL
static int tls_gendh_params (SSL_CTX *, const char *);
static EVP_PKEY *tls_load_dh_param (const char *);
#endif
static int tls_no_passphrase_cb (char *, int, int, void *);
static int tls_init_ctx_args (tls_ctx_args_t *);
static int tls_set_ctx_vdepth (u_config_t *, tls_ctx_args_t *);
static int tls_set_ctx_crlopts (u_config_t *, tls_ctx_args_t *);
static int tls_set_ctx_vmode (u_config_t *, tls_ctx_args_t *);
static int tls_check_ctx (tls_ctx_args_t *);
static void tls_free_ctx_args (tls_ctx_args_t *cargs);
static int tls_load_ctx_args (u_config_t *cfg, tls_ctx_args_t **cargs);
static SSL_CTX *tls_init_ctx (tls_ctx_args_t *cargs);
static int cb_vfy (int ok, X509_STORE_CTX *store_ctx);

/* tls_ciphers, tls_ciphersuites, tls_min_version (1.2 or 1.3) and
   tls_ticket_key_lifetime (minutes, or "Ns" seconds; 0: no tickets) */
static int tls_load_protocol_args (u_config_t *cfg, tls_ctx_args_t *cargs)
{
    const char *v;
    char *end;
    long n;

    if ((v = u_config_get_subkey_value(cfg, "tls_ciphers")) != NULL)
        cargs->ciphers = v;
    cargs->suites = u_config_get_subkey_value(cfg, "tls_ciphersuites");

    if ((v = u_config_get_subkey_value(cfg, "tls_min_version")) != NULL)
    {
        if (!strcmp(v, "1.2"))
            cargs->minver = TLS1_2_VERSION;
        else if (!strcmp(v, "1.3"))
            cargs->minver = TLS1_3_VERSION;
        else
            return ~0;
    }

    if ((v = u_config_get_subkey_value(cfg, "tls_ticket_key_lifetime")) != NULL)
    {
        n = strtol(v, &end, 10);
        if (end == v || n < 0 || n > 60L * 24 * 31)
            return ~0;
        if (*end == 's' || *end == 'S')
            ++end;
        else
            n *= 60;
        if (*end != '\0')
            return ~0;
        cargs->ticket_period = (int) n;
    }

    return 0;
}

/* Session ticket keys. Each HTTPS server draws a random secret when its
 * context is made, before the workers fork; every period (the epoch, the
 * time divided by tls_ticket_key_lifetime) the secret is replaced by an
 * HMAC of itself, and the old one erased. The period's keys are derived
 * from its secret. Every process steps its copy forward on its own, so
 * all have the same keys without talking to each other; and a process
 * whose memory is read gives away the current and previous periods' keys
 * (and later ones), not those of earlier periods: their tickets stay
 * closed. A ticket of the period before is still taken (and replaced by a
 * new one); older ones mean a full handshake. */
typedef struct
{
    int valid;
    unsigned char name[16], aes[32], mac[32];
} tls_ticket_key_t;

typedef struct tls_ticket_state_s
{
    long long period;           /* seconds                               */
    long long epoch;            /* the period 'secret' is of             */
    unsigned char secret[32];
    tls_ticket_key_t cur, prev; /* keys of 'epoch' and of 'epoch' - 1     */
    const void *owner;          /* the server's configuration             */
    int erased;                 /* not this process's: no keys            */
    struct tls_ticket_state_s *next;
} tls_ticket_state_t;

/* the most periods one call steps forward: a clock moved far ahead does
   not stall klind (the rest is done by later calls) */
enum { TLS_TICKET_MAX_STEPS = 1 << 16 };

static tls_ticket_state_t *tls_ticket_states = NULL;   /* every server's */
static int tls_ticket_idx = -1;     /* SSL_CTX ex data: its state        */

/* 'out' = HMAC-SHA256('secret', 'label') (32 bytes) */
static int tls_ticket_hmac (const unsigned char secret[32], const char *label,
        unsigned char *out, size_t len)
{
    unsigned char md[KC_MD_MAX];
    size_t n = sizeof md;

    dbg_err_if (kc_hmac(KC_SHA256, secret, 32, (const unsigned char *) label,
                strlen(label), md, &n) || n < len);
    memcpy(out, md, len);
    kc_cleanse(md, sizeof md);

    return 0;
err:
    kc_cleanse(md, sizeof md);
    return ~0;
}

/* the key name, AES key and HMAC key of the period of 'secret' */
static int tls_ticket_keys (const unsigned char secret[32],
        tls_ticket_key_t *k)
{
    dbg_err_if (tls_ticket_hmac(secret, "klin-ticket|name", k->name,
                sizeof k->name));
    dbg_err_if (tls_ticket_hmac(secret, "klin-ticket|aes", k->aes,
                sizeof k->aes));
    dbg_err_if (tls_ticket_hmac(secret, "klin-ticket|mac", k->mac,
                sizeof k->mac));
    k->valid = 1;

    return 0;
err:
    kc_cleanse(k, sizeof *k);
    return ~0;
}

/* step 'st' forward to the period of now, at most TLS_TICKET_MAX_STEPS
   periods (never back: a clock set back keeps the current keys, and
   processes that had stepped further than others keep theirs, so tickets
   between them need a full handshake until the clock catches up). The new
   secret and keys are made aside and kept only if all of it worked */
static int tls_ticket_advance (tls_ticket_state_t *st)
{
    long long epoch = (long long) time(NULL) / st->period;
    unsigned char next[32];
    tls_ticket_key_t k;
    int steps;

    dbg_return_if (st->erased, ~0);

    for (steps = 0; st->epoch < epoch && steps < TLS_TICKET_MAX_STEPS; ++steps)
    {
        dbg_err_if (tls_ticket_hmac(st->secret, "klin-ticket|next", next,
                    sizeof next));
        dbg_err_if (tls_ticket_keys(next, &k));
        memcpy(st->secret, next, sizeof next);
        st->prev = st->cur;
        st->cur = k;
        st->epoch++;
    }
    kc_cleanse(next, sizeof next);
    kc_cleanse(&k, sizeof k);

    return 0;
err:
    kc_cleanse(next, sizeof next);
    kc_cleanse(&k, sizeof k);
    return ~0;
}

/* erase a ticket state: this process does not serve it */
static void tls_ticket_erase (tls_ticket_state_t *st)
{
    kc_cleanse(st->secret, sizeof st->secret);
    kc_cleanse(&st->cur, sizeof st->cur);
    kc_cleanse(&st->prev, sizeof st->prev);
    st->erased = 1;
}

/**
 * \brief   Step every server's session ticket keys forward to the current
 *          period, erasing the earlier secrets (klind calls it every
 *          second, so that its workers, forked from it, start with current
 *          keys only; a worker, when it accepts a connection)
 */
void tls_tickets_advance (void)
{
    tls_ticket_state_t *st;

    for (st = tls_ticket_states; st != NULL; st = st->next)
        if (!st->erased)
            (void) tls_ticket_advance(st);
}

/**
 * \brief   Erase the session ticket secrets of every server but the one
 *          whose configuration is \p owner (NULL: all of them): a worker
 *          keeps its own server's only, a key manager none
 */
void tls_tickets_keep (const void *owner)
{
    tls_ticket_state_t *st;

    for (st = tls_ticket_states; st != NULL; st = st->next)
        if (owner == NULL || st->owner != owner)
            tls_ticket_erase(st);
}

static int tls_ticket_cb (SSL *s, unsigned char key_name[16],
        unsigned char *iv, EVP_CIPHER_CTX *ectx, EVP_MAC_CTX *hctx, int enc)
{
    tls_ticket_state_t *st;
    tls_ticket_key_t *k;
    OSSL_PARAM params[3];

    st = SSL_CTX_get_ex_data(SSL_get_SSL_CTX(s), tls_ticket_idx);
    dbg_return_if (st == NULL || tls_ticket_advance(st), -1);
    if (!st->cur.valid)
        return 0;   /* no key: no ticket, and a full handshake */

    if (enc)
    {
        k = &st->cur;
        memcpy(key_name, k->name, sizeof k->name);
        dbg_return_if (RAND_bytes(iv, EVP_CIPHER_get_iv_length(
                        EVP_aes_256_cbc())) != 1, -1);
    }
    else if (!memcmp(key_name, st->cur.name, sizeof st->cur.name))
        k = &st->cur;
    else if (st->prev.valid &&
            !memcmp(key_name, st->prev.name, sizeof st->prev.name))
        k = &st->prev;
    else
        return 0;   /* unknown or expired key: a full handshake */

    params[0] = OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_KEY,
            k->mac, sizeof k->mac);
    params[1] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
            (char *) "SHA256", 0);
    params[2] = OSSL_PARAM_construct_end();
    dbg_return_if (EVP_MAC_CTX_set_params(hctx, params) != 1, -1);
    dbg_return_if (EVP_CipherInit_ex(ectx, EVP_aes_256_cbc(), NULL, k->aes,
                iv, enc) != 1, -1);

    /* 1: go on; 2: take it, and issue a ticket with the current key */
    return (enc || k == &st->cur) ? 1 : 2;
}

static int tls_set_protocol (SSL_CTX *c, tls_ctx_args_t *cargs)
{
    char ciphers[1024];
    tls_ticket_state_t *st = NULL;

    dbg_err_if (SSL_CTX_set_min_proto_version(c, cargs->minver) != 1);

    /* RSA key exchange (TLS 1.2) decrypts with the private key, which the
       key manager does not do; it has no forward secrecy anyway */
    dbg_err_if (u_snprintf(ciphers, sizeof ciphers, "%s%s", cargs->ciphers,
                cargs->keymgr ? ":!kRSA" : ""));
    crit_err_ifm (SSL_CTX_set_cipher_list(c, ciphers) != 1,
            "tls_ciphers: no usable cipher in '%s'", cargs->ciphers);
    if (cargs->suites)
        crit_err_ifm (SSL_CTX_set_ciphersuites(c, cargs->suites) != 1,
                "tls_ciphersuites: bad list '%s'", cargs->suites);

    if (cargs->ticket_period == 0)
    {
        /* no tickets: TLS 1.2 session ids only, none for TLS 1.3 */
        SSL_CTX_set_options(c, SSL_OP_NO_TICKET);
        dbg_err_if (SSL_CTX_set_num_tickets(c, 0) != 1);
        return 0;
    }

    /* this server's ticket secret (kept for klind's lifetime, as is the
       context), drawn before the workers fork */
    dbg_err_if ((st = OPENSSL_secure_zalloc(sizeof *st)) == NULL);
    st->period = cargs->ticket_period;
    st->epoch = (long long) time(NULL) / st->period;
    dbg_err_if (RAND_priv_bytes(st->secret, sizeof st->secret) != 1);
    dbg_err_if (tls_ticket_keys(st->secret, &st->cur));

    if (tls_ticket_idx < 0)
        dbg_err_if ((tls_ticket_idx = SSL_CTX_get_ex_new_index(0, NULL,
                        NULL, NULL, NULL)) < 0);
    dbg_err_if (SSL_CTX_set_ex_data(c, tls_ticket_idx, st) != 1);
    dbg_err_if (SSL_CTX_set_tlsext_ticket_key_evp_cb(c, tls_ticket_cb) != 1);
    st->next = tls_ticket_states;
    tls_ticket_states = st;

    /* a ticket is taken in the period it was made and the next one: the
       key decides (the session's own timeout is longer, so it does not) */
    (void) SSL_CTX_set_timeout(c, 3 * cargs->ticket_period);

    return 0;
err:
    if (st != NULL && st != tls_ticket_states)
        OPENSSL_secure_clear_free(st, sizeof *st);
    return ~0;
}

/* the tls_ctx_t of klin/tls.h is an OpenSSL SSL_CTX */
tls_ctx_t *tls_load_init_ctx (u_config_t *cfg)
{
    SSL_CTX *ctx = NULL;
    tls_ctx_args_t *cargs = NULL;
    tls_ticket_state_t *st;

    dbg_return_if (cfg == NULL, NULL);

    dbg_err_if (tls_load_ctx_args(cfg, &cargs));
    dbg_err_if ((ctx = tls_init_ctx(cargs)) == NULL);

    /* its ticket secret belongs to this server (tls_tickets_keep) */
    if (tls_ticket_idx >= 0 && (st = SSL_CTX_get_ex_data(ctx,
                    tls_ticket_idx)) != NULL)
        st->owner = cfg;

    tls_free_ctx_args(cargs);

    return (tls_ctx_t *) ctx;
err:
    if (cargs)
        tls_free_ctx_args(cargs);
    if (ctx)
        SSL_CTX_free(ctx);
    return NULL;
}

/* load SSL_CTX args from configuration */
static int tls_load_ctx_args (u_config_t *cfg, tls_ctx_args_t **pcargs)
{
    tls_ctx_args_t *cargs = NULL;

    dbg_return_if (cfg == NULL, ~0);
    dbg_return_if (pcargs == NULL, ~0);

    cargs = u_zalloc(sizeof(tls_ctx_args_t));
    dbg_err_if (cargs == NULL);

    (void) tls_init_ctx_args(cargs);

    cargs->cert = u_config_get_subkey_value(cfg, "cert_file");
    cargs->key = u_config_get_subkey_value(cfg, "key_file");
    cargs->certchain = u_config_get_subkey_value(cfg, "certchain_file");
    cargs->ca = u_config_get_subkey_value(cfg, "ca_file");
    cargs->dh = u_config_get_subkey_value(cfg, "dh_file");
    cargs->crl = u_config_get_subkey_value(cfg, "crl_file");
    /* the private key in a separate process (doc/KEYMGR), by default;
       Windows has no key manager (it needs fork(2) and UNIX sockets) */
#ifdef OS_WIN
    dbg_err_if (u_config_get_subkey_value_b(cfg, "key_manager", 0,
                &cargs->keymgr));
#else
    dbg_err_if (u_config_get_subkey_value_b(cfg, "key_manager", 1,
                &cargs->keymgr));
#endif
    dbg_err_if (u_config_get_subkey_value_b(cfg, "sandbox", 1,
                &cargs->sandbox));
    dbg_err_if (tls_set_ctx_crlopts(cfg, cargs));
    dbg_err_if (tls_set_ctx_vdepth(cfg, cargs));
    dbg_err_if (tls_set_ctx_vmode(cfg, cargs));
    crit_err_ifm (tls_load_protocol_args(cfg, cargs),
            "bad tls_min_version or tls_ticket_key_lifetime");

    /* check cargs consistency against the supplied values */
    crit_err_ifm (tls_check_ctx(cargs), 
            "error validating SSL configuration options");

    *pcargs = cargs;

    return 0;
err:
    if (cargs)
        tls_free_ctx_args(cargs);
    return ~0;
}

/* initialize 'parent' SSL context */
static SSL_CTX *tls_init_ctx (tls_ctx_args_t *cargs)
{
    SSL_CTX *c = NULL;

    dbg_return_if (cargs == NULL, NULL);

    /* global init */
    dbg_err_if (tls_init());

    /* create SSL CTX from where all the SSL sessions will be cloned */
    dbg_err_if (tls_context(&c));

    /* don't ask for unlocking passphrases: this assumes that all 
     * credentials are stored in clear text */
    SSL_CTX_set_default_passwd_cb(c, tls_no_passphrase_cb);

    /* NOTE: configuration has been sanitized earlier by tls_check_ctx,
     * so X.509 credentials are in place. */

    if (cargs->cert)
        /* set key and certs against the SSL context */
        dbg_err_if (tls_load_x509_creds(c, cargs));

#ifdef SSL_OPENSSL
    /* DH parameters: chosen by OpenSSL, or loaded from dh_file */
    dbg_err_if (tls_gendh_params(c, cargs->dh));
#endif

    /* protocol versions, ciphers and session tickets */
    dbg_err_if (tls_set_protocol(c, cargs));

    /* set the session id context */
    dbg_err_if (tls_sid_context(c, &tls_sid));

    return c;
err:
    if (c)
        SSL_CTX_free(c);
    return NULL;
}

static int tls_sid_context (SSL_CTX *c, int *sid)
{
    int rc;

    dbg_return_if (c == NULL, ~0); 
    dbg_return_if (sid == NULL, ~0); 
    
    /* every time tls_init_ctx() is called, move on the session id context */
    (*sid)++;

    rc = SSL_CTX_set_session_id_context(c, (void *) sid, sizeof(int));
    dbg_err_ifm (rc == 0, "error setting sid: %s", tls_get_error());
    
    return 0;
err:
    return ~0;
}

static int tls_context (SSL_CTX **pc)
{
    SSL_CTX *c = NULL;

    dbg_return_if (pc == NULL, ~0);

    c = SSL_CTX_new(TLS_server_method());
    dbg_err_ifm (c == NULL, "error creating SSL CTX: %s", tls_get_error());
    dbg_err_if (SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION) != 1);

    /* no compression (CRIME); no renegotiation, which TLS 1.3 dropped and
       TLS 1.2 clients have no need of; the server's cipher order, so the
       best cipher both sides have is used */
    SSL_CTX_set_options(c, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION |
            SSL_OP_CIPHER_SERVER_PREFERENCE);

    /* free the read/write buffers (~34 KB) of a connection while it is idle,
       e.g. between requests on a kept-alive connection */
    (void) SSL_CTX_set_mode(c, SSL_MODE_RELEASE_BUFFERS);

    *pc = c;

    return 0;
err:
    if (c)
        SSL_CTX_free(c);
    return ~0;
}

/* XXX very primitive */
char *tls_get_error (void)
{
    unsigned long e = ERR_get_error();
    return ERR_error_string(e, NULL);
}

/* if skey is NULL, assume private key in scert, ca can be NULL */
static int tls_load_x509_creds (SSL_CTX *c, tls_ctx_args_t *cargs)
{
    dbg_return_if (c == NULL, ~0);
    dbg_return_if (cargs == NULL, ~0);
    dbg_return_if (cargs->cert == NULL, ~0);

    /* if key file unspecified assume key+cert are bundled */
    if (!cargs->key)
        cargs->key = cargs->cert;
    
    /* set ca if supplied */
    if (cargs->ca)
        crit_err_ifm(tls_load_verify_locations(c, cargs->ca),
                "error loading CA certificate from %s", cargs->ca);

#ifdef SSL_OPENSSL
    /* explicitly set the list of CAs for which we accept certificates */
    if (cargs->ca && cargs->vmode != SSL_VERIFY_NONE)
        SSL_CTX_set_client_CA_list(c, tls_load_client_CA_file(cargs->ca));
#endif

    /* load server certificate */
    crit_err_ifm (tls_use_certificate_file(c, cargs->cert, 
                SSL_FILETYPE_PEM) <= 0, 
            "error loading server certificate from %s", cargs->cert);

    /* load private key (perhaps from the cert file) */
    crit_err_ifm (tls_use_PrivateKey_file(c, cargs->key, SSL_FILETYPE_PEM,
                cargs->keymgr, cargs->sandbox) <= 0,
            "error loading the private key from %s", cargs->key);


    /* check skey consistency against scert */
    crit_err_ifm (!SSL_CTX_check_private_key(c),
            "the given private key doesn't seem to belong "
            "to the server certificate");

    /* load optional server certficate chain */
    if (cargs->certchain)
        crit_err_ifm (tls_use_certificate_chain(c, cargs->certchain, 
                    0, NULL) < 0, 
                "error loading server certificate chain");

    /* load optional CRL file + opts into args */
    if (cargs->crl)
        crit_err_ifm (tls_use_crls(c, cargs), "error loading CA CRL file");

    /* set SSL verify mode (no, optional, required) and callbacks */
    SSL_CTX_set_verify(c, cargs->vmode, cb_vfy);

    /* set verification depth (intermediate CAs allowed; 0: the client's
       certificate is signed directly by a CA in ca_file) */
    if (cargs->depth >= 0)
    {
#ifdef SSL_OPENSSL
        SSL_CTX_set_verify_depth(c, cargs->depth);
#else
        warn("certificate verification depth not supported");
#endif
    }

    return 0;
err:
    return ~0;
}

static int cb_vfy (int ok, X509_STORE_CTX *store_ctx)
{
    int e;
    X509 *x;
    char buf[1024];
    
    if (ok)
        return ok;

    e = X509_STORE_CTX_get_error(store_ctx);
    x = X509_STORE_CTX_get_current_cert(store_ctx);

    /* at present just put a note in the log.
     * the idea is that here we can catch CRL specific errors and, based 
     * on the value of crl_opts directive, use different accept/reject 
     * policies.  e.g. return ok in case X509_V_ERR_CRL_HAS_EXPIRED, etc. */
    u_info("%s; current certificate subject is %s",
            X509_verify_cert_error_string(e), x == NULL ? "(none)" :
            X509_NAME_oneline(X509_get_subject_name(x), buf, sizeof buf));

    return 0;
}

void tls_ctx_free (tls_ctx_t *ctx)
{
    SSL_CTX_free((SSL_CTX *) ctx);
}

int tls_init (void)
{
    if (tls_inited)
        return 0;

    /* OpenSSL 3 initialises itself; this makes it explicit (and loads the
       error strings, which SSL_load_error_strings() did) */
    dbg_err_if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL) != 1);
    dbg_err_if (RAND_status() != 1);
    tls_inited++;

    return 0;

err:
    u_dbg("%s", tls_get_error()); 
    return ~0;
}

#ifdef SSL_OPENSSL
/* set ephemeral DH parameters: OpenSSL's automatic choice, or dh_file */
static int tls_gendh_params(SSL_CTX *c, const char *dhfile)
{
    EVP_PKEY *eph_dh = NULL;

    dbg_return_if (c == NULL, ~0);

    if (!dhfile)
        return SSL_CTX_set_dh_auto(c, 1) == 1 ? 0 : ~0;

    eph_dh = tls_load_dh_param(dhfile);
    dbg_err_if (!(eph_dh));

    dbg_err_if (SSL_CTX_set0_tmp_dh_pkey(c, eph_dh) != 1);
    eph_dh = NULL; /* ownership transferred */

#if 0
    /* Avoid small subgroup attacks (if p and g are strong primes
     * this is not strictly necessary).  This is said to have a negligible (?)
     * impact during negotiation phase. TODO: test it ! */
    (void) SSL_CTX_set_options(c, SSL_OP_SINGLE_DH_USE); */
#endif /* 0 */

    return 0;
err:
    u_dbg("%s", tls_get_error());
    if (eph_dh)
        EVP_PKEY_free(eph_dh);

    return ~0;
}

static EVP_PKEY *tls_load_dh_param (const char *res_name)
{
    EVP_PKEY *dh = NULL;
    BIO *bio = NULL;

    dbg_return_if (res_name == NULL, NULL);

    /* say return_if here instead of err_if because bio_from_emb()
     * could have failed for a non-openssl error */
    dbg_return_if (!(bio = tls_get_file_bio(res_name)), NULL);
    dbg_err_if (!(dh = PEM_read_bio_Parameters(bio, NULL)));

    BIO_free(bio);

    return dh;
err:
    u_dbg("%s", tls_get_error());
    if (bio) 
        BIO_free(bio);

    return NULL;
}
#endif

static int tls_no_passphrase_cb (char *buf, int num, int w, void *arg)
{
    u_unused_args(buf, num, w, arg);

    return -1;
}

static int tls_init_ctx_args (tls_ctx_args_t *cargs)
{
    dbg_return_if (!cargs, ~0);

    cargs->cert = NULL;
    cargs->key = NULL;
    cargs->ca = NULL;
    cargs->dh = NULL;
    cargs->crl = NULL;
    cargs->crlopts = 0;
    cargs->depth = 1;
    cargs->vmode = SSL_VERIFY_NONE;
    cargs->ciphers = TLS_DEFAULT_CIPHERS;
    cargs->suites = NULL;
    cargs->minver = TLS1_2_VERSION;
    cargs->ticket_period = TLS_TICKET_PERIOD;

    return 0;
}

static int tls_set_ctx_vdepth (u_config_t *cfg, tls_ctx_args_t *cargs)
{
    u_config_t    *k;
    const char *v;
    char *end;
    long n;

    dbg_return_if (!cfg || !cargs, ~0);

    /* a number from 0 to 100 (a block, an empty value or "3x" stop klind
       instead of leaving OpenSSL's default in place) */
    if (!u_config_get_subkey(cfg, "verify_depth", &k))
    {
        v = u_config_get_value(k);
        n = v ? strtol(v, &end, 10) : -1;
        crit_err_ifm (v == NULL || end == v || *end != '\0' || n < 0 ||
                n > 100, "verify_depth must be a number from 0 to 100");
        cargs->depth = (int) n;
    }

    return 0;
err:
    return ~0;
}


static int tls_set_ctx_crlopts (u_config_t *cfg, tls_ctx_args_t *cargs)
{
    const char *v;
    
    dbg_return_if (cfg == NULL, ~0);
    dbg_return_if (cargs == NULL, ~0);

    v = u_config_get_subkey_value(cfg, "crl_opts");

    if (v == NULL)
    {
        cargs->crlopts = 0;
        return 0;
    }

#ifdef SSL_OPENSSL
    if (!strcasecmp(v, "check_all"))
        cargs->crlopts = X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL;
    else
        warn_err("unknown value %s for 'crl_opts' directive", v);
#else
    warn_err("CRLs not supported");
#endif

    return 0;
err:
    return ~0;
}

/* 'verify_mode' and 'verify_client' are aliases
 * the former is deprecated and retained only for compatibility with klin 1 */
static int tls_set_ctx_vmode (u_config_t *cfg, tls_ctx_args_t *cargs)
{
    const char *v;
    
    dbg_return_if (cfg == NULL, ~0);
    dbg_return_if (cargs == NULL, ~0);
    
    /* try 'verify_mode' directive first then 'verify_client' */
    if  ((v = u_config_get_subkey_value(cfg, "verify_mode")) == NULL)
        v = u_config_get_subkey_value(cfg, "verify_client");

    if (v == NULL || !strcasecmp(v, "no"))  /* unset == none */
        cargs->vmode = SSL_VERIFY_NONE;
    else if (!strcasecmp(v, "optional"))
        cargs->vmode = SSL_VERIFY_PEER;
    else if (!strcasecmp(v, "require"))
        cargs->vmode = SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    else
        warn_err("unknown verification value:\'%s\'", v);
    
    return 0;
err:
    return ~0;
}

static int tls_check_ctx (tls_ctx_args_t *cargs)
{
    dbg_return_if (cargs == NULL, ~0);

    /* cert_file is a MUST */
    crit_err_ifm (!cargs->cert || strlen(cargs->cert) == 0,
        "missing cert_file option parameter");

    /* if private key file is missing, assume the key is inside cert_file */
    warn_ifm (cargs->key == NULL,
        "missing cert key option, assuming the key is inside cert_file");

    /* if verify_mode == "required" the CA file MUST be present */
    if (cargs->vmode & SSL_VERIFY_PEER)
        crit_err_ifm (cargs->ca == NULL,
            "SSL verify is required but CA certificate file is missing");

#ifdef SSL_OPENSSL
    /* if 'crl_file' was given, set crlopts at least to verify the client
     * certificate against the supplied CRL */
    if (cargs->crl && cargs->crlopts == 0)
        cargs->crlopts = X509_V_FLAG_CRL_CHECK;
#endif

    return 0;
err:
    return ~0;
}


static void tls_free_ctx_args (tls_ctx_args_t *cargs)
{
    KLIN_FREE(cargs);
    return;
}
#endif /* SSL_ON */
