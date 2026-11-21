# Klin

**A web framework for embedded devices: the web server, its configuration
and your pages, compiled into one small program.**

[![Build and test](https://github.com/DanielTillett/klin/actions/workflows/build.yml/badge.svg)](https://github.com/DanielTillett/klin/actions/workflows/build.yml)
[![CodeQL](https://github.com/DanielTillett/klin/actions/workflows/codeql.yml/badge.svg)](https://github.com/DanielTillett/klin/actions/workflows/codeql.yml)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/DanielTillett/klin/badge)](https://scorecard.dev/viewer/?uri=github.com/DanielTillett/klin)
[![License: BSD-3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](https://github.com/DanielTillett/klin/blob/master/LICENSE)

Klin builds a device's web interface into a single executable, `klind`:
an HTTP and HTTPS server, its configuration, and the site's static files
and pages, where a page is HTML with C code in it. It is about 0.7 MB
(stripped, x86-64 Linux), needs only libc, OpenSSL 3 and zlib, and comes
with secure defaults for a device on a network.

```html
<%! #include <time.h> %>
<html><body>
  <h1>Device status</h1>
  <p>Time on the device: <% time_t t = time(NULL); io_printf(out, "%s", ctime(&t)); %></p>
</body></html>
```

Klin is derived from [KLone](https://github.com/koanlogic/klone) by
KoanLogic s.r.l. (no longer maintained) and was called KLone until 4.0.0,
which brings it up to date: a CMake build that needs no GNU make, OpenSSL 3
with TLS 1.2 or newer, hardened HTTP parsing and sessions, a sandbox, and
automated regression, full-server and fuzz tests. Upgrading from KLone?
Read [Security and compatibility changes](#security-and-compatibility-changes)
and [Renamed from KLone](#renamed-from-klone) first.

**Try it** (Linux or WSL, with the tools in [Quick start](#quick-start-linux-wsl-macos)):

```sh
git clone https://github.com/DanielTillett/klin.git && cd klin
python3 build/build.py                       # fetches the pinned LibU, builds
cmake -S . -B out -G Ninja -DKLIN_SITE_DIR=doc/example-site
cmake --build out
out/bin/klind -F                             # then open http://localhost:8080/
```

**Contents:** [Why Klin](#why-klin) ·
[Documentation](#documentation) ·
[Quick start](#quick-start-linux-wsl-macos) ·
[Building with CMake](#building-with-cmake) ·
[Build-and-test runner](#build-and-test-runner) ·
[Building and running a site](#building-and-running-a-site) ·
[Testing](#testing) ·
[Renamed from KLone](#renamed-from-klone) ·
[Security and compatibility changes](#security-and-compatibility-changes) ·
[Known limitations](#known-limitations) ·
[Contributing](CONTRIBUTING.md) · [Security policy](SECURITY.md)


## Why Klin

Many devices need a web interface but cannot carry a general-purpose web
stack: a router, an industrial controller, a laboratory or test
instrument, a camera or a sensor gateway. Klin is built for that case.

- **One self-contained program.** The server, its configuration and the
  whole site (pages, images, scripts, style sheets) are compiled into a
  single executable, `klind`. There is no interpreter, no separate web
  server and nothing to unpack at install time, and the embedded site
  cannot be changed on the device. It runs from a read-only file system.
- **Small.** A stripped `klind` for x86-64 Linux is about 0.7 MB, and it needs
  only libc, OpenSSL 3 and zlib (both optional at build time). Pages are
  compiled C, so a request costs no interpreter start-up or garbage
  collection.
- **Pages in C.** A page is HTML with C code in it (`<% ... %>`), so it can
  call a device's own libraries, drivers and ioctls directly, with no
  bindings or IPC layer between the web interface and the hardware.
- **Secure defaults** for a device exposed to a network: TLS 1.2 and 1.3
  with forward-secret ciphers only; the TLS private key held by a separate
  process; workers under a seccomp sandbox with resource limits; a
  strict, fuzzed HTTP parser; hardened sessions with CSRF tokens; access
  rules and HTTP authentication; reproducible, signed releases with an
  SBOM. Each protection is documented with the setting that changes it.
- **Current web features:** REST routes and JSON, request validation,
  Server-Sent Events and WebSockets, keep-alive, ETags and ranges.
- **Portable C** (C99) with a CMake build that needs no GNU make: Linux,
  macOS, and Windows (MinGW, single process); BSD-licensed.
- **KLone's successor.** Klin keeps KLone's page format and configuration and
  accepts its old names throughout 4.x, so a KLone 3.x site moves over
  with few changes ([Renamed from KLone](#renamed-from-klone)).

It is less suited to sites whose content changes at run time (a CMS or a
blog), to teams that want a scripting language for their pages, or to
large-scale web serving, where a general-purpose server behind a load
balancer is the usual choice.


## Documentation

This README covers building, running, testing and what changed from
KLone. The manual pages are in [doc/man](https://github.com/DanielTillett/klin/tree/master/doc/man): `klind(8)` the daemon,
`klind.conf(5)` its configuration, `klin(1)` the page translator and site
tool. The other topics are in [doc/](https://github.com/DanielTillett/klin/tree/master/doc):

| Building a site | Security | Running and project |
| --- | --- | --- |
| [Routes](doc/ROUTES.md): REST routes | [TLS](doc/TLS.md): protocols, ciphers, tickets | [Limits](doc/LIMITS.md): request and worker limits |
| [JSON](doc/JSON.md): reader and writer | [Key manager](doc/KEYMGR.md): the TLS private key | [systemd](doc/SYSTEMD.md): the hardened unit |
| [Validation](doc/VALIDATE.md): request parameters | [Sandbox](doc/SANDBOX.md): seccomp for workers | [Hardening](doc/HARDENING.md): compiler flags |
| [Sessions](doc/SESSIONS.md): sessions and CSRF | [Access](doc/ACCESS.md): IP access rules | [Fuzzing](doc/FUZZING.md): fuzzing, static analysis |
| [SSE](doc/SSE.md): Server-Sent Events | [Authentication](doc/AUTH.md): Basic and Digest | [Releases](doc/RELEASES.md): signing, SBOM, CI rules |
| [WebSocket](doc/WEBSOCKET.md): WebSockets | [Headers](doc/HEADERS.md): security headers | [Conventions](doc/CONVENTIONS.md): code conventions |
| [Files](doc/FILES.md): files from the disk | [CGI](doc/CGI.md): CGI scripts and their limits | [Crypto](doc/CRYPTO.md): the encryption API |

[doc/example-site](https://github.com/DanielTillett/klin/tree/master/doc/example-site) is a small site to start from.
[SECURITY.md](SECURITY.md) says how to report a vulnerability,
[ChangeLog](https://github.com/DanielTillett/klin/blob/master/ChangeLog) lists every change and [TODO](https://github.com/DanielTillett/klin/blob/master/TODO) what is
planned.


## Quick start (Linux, WSL, macOS)

Requirements: CMake 3.24 or newer, a build tool (Ninja, BSD make or GNU
make), GCC or Clang, OpenSSL 3, zlib, Git, and Python 3.9 or newer for the
build-and-test runner. On Ubuntu:

```
sudo apt-get update
sudo apt-get install build-essential clang libclang-rt-dev libssl-dev \
    zlib1g-dev python3 git cmake ninja-build
python3 build/build.py --test
```

On Windows, run the same commands inside WSL from PowerShell in this
repository, for example:

```
wsl -d Ubuntu -- python3 build/build.py --test
```

On macOS, with Homebrew:

```
brew install cmake ninja openssl@3
export OPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
python3 build/build.py --test
```

The daemon is built at .build/release/cmake/bin/klind and the page
translator at .build/release/cmake/bin/klin. Build logs and
build-info.json are in .build/release.


## Building with CMake

build/build.py (below) wraps CMake with pinned dependencies and tests. To
use CMake directly, fetch the pinned LibU once (python3 build/build.py does
this, or pass -DLIBU_SOURCE_DIR=`<checkout>`), then:

```
cmake -S . -B out -G Ninja -DKLIN_SITE_DIR=doc/example-site
cmake --build out
cmake --install out           # optional; honors CMAKE_INSTALL_PREFIX
```

The install includes the libraries and all headers (Klin, LibU and the
generated configuration headers). To build against it, also put OpenSSL's
headers on the include path; Klin's public headers include them. The
server itself (including the embedded site and emb_init()) is linked by
the CMake build; the installed libraries do not contain klind's own
sources, as in KLone 3.x.

Multi-configuration generators (Visual Studio, Xcode, Ninja Multi-Config)
are not supported; choose the configuration with CMAKE_BUILD_TYPE (Release,
the default, builds with -O2).

GNU make is not required. With BSD make, use the Makefile generator (BSD
make cannot handle spaces in site paths; use Ninja for those):

```
cmake -S . -B out -G "Unix Makefiles" -DCMAKE_MAKE_PROGRAM=bmake
cd out && bmake
```

Options (KLone 3.x configure equivalents in brackets):

```
KLIN_SITE_DIR=<dir>          site to embed into klind (default: none)
KLIN_IMPORT_ARGS=<list>      extra 'klin -c import' options, e.g.
                              "-z;-x;*.bak" (compression, exclusions,
                              encryption patterns and key file)
KLIN_DEBUG=ON                debug logging and symbols [--enable_debug]
KLIN_ENABLE_SUP_FS=ON        serve files from disk [--enable_sup_fs]
KLIN_ENABLE_SUP_CGI=ON       external CGIs [--enable_sup_cgi]
KLIN_ENABLE_HOOKS=ON         user hooks_setup() [--enable_hooks]
KLIN_ENABLE_WARNINGS=ON      extra warnings [--enable_warns]
KLIN_DAEMON_NAME=<name>      daemon file name [--daemon_name]
KLIN_INSTALL_DAEMON_ONLY=ON  install only klind [--install_daemon_only]
KLIN_WEBAPP_SOURCES=<list>   web application sources [WEBAPP_SRCS]
KLIN_WITH_OPENSSL=OFF        build without TLS and encryption
KLIN_WITH_ZLIB=OFF           build without compression
```

With Doxygen installed, 'cmake --build out --target doc' writes the API
reference to out/doc.

How it works: CMake runs the platform checks that KLone 3.x's configure ran and
generates klin_conf.h and LibU's libu_conf.h in the build directory. It
builds LibU from the pinned checkout without modifying it, then the klin
tool, then the site. The site is a small separate CMake project (cmake/site)
because the klin tool must exist before the site can be imported:
'klin -c import' writes autogen.cmake, a list of pages, and each page is
translated to C and compiled into libsite. Editing a page retranslates only
that page; adding or removing files re-runs the import automatically.


## Build-and-test runner

```
python3 build/build.py [--sanitize] [--test] [--offline] [--incremental]
                       [--jobs N] [--generator G] [--make-program P]

--sanitize      Build with AddressSanitizer and UndefinedBehaviorSanitizer
                into .build/sanitize instead of .build/release.
--test          Run the regression suite and the full-server tests after
                building (see Testing).
--offline       Use already downloaded dependencies; fail if missing.
--incremental   Reuse the previous build directory during development.
                Omit it for clean, release-quality checks.
--jobs N        Parallel build jobs (default 2).
--generator G   CMake generator (default Ninja if installed, otherwise
                "Unix Makefiles").
--make-program P  Build tool for the generator, e.g. bmake.
```

How the runner works:

- LibU is cloned at the exact commit pinned in build/dependencies.json.
  Existing checkouts must match that commit and have no tracked
  modifications (CRLF-only differences are tolerated).
- Sources are copied as they are (.gitattributes checks text out with LF
  on every platform) into `.build/<mode>/source` and built out of tree in
  `.build/<mode>/cmake`. Your checkout is never configured or compiled in
  place, and nothing is installed on the system.
- Only one build may use a given `.build/<mode>` directory at a time; a
  second concurrent build of the same mode is refused.
- build-info.json records the dependency commits, a normalized source
  fingerprint, compiler, CMake, generator and OpenSSL versions, and C flags.
  This reproduces the source inputs; identical binaries across different
  compilers or distributions are not guaranteed.
- The build fails rather than silently disabling TLS if OpenSSL 3 is not
  found.


## Building and running a site

Klin embeds your site at build time. Point KLIN_SITE_DIR at the site
directory (its etc/ and www/ trees become the embedded file system) and
build:

```
cmake -S . -B out -G Ninja -DKLIN_SITE_DIR=doc/example-site
cmake --build out
```

Then run the daemon in the foreground with a configuration file:

```
out/bin/klind -F -f /path/to/klind.conf
```

-F keeps klind attached to the terminal instead of detaching as a daemon;
it still uses the configured server model. -d adds debug logging and serves
requests iteratively in a single process. -f overrides values in the
embedded configuration. See klind(8) and klind.conf(5) in doc/man for all
options.

TLS: 1.2 or 1.3 (preferred), forward-secret AEAD ciphers only, no
renegotiation, rotating session ticket keys, optional client certificates
(doc/TLS.md).

Sessions: HttpOnly, SameSite=Lax (and Secure over TLS) cookies, ids a
client makes up refused, session_regenerate_id() at login, idle and
absolute timeouts, client-side sessions sealed with AES-256-GCM, and CSRF
tokens (session_csrf_token/check) (doc/SESSIONS.md).

Fuzzing: every parser a client reaches has a libFuzzer target (fuzz/,
test/run_fuzz.py), run by ClusterFuzzLite on pull requests and daily;
CodeQL, clang-tidy and Coverity Scan analyse the code (doc/FUZZING.md).

Releases and supply chain: release tarballs are reproducible, come with an
SPDX SBOM and are signed with GitHub artifact attestations (keyless,
through Sigstore); workflows pin actions to commit SHAs with least-privilege
tokens, Dependabot keeps the pins current and OpenSSF Scorecard checks the
repository (doc/RELEASES.md). SECURITY.md says how to report a vulnerability.

Limits: request header size and field count (431), body size (413),
workers' memory, open files and processes, a
header deadline against slow clients, connections per client address and
SSE/WebSocket connections per server are capped, and requests that could be
smuggled past a proxy are refused (doc/LIMITS.md).

Files and CGI: serving files from the file system and running CGI scripts
are build options, off by default, and serve_files no / run_cgi no turn
them off per server or vhost. Symbolic links out of the document root
are not followed (follow_symlinks, doc/FILES.md). CGI scripts get only the
environment variables listed, never HTTP_PROXY or the credentials, and
time, CPU and memory limits (doc/CGI.md).

systemd: contrib/systemd/klind.service is a hardened unit; klind drops
from root to the uid and gid in klind.conf and refuses to keep root
credentials otherwise, unless 'allow_root yes' (doc/SYSTEMD.md).

Hardening: klind is built with _FORTIFY_SOURCE, stack-clash and
control-flow protection, PIE, full RELRO and a non-executable stack, each
where the toolchain has it (doc/HARDENING.md); -DKLIN_HARDENING=OFF builds
without.

Sandbox: workers and the TLS key manager run under a seccomp filter on
Linux (pledge on OpenBSD) that kills them for calls only an attacker needs
and refuses the ones they have no use for (doc/SANDBOX.md). On by default;
sandbox no turns it off.

TLS key manager: the private key of an HTTPS server is held by a separate
process; workers sign handshakes through it and never hold the key
(doc/KEYMGR.md). On by default; key_manager no turns it off.

IP access control: an "access" block allows or denies clients by address
and network, per path; a client denied everywhere is dropped before the
TLS handshake (doc/ACCESS.md).

HTTP authentication: an "auth" block protects paths with Basic or Digest
(SHA-256) authentication, realms and roles; passwords are stored as hashes
made by klin -c passwd. See doc/AUTH.md.

Security headers: responses carry X-Content-Type-Options: nosniff,
X-Frame-Options: SAMEORIGIN, Referrer-Policy and, over HTTPS,
Strict-Transport-Security; a Content-Security-Policy can be set. Change or
turn them off per vhost in a "security_headers" block (doc/HEADERS.md).

Caching and partial downloads: embedded static files carry a strong ETag
(a digest of the file made when the site is built), answer If-None-Match
with 304 and serve single byte ranges (206, or 416 when the range is past
the end; a request for several ranges gets the whole file). Encrypted
files have no ETag and no ranges. Pages are not affected.

Server-Sent Events: a page streams events with sse_start(), sse_send()
and sse_wait(), which waits for the page's own descriptor, a timeout or
the client going away (doc/SSE.md).

WebSockets: a page accepts a WebSocket with ws_accept() and exchanges
messages with ws_wait(), ws_recv() and ws_send(); pages of other sites are
refused by default (doc/WEBSOCKET.md).

REST APIs: a "routes" block maps methods and path patterns such as
/api/users/{id} to pages (doc/ROUTES.md), and `<klin/json.h>` reads request
bodies and writes responses as JSON (doc/JSON.md).

Request validation: "validators" and "params" blocks check query, form,
route and header parameters before a page runs (regular expressions,
integer and length ranges, value lists); invalid input gets 400 and
parameters no rule names are dropped (doc/VALIDATE.md).

Encrypted pages: give the import a pattern of pages to encrypt and the key
file, with an absolute path, for example
-DKLIN_IMPORT_ARGS="-e;`*.kl1`;-k;/path/to/key". Pages are re-encrypted when
the key file changes. A key given through the KLIN_CIPHER_KEY environment
variable is not tracked, so rebuild from clean after changing it.

HTTPS: the certificate in doc/example-site/etc/klind.pem is a 1024-bit RSA
key with an MD5 signature that expired in 2006. OpenSSL 3 at the default
security level of common distributions (tested on Ubuntu) refuses it, so
an HTTPS server using it will not start. Generate your own certificate
instead, for example:

```
openssl req -x509 -newkey rsa:3072 -nodes -keyout key.pem \
    -out cert.pem -days 365 -subj /CN=localhost
```

and point cert_file and key_file at the new files. Use a certificate from
a trusted CA in production.


## Testing

```
python3 build/build.py --test
python3 build/build.py --sanitize --test
```

--test runs, in order:

1. The unit tests (ctest).
2. The regression suite (about a hundred checks), which compiles selected
   production functions verbatim as C99, with warnings treated as errors,
   and runs them under the sanitizers. LibU and network collaborators are
   test doubles; production MD5 is linked directly.
3. The full-server tests (about 500 checks), run twice: once in
   single-process mode and once in the prefork model. They embed the
   test-only site in test/site, start klind on temporary localhost ports
   (IPv4, and IPv6 where the host has it) and check, among others: static
   files, pages, HEAD, ranges and ETags; malformed headers and URIs,
   request smuggling and limits; keep-alive; TLS versions, ciphers,
   renegotiation, client certificates and session tickets; sessions and
   CSRF; access rules, authentication, routes, validation, SSE and
   WebSockets; files, symbolic links and CGI scripts; the sandbox, the key
   manager (whose memory no worker may read) and worker limits; and
   configurations klind must refuse at startup. As root (WSL) they also
   check dropping root, blind_chroot and the key manager run as nobody.
   Under --sanitize, any AddressSanitizer, LeakSanitizer or UBSan report
   fails the run.

The security topics in doc/ (ACCESS, CGI, FILES, KEYMGR, LIMITS, RELEASES,
SANDBOX, SESSIONS, SYSTEMD, TLS) end with what their tests check. Test
certificates and listeners are temporary.

The regression suite also runs without a full build, on Windows or Linux,
with Python 3 and Clang:

```
python test/run_regressions.py --sanitize
```

After a release build, run a bounded libFuzzer test of the real header,
field and utility parsers:

```
python3 test/run_fuzz.py --build .build/release/cmake --runs 10000
```

Fuzz logs, the corpus and any crash inputs are kept in the build folder.

Build-system checks build a site with awkward file names, page includes,
escaped compiler flags, import options and the optional CGI and file-system
suppliers, check incremental rebuilds, and compile and link a program
against the install tree alone (in a temporary directory):

```
python3 test/run_build_checks.py [--generator G] [--make-program P]
```

The fuzz runner uses GNU linker options, so it runs on Linux only.

Release checks build the release files twice and compare them, check the
SBOM and checksums, build the tarball offline and run its unit tests, and
check every workflow's pins and permissions (doc/RELEASES.md; needs PyYAML):

```
python3 test/run_release_checks.py [--no-build]
```

klind under the hardened systemd unit (Linux, as root):

```
sudo python3 test/run_systemd_test.py --build .build/release/cmake
```

Continuous integration (GitHub Actions, .github/workflows) runs on every
push and pull request:

```
build.yml          release and sanitizer builds and tests on Linux;
                   the fuzz test, build-system, release and systemd
                   checks; BSD make (bmake) with GNU make made to
                   fail; native Windows (MinGW): build, HTTP and
                   HTTPS, regressions; macOS weekly and by hand
analysis.yml       clang-tidy
codeql.yml         CodeQL (security-extended queries)
cflite_pr.yml      ClusterFuzzLite on the code a pull request changes
```

and on a schedule: ClusterFuzzLite batch fuzzing with corpus pruning and
coverage (daily), CodeQL and OpenSSF Scorecard (weekly), Coverity Scan
(weekly, once its token is set), and Dependabot for the actions. Release
drafts come from a pushed tag (doc/RELEASES.md). Build logs are uploaded.


## Renamed from KLone

KLone was renamed Klin in 4.0.0. The old names below still work in 4.x and
will be removed in 5.0.0 (TODO lists what goes):

```
KLone                          Klin                 old name in 4.x
kloned, klone (commands)       klind, klin          symlinks (Unix install)
<klone/X.h>, klone_conf.h      <klin/X.h>,          forwarding headers
                               klin_conf.h          (installed too)
klone_die, klone_die_if,       klin_die, ...,       aliases, via the
klone_version, KLONE_VERSION,  KLIN_*               forwarding headers
KLONE_UNCONST, KLONE_FREE ...
-DKLONE_* CMake settings       -DKLIN_*             accepted, with a
                                                    deprecation warning
klone_sid (session cookie)     klin_sid             read, never set
KLONE_CIPHER_KEY (session      KLIN_CIPHER_KEY      read (saved sessions,
variable, environment)                              pages, the klin tool)
.klone pages, /index.klone     .klin, /index.klin   both are pages
/etc/kloned.conf embedded      /etc/klind.conf      used when there is
in the site                                         no /etc/klind.conf
klone_sess_* session files     klin_sess_*          renamed when used
Server: klone/<version>        Server: klin/...     -
```

Unchanged: the .kl1 (and .klx, .klc) page extensions, the `KL1_CLISES_*`
client-session cookies, the configuration file format, the Windows service
name (kloned) and the installed library names (libhttp.a, libutils.a ...).
A kloned.conf given with -f works as it is; only the example and manual
names changed (klind.conf).


## Security and compatibility changes

Klin 4.0.0 is stricter than KLone 3.1.0 by default. These new defaults
may change how an existing deployment behaves; each has a setting (the
ChangeLog lists every change):

```
what changed                              to change it
TLS 1.2 ciphers: forward-secret AEAD      tls_ciphers (doc/TLS.md)
  only; old clients with CBC-only or
  RSA key exchange are refused
the TLS private key is held by a          key_manager no (doc/KEYMGR.md)
  separate process (RSA and EC keys)
workers run under a seccomp sandbox,      sandbox no, sandbox_allow
  with argument checks (sockets, W^X      (doc/SANDBOX.md)
  memory, signals)
worker limits: 1024 MB, 1024 files,       worker_max_memory,
  max_child + 64 processes                worker_max_files,
                                          worker_max_procs; 0: off
                                          (doc/LIMITS.md)
header limits, a header deadline,         max_header_*, idle_timeout,
  connections per client address,         max_conn_per_ip, max_streams
  SSE/WebSocket streams                   (doc/LIMITS.md)
symbolic links are followed only inside   follow_symlinks (doc/FILES.md)
  the document root
CGI: 60 s, 30 s CPU, 512 MB; klind's      the cgi block: timeout,
  environment and the Authorization       max_cpu, max_memory, pass_env,
  field are not passed                    pass_headers (doc/CGI.md)
security headers on every response        security_headers (doc/HEADERS.md)
a session id the client makes up is       none: use session_regenerate_id()
  not taken on                            at login (doc/SESSIONS.md)
klind does not keep root credentials      uid and gid, or allow_root yes
                                          (doc/SYSTEMD.md)
settings out of range stop klind at       max_child 1 to 896, verify_depth
  startup                                 0 to 100 (klind.conf(5))
```

## TLS

- TLS 1.2 is the minimum protocol version; TLS 1.0 and 1.1 are rejected
  and cannot be turned back on (tls_min_version 1.3 raises the minimum).
  TLS compression and renegotiation are disabled.
- OpenSSL 3 is required. Contexts are owned opaque objects accessed through
  public APIs.
- Ephemeral 512-bit RSA and the built-in 1024-bit DH group are gone. Without
  dh_file, OpenSSL selects suitable DH parameters automatically.

Sessions and cookies

- Session IDs carry 128 bits from a cryptographic random source, replacing
  MD5(time, pid, rand()).
- Client-side session MACs default to HMAC-SHA256 (previously SHA-1), use
  checked OpenSSL EVP_MAC calls and compare tags in constant time.
  Client-side session IV and encoded-data lengths are bounds-checked.
  Existing client-side sessions may need to be renewed after upgrading.
  The digest can still be chosen with client.hash_function (klind.conf(5)).
- Session cookies are sent with Path=/, HttpOnly and SameSite=Lax, plus
  Secure on direct TLS connections. Deletion cookies keep the same scope and
  attributes. Third-party login or embedding flows may need a different
  SameSite policy. TLS terminated by a proxy is not inferred from forwarded
  headers, which are untrusted.
- response_set_cookie() keeps its previous behavior for application cookies.
  The new response_set_cookie_ex() takes explicit `RESPONSE_COOKIE_*` flags
  (SECURE, HTTPONLY, SAMESITE_LAX, SAMESITE_STRICT, SAMESITE_NONE).

HTTP request handling

- Rejected: duplicate or overflowing Content-Length, any Transfer-Encoding
  (chunked request bodies are not supported), invalid field names, control
  characters in field values, folded headers, unterminated headers, and
  decoded NUL bytes in URI paths.
- Limits (configurable, doc/LIMITS.md): 100 header fields, 8192 bytes per
  header line, 32768 bytes of headers in total.
- The query string is split off before the path is URL-decoded, so an
  escaped '?' (%3F) and a literal '+' in the path are preserved.

Persistent connections (keep-alive)

- Connections stay open for further requests (HTTP/1.1 by default, HTTP/1.0
  with "Connection: keep-alive"), including pipelined requests. Settings:
  keep_alive, keep_alive_timeout (5 s) and keep_alive_max (100 requests),
  see klind.conf(5). Off in the iterative model (-d) and in CGI mode.
- Responses to HTTP/1.1 requests use an HTTP/1.1 status line. Every body is
  delimited: pages that fit the 4 KB response buffer get a Content-Length,
  longer ones use chunked transfer coding (HTTP/1.1) or end with the close
  (HTTP/1.0). A Content-Length set by a page or CGI is enforced and any
  Transfer-Encoding it sets is replaced.
- HEAD, 204 and 304 responses never carry a body; empty pages no longer
  send a stray newline; the built-in error page has a Content-Length and is
  never appended to a response already under way; a CGI "Status:" line is
  no longer passed to the client; HEAD on an embedded static file works.
- Reading a request body (forms, uploads, pages, CGIs) stops at its end,
  and an unread body is skipped before the next request. Chunked request
  bodies are still rejected. Over TLS, connections end with close_notify.
- In the prefork model a worker holding a kept-alive connection counts as
  busy: while clients wait, the pool doubles every second up to
  prefork.max_child. A worker started beyond prefork.start_child exits after
  prefork.idle_timeout seconds (default 10, 0 = never) without a client, so
  the pool shrinks back after a burst. prefork.max_requests_per_child counts
  connections, not requests.

Utility APIs

- New capacity-aware u_urlncpy_s(), u_hexncpy_s(), u_htmlncpy_s() and
  u_sqlncpy_s() reject undersized destinations. Internal callers use them;
  the legacy functions remain for compatibility and still require correctly
  sized caller buffers.
- SQL quote encoding now processes the whole input instead of truncating
  after quotes.
- Fixed out-of-bounds reads in URL decoding and substring search, invalid
  hex input handling, undefined behavior in MD5 and path normalization,
  seeking to the end of memory streams, and negative read results being
  treated as large lengths.

Reliability

- The SIGCHLD handler no longer touches the server object, which may be
  unset during startup or shutdown.
- Memory leaks found by LeakSanitizer in klind shutdown, the page
  translator and the klin tool are fixed.


## Known limitations

- Built and tested on Linux (including WSL), with Ninja and with BSD make,
  and on macOS 15 (Apple Silicon, Apple Clang, Homebrew OpenSSL 3). `*BSD`
  is untested. On macOS, LeakSanitizer is unavailable, so the sanitizer
  build checks memory errors and undefined behavior only.
- Native Windows builds with MinGW-w64 (MSYS2 UCRT64: gcc, cmake, ninja,
  openssl, zlib, libsystre) and runs the single-process server model: no
  prefork or fork models, no TLS key manager (key_manager defaults to no),
  no sandbox. CI builds it, serves HTTP and HTTPS and runs the regression
  suite; the full server tests need fork(2) and run on Linux and macOS.
  MSVC is not supported: LibU needs POSIX headers. WSL remains the way to
  run everything on Windows.
- C++ pages (.klx) are not supported; the import stops with an error.
  LibU's SCTP support is off by default (LIBU_ENABLE_SCTP).
- The pinned LibU dependency still has some compiler warnings; Klin's own
  code builds without warnings (GCC, Clang, Apple Clang).
- "Expect: 100-continue" is not answered; clients send the body after their
  own timeout.
- Fuzzing runs for half an hour a day per sanitizer (ClusterFuzzLite);
  the project is not in OSS-Fuzz, whose campaigns are much longer.
- Under the sandbox, a CGI script cannot signal its own process group
  with kill 0 (doc/SANDBOX.md).


## Removed in 4.0.0

The MaKL configure script and GNU makefiles, the skin/ packaging system, the
CyaSSL backend, OpenSSL TLS-PSK support, the Kilt URL module and the old
LibU 1.x unit tests (their checks are covered by the regression suite) were
removed.
'klin -c import' no longer writes autogen.mk or autogen.dps. KLone 3.1.0 in
the original KoanLogic repository still has them.
