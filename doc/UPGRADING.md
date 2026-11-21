# Upgrading from KLone

Klin 4.0.0 is KLone 3.1.0 renamed and brought up to date. A KLone site
builds with few changes, but 4.0.0 is stricter by default: read the new
defaults below before you deploy it. The [ChangeLog](https://github.com/DanielTillett/klin/blob/master/ChangeLog)
lists every change.

## New defaults that may change a deployment

Each has a setting:

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

Unchanged: the `.kl1` (and `.klx`, `.klc`) page extensions, the
`KL1_CLISES_*` client-session cookies, the configuration file format, the
Windows service name (`kloned`) and the installed library names
(`libhttp.a`, `libutils.a` ...). A `kloned.conf` given with `-f` works as
it is; only the example and manual names changed (`klind.conf`).

## Other changes since KLone 3.1.0

### TLS

- TLS 1.2 is the minimum protocol version; TLS 1.0 and 1.1 are rejected
  and cannot be turned back on (`tls_min_version 1.3` raises the minimum).
  TLS compression and renegotiation are disabled.
- OpenSSL 3 is required. Contexts are owned opaque objects accessed
  through public APIs.
- Ephemeral 512-bit RSA and the built-in 1024-bit DH group are gone.
  Without `dh_file`, OpenSSL selects suitable DH parameters automatically.

### Sessions and cookies

- Session ids carry 128 bits from a cryptographic random source, replacing
  MD5(time, pid, rand()).
- Client-side sessions are sealed with AES-256-GCM ([SESSIONS](SESSIONS.md));
  sessions in the format before 4.0 are still read. Existing client-side
  sessions may need to be renewed after upgrading.
- Session cookies are sent with `Path=/`, `HttpOnly` and `SameSite=Lax`,
  plus `Secure` on direct TLS connections. Deletion cookies keep the same
  scope and attributes. Third-party login or embedding flows may need a
  different SameSite policy. TLS terminated by a proxy is not inferred from
  forwarded headers, which are untrusted.
- `response_set_cookie()` keeps its previous behavior for application
  cookies. The new `response_set_cookie_ex()` takes explicit
  `RESPONSE_COOKIE_*` flags (`SECURE`, `HTTPONLY`, `SAMESITE_LAX`,
  `SAMESITE_STRICT`, `SAMESITE_NONE`).

### HTTP request handling

- Rejected: duplicate or overflowing Content-Length, any
  Transfer-Encoding (chunked request bodies are not supported), invalid
  field names, control characters in field values, folded headers,
  unterminated headers, and decoded NUL bytes in URI paths.
- Limits (configurable, [LIMITS](LIMITS.md)): 100 header fields, 8192
  bytes per header line, 32768 bytes of headers in total.
- The query string is split off before the path is URL-decoded, so an
  escaped `?` (`%3F`) and a literal `+` in the path are preserved.

### Persistent connections (keep-alive)

- Connections stay open for further requests (HTTP/1.1 by default,
  HTTP/1.0 with `Connection: keep-alive`), including pipelined requests.
  Settings: `keep_alive`, `keep_alive_timeout` (5 s) and `keep_alive_max`
  (100 requests), see klind.conf(5). Off in the iterative model (`-d`) and
  in CGI mode.
- Responses to HTTP/1.1 requests use an HTTP/1.1 status line. Every body
  is delimited: pages that fit the 4 KB response buffer get a
  Content-Length, longer ones use chunked transfer coding (HTTP/1.1) or
  end with the close (HTTP/1.0). A Content-Length set by a page or CGI is
  enforced and any Transfer-Encoding it sets is replaced.
- HEAD, 204 and 304 responses never carry a body; empty pages no longer
  send a stray newline; the built-in error page has a Content-Length and
  is never appended to a response already under way; a CGI `Status:` line
  is no longer passed to the client; HEAD on an embedded static file
  works.
- Reading a request body (forms, uploads, pages, CGIs) stops at its end,
  and an unread body is skipped before the next request. Over TLS,
  connections end with close_notify.
- In the prefork model a worker holding a kept-alive connection counts as
  busy: while clients wait, the pool doubles every second up to
  `prefork.max_child`. A worker started beyond `prefork.start_child` exits
  after `prefork.idle_timeout` seconds (default 10, 0 = never) without a
  client, so the pool shrinks back after a burst.
  `prefork.max_requests_per_child` counts connections, not requests.

### Utility APIs

- New capacity-aware `u_urlncpy_s()`, `u_hexncpy_s()`, `u_htmlncpy_s()`
  and `u_sqlncpy_s()` reject undersized destinations. Internal callers use
  them; the legacy functions remain for compatibility and still require
  correctly sized caller buffers.
- SQL quote encoding now processes the whole input instead of truncating
  after quotes.
- Fixed out-of-bounds reads in URL decoding and substring search, invalid
  hex input handling, undefined behavior in MD5 and path normalization,
  seeking to the end of memory streams, and negative read results being
  treated as large lengths.

### Reliability

- The SIGCHLD handler no longer touches the server object, which may be
  unset during startup or shutdown.
- Memory leaks found by LeakSanitizer in klind shutdown, the page
  translator and the klin tool are fixed.

## Removed in 4.0.0

The MaKL configure script and GNU makefiles, the `skin/` packaging system,
the CyaSSL backend, OpenSSL TLS-PSK support, the Kilt URL module and the
old LibU 1.x unit tests (their checks are covered by the regression suite)
were removed. `klin -c import` no longer writes `autogen.mk` or
`autogen.dps`; KLone 3.1.0 in the original KoanLogic repository still has
them.
