# Upgrading from KLone

Klin 4.0.0 is KLone 3.1.0 renamed and brought up to date. A KLone 3.x site
builds with few changes, but 4.0.0 is stricter by default, so read the new
defaults below before you deploy it. The [ChangeLog](../ChangeLog) lists
every change.

## The checklist

Work through these in order. Most sites need only the first four.

1. Build with CMake instead of MaKL, mapping your old configure options
   with the [build options](#build-options) table. Pass `-DKLIN_SITE_DIR`
   for the site you embed.
2. Set `uid` and `gid` in klind.conf if klind starts as root. Without them
   it now refuses to start on Unix-like systems, unless you set
   `allow_root yes` ([SYSTEMD](SYSTEMD.md)).
3. Read [New defaults that may change a
   deployment](#new-defaults-that-may-change-a-deployment) and turn back
   on anything your devices or clients depend on, one setting at a time.
4. Call `session_regenerate_id()` at login. A session id made up by the
   client is no longer accepted ([SESSIONS](SESSIONS.md)).
5. Test any third-party login or embedding flow, since session cookies are
   now `SameSite=Lax`. If you use client-side sessions, expect every
   existing one to be rejected once, because klind draws fresh session keys
   at each start.
6. Check that numeric settings are in range: `max_child` from 1 to 896 and
   `verify_depth` from 0 to 100. A setting out of range now stops klind at
   startup.
7. When convenient, move to the Klin names (`klind`, `<klin/X.h>`,
   `KLIN_*`, `.klin`). The KLone names keep working in 4.x and go in 5.0.0.

## New defaults that may change a deployment

I made the defaults strict on purpose. A device that ships with a web
server is usually never reconfigured, so the safe setting has to be the
one it gets out of the box. Each change has a setting if you need the old
behaviour back:

| What changed | To change it |
| --- | --- |
| TLS 1.2 ciphers by default: forward-secret authenticated encryption (AEAD) only; old clients with CBC-only or RSA key exchange are refused | `tls_ciphers` ([TLS](TLS.md)); RSA key exchange also needs `key_manager no`, since the key manager does not decrypt with the key ([KEYMGR](KEYMGR.md)) |
| The TLS private key is held by a separate process (RSA and EC keys; not on native Windows); it stays out of the workers only with a `key_file` separate from the certificate that the workers' user cannot read and no other server loads with `key_manager no` ([conditions](KEYMGR.md#why-it-exists)) | `key_manager no` ([KEYMGR](KEYMGR.md)) |
| Workers run under a seccomp sandbox on Linux (x86-64 and ARM64), with argument checks (sockets, W^X memory, signals) | `sandbox no`, `sandbox_allow` ([SANDBOX](SANDBOX.md)) |
| Worker limits: 1024 MB, 1024 files, and processes up to the model's maximum workers (`prefork.max_child` or `fork.max_child`, 150 by default) + 64 | `worker_max_memory`, `worker_max_files`, `worker_max_procs`; 0 leaves the inherited system limit unchanged ([LIMITS](LIMITS.md)) |
| Header limits, a header deadline, connections per client address, SSE and WebSocket streams | `max_header_*`, `idle_timeout`, `max_conn_per_ip`, `max_streams` ([LIMITS](LIMITS.md)) |
| Symbolic links are followed only inside the document root | `follow_symlinks` ([FILES](FILES.md)) |
| CGI: 60 s, 30 s of CPU, 512 MB; klind's environment and the Authorization field are not passed | the `cgi` block: `timeout`, `max_cpu`, `max_memory`, `pass_env`, `pass_headers` ([CGI](CGI.md)) |
| Security headers on every response, unless turned off | `security_headers` ([HEADERS](HEADERS.md)) |
| A session id the client makes up is not accepted | none: call `session_regenerate_id()` at login ([SESSIONS](SESSIONS.md)) |
| klind does not keep root credentials | `uid` and `gid`, or `allow_root yes` ([SYSTEMD](SYSTEMD.md)) |
| Settings out of range stop klind at startup | `max_child` 1 to 896, `verify_depth` 0 to 100 ([klind.conf(5)](man/klind.conf.5)) |

## Build options

The MaKL configure script and GNU makefiles are gone. I replaced them with
CMake so that the same build runs with Ninja, BSD make or GNU make, and is
tested on Linux and macOS. The old options map across like this:

| KLone 3.x | Klin 4.x (CMake) |
| --- | --- |
| `./configure --enable_sup_fs` (or `--enable_fs`) | `-DKLIN_ENABLE_SUP_FS=ON` |
| `./configure --enable_sup_cgi` (or `--enable_cgi`) | `-DKLIN_ENABLE_SUP_CGI=ON` |
| `./configure --enable_hooks` | `-DKLIN_ENABLE_HOOKS=ON` |
| `./configure --enable_debug` | `-DKLIN_DEBUG=ON` |
| `./configure --enable_libu_debug` | `-DLIBU_DEBUG=ON` |
| `./configure --enable_warns` | `-DKLIN_ENABLE_WARNINGS=ON` |
| `./configure --daemon_name` | `-DKLIN_DAEMON_NAME=<name>` (default `klind`) |
| `./configure --install_daemon_only` | `-DKLIN_INSTALL_DAEMON_ONLY=ON` |
| `WEBAPP_SRCS` | `-DKLIN_WEBAPP_SOURCES=<list>` |
| `-DKLONE_*` (CMake builds from before the rename) | `-DKLIN_*`; the old names are accepted with a deprecation warning |
| `make`, `make install` | `cmake --build out`, `cmake --install out` |

The full list of options, including new ones such as `KLIN_WITH_OPENSSL`,
`KLIN_HARDENING` and `KLIN_LTO`, is in the README under
[Building with CMake](../README.md#building-with-cmake).

## Renamed from KLone

KLone was renamed Klin in 4.0.0. The old names still work in 4.x and will
be removed in 5.0.0 (TODO lists what goes):

| KLone | Klin | The old name in 4.x |
| --- | --- | --- |
| `kloned`, `klone` (commands) | `klind`, `klin` | symlinks (Unix install) |
| `<klone/X.h>`, `klone_conf.h` | `<klin/X.h>`, `klin_conf.h` | forwarding headers (installed too) |
| `klone_die`, `klone_die_if`, `klone_version`, `KLONE_VERSION`, `KLONE_UNCONST`, `KLONE_FREE` ... | `klin_die`, ..., `KLIN_*` | aliases, through the forwarding headers |
| `-DKLONE_*` CMake settings | `-DKLIN_*` | accepted, with a deprecation warning |
| `klone_sid` (session cookie) | `klin_sid` | read, never set |
| `KLONE_CIPHER_KEY` (session variable, environment) | `KLIN_CIPHER_KEY` | read (saved sessions, pages, the klin tool) |
| `.klone` pages, `/index.klone` | `.klin`, `/index.klin` | both are pages |
| `/etc/kloned.conf` embedded in the site | `/etc/klind.conf` | used when there is no `/etc/klind.conf` |
| `klone_sess_*` session files | `klin_sess_*` | renamed when used |
| `Server: klone/<version>` | `Server: klin/<version>` | - |

Some things did not change: the `.kl1` and `.klc` page extensions, the
`KL1_CLISES_*` client-session cookies, the configuration file format, the
Windows service name (`kloned`) and the installed library names
(`libhttp.a`, `libutils.a` ...). A `kloned.conf` given with `-f` works as
it is; only the names of the example and the manual changed
([klind.conf(5)](man/klind.conf.5)).

## Other changes since KLone 3.1.0

Most of what changed needs nothing from you, and the
[ChangeLog](../ChangeLog) has all of it. These are the changes a port may
have to act on.

### TLS

If some of your clients still speak only TLS 1.0 or 1.1, they can no
longer connect. TLS 1.2 is the minimum and cannot be lowered, though
`tls_min_version 1.3` raises it. TLS compression and renegotiation are
off.

If you build with TLS (`KLIN_WITH_OPENSSL`, on by default), you need
OpenSSL 3. Ephemeral 512-bit RSA and the built-in 1024-bit DH group are
gone; without `dh_file`, OpenSSL picks suitable DH parameters itself.

### Sessions and cookies

Session ids now carry 128 bits from a cryptographic random source, not the
MD5(time, pid, rand()) KLone used. Client-side sessions are sealed with
AES-256-GCM ([SESSIONS](SESSIONS.md)). klind can still read the format from
before 4.0, but it generates new random keys every time it starts, so a
client-side session cookie issued before the upgrade will not verify and
the user starts a new session.

Session cookies are sent with `Path=/`, `HttpOnly` and `SameSite=Lax`,
plus `Secure` on direct TLS connections, and deletion cookies keep the same
scope and attributes. If you rely on a third-party login or embedding flow,
it may need a different SameSite policy. If a proxy terminates TLS in front
of klind, the session cookie is sent without `Secure`, because klind does
not trust forwarded headers to tell it the client used TLS.

`response_set_cookie()` behaves as before for application cookies. To set
the new attributes on your own cookies, call `response_set_cookie_ex()` with
`RESPONSE_COOKIE_*` flags (`SECURE`, `HTTPONLY`, `SAMESITE_LAX`,
`SAMESITE_STRICT`, `SAMESITE_NONE`).

### HTTP requests

If a client sends a chunked request body, it now gets 501: send a
Content-Length instead. klind also rejects a duplicate or overflowing
Content-Length, invalid field names, control characters in field values,
folded or unterminated headers and decoded NUL bytes in URI paths. Headers
are limited to 100 fields, 8192 bytes per line and 32768 bytes in total,
all configurable ([LIMITS](LIMITS.md)).

The query string is split off before the path is URL-decoded, so an
escaped `?` (`%3F`) and a literal `+` in the path now reach your page
unchanged.

### Persistent connections (keep-alive)

Connections now stay open for further requests (HTTP/1.1 by default,
HTTP/1.0 with `Connection: keep-alive`), including pipelined requests. The
settings are `keep_alive`, `keep_alive_timeout` (5 s) and `keep_alive_max`
(100 requests); see [klind.conf(5)](man/klind.conf.5). Keep-alive is off
in the iterative model (`-d`) and in CGI mode.

Every response body is now delimited, so a page or CGI must not lie about
its length. Pages that fit the 4 KB response buffer get a Content-Length;
longer ones use chunked transfer coding (HTTP/1.1) or end with the close
(HTTP/1.0). A Content-Length set by a page or CGI is enforced, any
Transfer-Encoding it sets is replaced, and a CGI `Status:` line sets the
status rather than being passed through. Reading a request body stops at
its end, and an unread body is skipped before the next request.

What to do about it:

- Make sure no page or CGI script sets a Content-Length that does not
  match its body; it is now enforced.
- If you set `prefork.max_requests_per_child`, recount it: it now counts
  connections, not requests.
- On prefork sites, size `prefork.max_child` for kept-alive clients. A
  worker holding a kept-alive connection counts as busy; while clients
  wait, the pool doubles every second up to `prefork.max_child`, and
  workers beyond `prefork.start_child` exit after
  `prefork.idle_timeout` seconds without a client (default 10, 0 means
  never).

### Utility functions

If your pages call `u_urlncpy()`, `u_hexncpy()`, `u_htmlncpy()` or
`u_sqlncpy()`, consider the capacity-aware `u_urlncpy_s()`,
`u_hexncpy_s()`, `u_htmlncpy_s()` and `u_sqlncpy_s()`, which reject an
undersized destination. The old functions remain, and still need a
correctly sized buffer from the caller.

## Removed in 4.0.0

| Removed | What to use instead |
| --- | --- |
| MaKL configure script and GNU makefiles | CMake ([build options](#build-options)) |
| `skin/` packaging system and `pkg/` | `cmake --install`, with the installed CMake package and pkg-config file |
| the RHEL init script and the old `contrib/` tools | the systemd unit in `contrib/systemd` ([SYSTEMD](SYSTEMD.md)) |
| CyaSSL backend | OpenSSL 3 |
| C++ pages (`.klx`) | none: `klin -c import` still accepts them, but the site build stops at the CMake configure step ("C++ page ... is not supported by this build") |
| OpenSSL TLS-PSK support | none |
| Kilt URL module | none; a `routes` block ([ROUTES](ROUTES.md)) is the nearest thing |
| `autogen.mk` and `autogen.dps` from `klin -c import` | `autogen.cmake`, written by the same command for the site build |
| LibU 1.x unit tests | the regression suite, which covers their checks |

If you need `autogen.mk` or `autogen.dps`, KLone 3.1.0 in the original
KoanLogic repository still has them.

Next: [SYSTEMD](SYSTEMD.md) to run the upgraded klind as a service,
[LIMITS](LIMITS.md) for the new request and worker limits, and
[SESSIONS](SESSIONS.md) for the session changes.
