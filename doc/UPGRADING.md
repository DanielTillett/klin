# Upgrading from KLone

This page is for anyone moving a KLone 3.x site to Klin. It lists the
defaults that may change how a deployment behaves, the names that changed,
the build options that replace the old configure script and what was
removed.

Klin 4.0.0 is KLone 3.1.0 renamed and brought up to date. A KLone site
builds with few changes, but 4.0.0 is stricter by default, so read the new
defaults below before you deploy it. The
[ChangeLog](https://github.com/DanielTillett/klin/blob/master/ChangeLog)
lists every change.

## The checklist

Work through these in order. Most sites need only the first four.

1. Build with CMake instead of MaKL, mapping your old configure options
   with the [build options](#build-options) table. Pass `-DKLIN_SITE_DIR`
   for the site you embed.
2. Set `uid` and `gid` in klind.conf if klind starts as root. Without them
   it now refuses to start ([SYSTEMD](SYSTEMD.md)).
3. Read [New defaults that may change a deployment](#new-defaults-that-may-change-a-deployment)
   and turn back on anything your devices or clients depend on, one setting
   at a time.
4. Call `session_regenerate_id()` at login. A session id made up by the
   client is no longer accepted ([SESSIONS](SESSIONS.md)).
5. Test any third-party login or embedding flow, since session cookies are
   now `SameSite=Lax`, and expect existing client-side sessions to need
   renewing.
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
| TLS 1.2 ciphers: forward-secret AEAD only; old clients with CBC-only or RSA key exchange are refused | `tls_ciphers` ([TLS](TLS.md)) |
| The TLS private key is held by a separate process (RSA and EC keys) | `key_manager no` ([KEYMGR](KEYMGR.md)) |
| Workers run under a seccomp sandbox, with argument checks (sockets, W^X memory, signals) | `sandbox no`, `sandbox_allow` ([SANDBOX](SANDBOX.md)) |
| Worker limits: 1024 MB, 1024 files, `max_child` + 64 processes | `worker_max_memory`, `worker_max_files`, `worker_max_procs`; 0 turns one off ([LIMITS](LIMITS.md)) |
| Header limits, a header deadline, connections per client address, SSE and WebSocket streams | `max_header_*`, `idle_timeout`, `max_conn_per_ip`, `max_streams` ([LIMITS](LIMITS.md)) |
| Symbolic links are followed only inside the document root | `follow_symlinks` ([FILES](FILES.md)) |
| CGI: 60 s, 30 s of CPU, 512 MB; klind's environment and the Authorization field are not passed | the `cgi` block: `timeout`, `max_cpu`, `max_memory`, `pass_env`, `pass_headers` ([CGI](CGI.md)) |
| Security headers on every response | `security_headers` ([HEADERS](HEADERS.md)) |
| A session id the client makes up is not accepted | none: call `session_regenerate_id()` at login ([SESSIONS](SESSIONS.md)) |
| klind does not keep root credentials | `uid` and `gid`, or `allow_root yes` ([SYSTEMD](SYSTEMD.md)) |
| Settings out of range stop klind at startup | `max_child` 1 to 896, `verify_depth` 0 to 100 (klind.conf(5)) |

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

Some things did not change: the `.kl1` (and `.klx`, `.klc`) page
extensions, the `KL1_CLISES_*` client-session cookies, the configuration
file format, the Windows service name (`kloned`) and the installed library
names (`libhttp.a`, `libutils.a` ...). A `kloned.conf` given with `-f`
works as it is; only the names of the example and the manual changed
(`klind.conf`).

## Other changes since KLone 3.1.0

### TLS

TLS 1.2 is the minimum protocol version. TLS 1.0 and 1.1 are rejected and
cannot be turned back on, though `tls_min_version 1.3` raises the minimum.
TLS compression and renegotiation are disabled.

OpenSSL 3 is required. Contexts are owned opaque objects accessed through
public APIs.

Ephemeral 512-bit RSA and the built-in 1024-bit DH group are gone. Without
`dh_file`, OpenSSL selects suitable DH parameters itself.

### Sessions and cookies

Session ids carry 128 bits from a cryptographic random source, replacing
the MD5(time, pid, rand()) ids KLone made.

Client-side sessions are sealed with AES-256-GCM ([SESSIONS](SESSIONS.md)).
Sessions in the format before 4.0 are still read, but existing client-side
sessions may need to be renewed after upgrading.

Session cookies are sent with `Path=/`, `HttpOnly` and `SameSite=Lax`,
plus `Secure` on direct TLS connections. Deletion cookies keep the same
scope and attributes. Third-party login or embedding flows may need a
different SameSite policy. TLS terminated by a proxy is not inferred from
forwarded headers, because those headers can't be trusted.

`response_set_cookie()` keeps its previous behaviour for application
cookies. The new `response_set_cookie_ex()` takes explicit
`RESPONSE_COOKIE_*` flags (`SECURE`, `HTTPONLY`, `SAMESITE_LAX`,
`SAMESITE_STRICT`, `SAMESITE_NONE`).

### HTTP request handling

klind now rejects a duplicate or overflowing Content-Length, any
Transfer-Encoding (chunked request bodies are not supported), invalid
field names, control characters in field values, folded headers,
unterminated headers and decoded NUL bytes in URI paths. The limits are
100 header fields, 8192 bytes per header line and 32768 bytes of headers
in total, all configurable ([LIMITS](LIMITS.md)).

The query string is split off before the path is URL-decoded, so an
escaped `?` (`%3F`) and a literal `+` in the path are preserved.

### Persistent connections (keep-alive)

Connections now stay open for further requests (HTTP/1.1 by default,
HTTP/1.0 with `Connection: keep-alive`), including pipelined requests. The settings are
`keep_alive`, `keep_alive_timeout` (5 s) and `keep_alive_max` (100
requests); see klind.conf(5). Keep-alive is off in the iterative model
(`-d`) and in CGI mode.

Responses to HTTP/1.1 requests use an HTTP/1.1 status line. Every body is
delimited: pages that fit the 4 KB response buffer get a Content-Length,
longer ones use chunked transfer coding (HTTP/1.1) or end with the close
(HTTP/1.0). A Content-Length set by a page or CGI is enforced, and any
Transfer-Encoding it sets is replaced.

Several old bugs went with this work. HEAD, 204 and 304 responses never
carry a body. Empty pages no longer send a stray newline. The built-in
error page has a Content-Length and is never appended to a response
already under way. A CGI `Status:` line is no longer passed to the client.
HEAD on an embedded static file works.

Reading a request body (forms, uploads, pages, CGIs) stops at its end, and
an unread body is skipped before the next request. Over TLS, connections
end with close_notify.

The catch for prefork sites is that a worker holding a kept-alive
connection counts as busy. While clients wait, the pool doubles every
second up to `prefork.max_child`. A worker started beyond
`prefork.start_child` exits after `prefork.idle_timeout` seconds without a
client (default 10, 0 means never), so the pool shrinks back after a
burst. `prefork.max_requests_per_child` now counts connections, not
requests.

### Utility APIs

New capacity-aware `u_urlncpy_s()`, `u_hexncpy_s()`, `u_htmlncpy_s()` and
`u_sqlncpy_s()` reject undersized destinations. Klin's own code uses them.
The legacy functions remain for compatibility and still need correctly
sized caller buffers.

SQL quote encoding now processes the whole input instead of truncating
after quotes.

Fixed along the way: out-of-bounds reads in URL decoding and substring
search, invalid hex input handling, undefined behaviour in MD5 and path
normalisation, seeking to the end of memory streams, and negative read
results being treated as large lengths.

### Reliability

The SIGCHLD handler no longer touches the server object, which may be
unset during startup or shutdown. Memory leaks found by LeakSanitizer in
klind shutdown, the page translator and the klin tool are fixed.

## Removed in 4.0.0

| Removed | What to use instead |
| --- | --- |
| MaKL configure script and GNU makefiles | CMake ([build options](#build-options)) |
| `skin/` packaging system and `pkg/` | `cmake --install`, with the installed CMake package and pkg-config file |
| the RHEL init script and the old `contrib/` tools | the systemd unit in `contrib/systemd` ([SYSTEMD](SYSTEMD.md)) |
| CyaSSL backend | OpenSSL 3 |
| OpenSSL TLS-PSK support | none |
| Kilt URL module | none; a `routes` block ([ROUTES](ROUTES.md)) is the nearest thing |
| `autogen.mk` and `autogen.dps` from `klin -c import` | `autogen.cmake`, written by the same command for the site build |
| LibU 1.x unit tests | the regression suite, which covers their checks |

If you need `autogen.mk` or `autogen.dps`, KLone 3.1.0 in the original
KoanLogic repository still has them.
