#!/usr/bin/env python3
# Copyright (c) 2026 Daniel Tillett. Part of Klin; see the LICENSE file.
"""Build a test-only embedded page and exercise the real HTTP/TLS daemon."""
import argparse
import base64
import hashlib
import json
from http.cookies import SimpleCookie
import os
from pathlib import Path
import re
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]


def openssl_cli():
    """The openssl tool of the OpenSSL klind is built with: OPENSSL_ROOT_DIR's
    where set (on macOS /usr/bin/openssl is LibreSSL, which, for one, does
    not resume TLS 1.3 sessions from a file), else the one on PATH."""
    root = os.environ.get("OPENSSL_ROOT_DIR")
    if root and os.access(Path(root) / "bin" / "openssl", os.X_OK):
        return str(Path(root) / "bin" / "openssl")
    return "openssl"


OPENSSL = openssl_cli()


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def exchange(port, data, context=None, half_close=False, close=True, timeout=3, host="127.0.0.1"):
    """Send one request and read until the server closes the connection.

    With close, "Connection: close" is added so a persistent (keep-alive)
    connection is not held open by the server after the response."""
    if close and b"\r\n" in data:
        line, rest = data.split(b"\r\n", 1)
        data = line + b"\r\nConnection: close\r\n" + rest
    with socket.create_connection((host, port), timeout=timeout) as raw:
        connection = context.wrap_socket(raw, server_hostname="localhost") if context else raw
        with connection:
            connection.sendall(data)
            if half_close:
                connection.shutdown(socket.SHUT_WR)
            chunks = []
            while True:
                try:
                    chunk = connection.recv(65536)
                except ConnectionResetError:
                    break
                if not chunk:
                    break
                chunks.append(chunk)
            return b"".join(chunks)


class Connection:
    """A client connection that reads responses by their HTTP framing, so
    several requests can be sent on it (keep-alive, pipelining)."""

    def __init__(self, port, context=None):
        raw = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock = context.wrap_socket(raw, server_hostname="localhost") if context else raw
        self.buf = b""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.sock.close()

    def send(self, data):
        self.sock.sendall(data)

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError("connection closed")
        self.buf += chunk

    def _line(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def _take(self, size):
        while len(self.buf) < size:
            self._fill()
        data, self.buf = self.buf[:size], self.buf[size:]
        return data

    def response(self, head=False):
        """Read one response: (status line, {lowercase name: value}, body)."""
        status = self._line().decode()
        headers = {}
        while line := self._line():
            name, _, value = line.decode().partition(":")
            headers[name.strip().lower()] = value.strip()
        code = int(status.split()[1])
        if head or code in (204, 304) or 100 <= code < 200:
            body = b""
        elif headers.get("transfer-encoding", "").lower() == "chunked":
            body = b""
            while size := int(self._line().split(b";")[0], 16):
                body += self._take(size)
                if self._take(2) != b"\r\n":
                    raise ValueError("chunk not followed by CRLF")
            while self._line():
                pass  # trailer fields
        elif "content-length" in headers:
            body = self._take(int(headers["content-length"]))
        else:
            body = self.buf
            while True:
                try:
                    chunk = self.sock.recv(65536)
                except (ConnectionResetError, ssl.SSLError):
                    break
                if not chunk:
                    break
                body += chunk
            self.buf = b""
        return status, headers, body

    def closed(self):
        """True if the server closes the connection (nothing else is sent)."""
        if self.buf:
            return False
        try:
            return self.sock.recv(1) == b""
        except socket.timeout:
            return False
        except (ConnectionResetError, ssl.SSLError):
            return True


def client_context(version):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE  # ephemeral local test certificate
    context.minimum_version = version
    context.maximum_version = version
    return context


def run_build_steps(commands, env, log_path):
    # BSD make compares whole-second file times: what is rebuilt in the
    # second the previous build ended (the library, klind) would look no
    # newer than it, and the old site would stay linked in
    time.sleep(1.1)
    with log_path.open("a") as log:
        for command, cwd in commands:
            result = subprocess.run(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                log.flush()
                raise RuntimeError(log_path.read_text()[-6000:])


def cmake_cache_value(build, name):
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith(name + ":"):
            return line.split("=", 1)[1]
    return ""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True, help="CMake build directory")
    parser.add_argument("--prefork", action="store_true", help="exercise the normal multiprocess server model")
    args = parser.parse_args()
    tree = args.build.resolve()
    if not tree.is_relative_to(ROOT / ".build"):
        parser.error("Tests modify only generated builds inside this repository's .build")
    env = os.environ.copy()
    output = tree.parent
    log_path = output / "test-build.log"
    log_path.write_text("")
    restore = None
    try:
        # Embed the test site (with no import options) for this run only.
        # The original settings are restored afterwards, even if the
        # fixture fails to build, so the test page never stays in klind.
        settings = {name: cmake_cache_value(tree, name)
                    for name in ("KLIN_SITE_DIR", "KLIN_IMPORT_ARGS")}
        rebuild = ["cmake", "--build", str(tree), "--target", "klind"]
        # 'cmake -D... <existing build dir>' reconfigures with the cached source
        restore = [(["cmake", *[f"-D{k}={v}" for k, v in settings.items()], str(tree)], tree),
                   (rebuild, tree)]
        run_build_steps([(["cmake", f"-DKLIN_SITE_DIR={ROOT / 'test/site'}",
                           "-DKLIN_IMPORT_ARGS=-Z;*range-z.txt", str(tree)], tree),
                         (rebuild, tree)], env, log_path)
        daemon = tree / "bin" / cmake_cache_value(tree, "KLIN_DAEMON_NAME")
        run_server_checks(args, env, output, daemon)
    finally:
        if restore:
            run_build_steps(restore, env, log_path)


def run_keep_alive_checks(args, check, plain, secure, request):
    """Persistent connections: several requests per connection, pipelining,
    response framing (Content-Length, chunked, bodyless) and closing."""
    hello = b"klin integration test\n"

    def big_page(body):
        lines = body.strip().split(b"\n")
        return (len(lines) == 2000 and lines[0] == b"line 0000 of a long dynamic page"
                and lines[-1] == b"line 1999 of a long dynamic page")

    def get(path, method=b"GET", extra=b""):
        return method + b" " + path + b" HTTP/1.1\r\nHost: localhost\r\n" + extra + b"\r\n"

    # A one-byte write buffer splits the size lines, payloads and trailers.
    reply = exchange(plain, get(b"/chunked.kl1"))
    head, _, wire = reply.partition(b"\r\n\r\n")
    expected = (b"3\r\nabc\r\n1000\r\n" + b"x" * 4096 +
                b"\r\n5\r\nxxxxx\r\n1\r\n\n\r\n0\r\n\r\n")  # final page newline
    check(b"\r\ntransfer-encoding: chunked" in head.lower() and wire == expected,
          f"Chunk framing with a one-byte write buffer: {head!r}, {len(wire)} body bytes")

    if not args.prefork:
        # the iterative server (-d) would be blocked by an idle client
        with Connection(plain) as conn:
            conn.send(request)
            status, headers, body = conn.response()
            check(body == hello and headers.get("connection") == "close" and conn.closed(),
                  f"Iterative server kept the connection open: {status} {headers}")
        return

    with Connection(plain) as conn:
        for _ in range(2):
            conn.send(request)
            status, headers, body = conn.response()
            check(status.startswith("HTTP/1.1 200") and body == hello and "connection" not in headers,
                  f"Keep-alive GET failed: {status} {headers} {body[:80]!r}")

        # pipelined requests, each response delimited differently
        conn.send(get(b"/static/hello.txt") + get(b"/session.kl1") + get(b"/big.kl1") +
                  get(b"/embedded.txt", b"HEAD") + get(b"/nocontent.kl1") + get(b"/missing.txt") +
                  get(b"/embedded.txt"))
        status, headers, body = conn.response()
        check(body == hello, f"Pipelined static file: {status} {body[:80]!r}")
        status, headers, body = conn.response()
        check(b"new" in body and headers.get("content-length") == str(len(body)),
              f"Dynamic page without Content-Length: {status} {headers}")
        status, headers, body = conn.response()
        check(headers.get("transfer-encoding") == "chunked" and big_page(body),
              f"Chunked dynamic page: {status} {headers} {len(body)}")
        status, headers, body = conn.response(head=True)
        check(status.startswith("HTTP/1.1 200") and headers.get("content-length") == "21",
              f"Embedded static HEAD: {status} {headers}")
        status, headers, body = conn.response()
        check(status.startswith("HTTP/1.1 204") and "content-length" not in headers and
              "transfer-encoding" not in headers, f"204 response: {status} {headers}")
        status, headers, body = conn.response()
        check(status.startswith("HTTP/1.1 404") and headers.get("content-length") == str(len(body)),
              f"Error page framing: {status} {headers}")
        status, headers, body = conn.response()
        check(body == b"embedded static file\n", f"Response after 204/404: {status} {body!r}")

        # an unread request body is skipped before the next request
        conn.send(b"POST /static/hello.txt HTTP/1.1\r\nHost: localhost\r\n"
                  b"Content-Type: application/octet-stream\r\nContent-Length: 5000\r\n\r\n" +
                  b"x" * 5000 + request)
        conn.response()
        status, headers, body = conn.response()
        check(body == hello, f"Request after an unread body: {status} {body[:80]!r}")

        # the client closes
        conn.send(get(b"/static/hello.txt", extra=b"Connection: close\r\n"))
        status, headers, body = conn.response()
        check(body == hello and headers.get("connection") == "close" and conn.closed(),
              f"Connection: close not honoured: {headers}")

    # bodies larger than the connection's write buffer skip it (passthru),
    # still within the announced Content-Length
    large = bytes(ord("a") + i % 26 for i in range(150000))
    for port, context in [(plain, None), (secure, client_context(ssl.TLSVersion.TLSv1_3))]:
        with Connection(port, context) as conn:
            for _ in range(2):
                conn.send(get(b"/large.kl1") + get(b"/static/hello.txt"))
                status, headers, body = conn.response()
                check(headers.get("content-length") == "150000" and body == large,
                      f"Large body on port {port}: {status} {headers} {len(body)}")
                status, headers, body = conn.response()
                check(body == hello, f"Request after a large body on port {port}: {status}")
        with Connection(port, context) as conn:
            conn.send(get(b"/large.kl1?m=long"))
            status, headers, body = conn.response()
            check(headers.get("content-length") == "149990" and body == large[:149990] and
                  conn.closed(),
                  f"Large body longer than Content-Length on port {port}: {headers} {len(body)}")
        with Connection(port, context) as conn:
            conn.send(get(b"/large.kl1?m=short"))
            try:
                status, headers, body = conn.response()
                short = False
            except EOFError:
                short = len(conn.buf) == 150000
            check(short, f"Large body shorter than Content-Length not ended by a close on port {port}")

    # HTTP/1.0: closed unless the client asks for keep-alive
    with Connection(plain) as conn:
        conn.send(b"GET /static/hello.txt HTTP/1.0\r\n\r\n")
        status, headers, body = conn.response()
        check(status.startswith("HTTP/1.0 200") and body == hello and conn.closed(),
              f"HTTP/1.0 connection not closed: {status} {headers}")
    with Connection(plain) as conn:
        for _ in range(2):
            conn.send(b"GET /static/hello.txt HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
            status, headers, body = conn.response()
            check(body == hello and headers.get("connection") == "keep-alive",
                  f"HTTP/1.0 keep-alive: {status} {headers}")
        conn.send(b"GET /big.kl1 HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
        status, headers, body = conn.response()
        check(big_page(body) and "transfer-encoding" not in headers and conn.closed(),
              f"HTTP/1.0 body of unknown length must end with the close: {headers}")

    # a connection kept open for more than a second, but busy only for short
    # requests, must not make the worker ask for a replacement and exit
    pids, dropped = set(), False
    with Connection(plain) as conn:
        try:
            for _ in range(7):
                conn.send(get(b"/pid.kl1"))
                status, headers, body = conn.response()
                pids.add(body.strip())
                time.sleep(0.4)
        except EOFError:
            dropped = True
    check(not dropped, "Kept-alive connection dropped after a second (prefork watchdog)")
    worker = pids.pop().decode() if len(pids) == 1 else ""
    time.sleep(1)
    state = subprocess.run(["ps", "-o", "stat=", "-p", worker or "0"],
                           stdout=subprocess.PIPE, text=True).stdout.strip()
    check(worker.isdigit() and state and not state.startswith("Z"),
          f"Worker retired after an idle keep-alive connection: pid {worker} state {state!r}")

    # a client waiting while every worker holds a kept-alive connection gets a
    # new worker after about a second, not when that connection ends
    stop, held_errors, waited, spare = threading.Event(), [], None, ""

    def keep_using(conn):
        try:
            while not stop.is_set():
                conn.send(request)
                conn.response()
                time.sleep(0.3)
        except (OSError, EOFError) as e:
            held_errors.append(e)

    with Connection(plain) as held, Connection(plain) as waiting:
        held.send(request)
        held.response()
        user = threading.Thread(target=keep_using, args=(held,))
        user.start()
        try:
            start = time.time()
            waiting.send(get(b"/pid.kl1"))
            status, headers, body = waiting.response()
            if body.strip().isdigit():
                waited, spare = time.time() - start, body.strip().decode()
        except (OSError, EOFError):
            pass
        finally:
            stop.set()
            user.join()
    check(waited is not None and waited < 3,
          f"Client not served while another kept a connection in use (waited {waited})")
    check(not held_errors, f"Kept-alive connection dropped for a waiting client: {held_errors}")

    # the worker started for the waiting client is a spare: it exits after
    # prefork.idle_timeout (1 s) without a client, so the pool shrinks back
    time.sleep(3)
    state = subprocess.run(["ps", "-o", "stat=", "-p", spare or "0"],
                           stdout=subprocess.PIPE, text=True).stdout.strip()
    check(spare and (not state or state.startswith("Z")),
          f"Idle spare worker {spare} still running: state {state!r}")

    # an idle connection is closed after keep_alive_timeout (1 s)
    with Connection(plain) as conn:
        conn.send(request)
        conn.response()
        time.sleep(2)
        check(conn.closed(), "Idle keep-alive connection not closed")

    # TLS: several requests on one connection, then close_notify
    with Connection(secure, client_context(ssl.TLSVersion.TLSv1_3)) as conn:
        for path in [b"/static/hello.txt", b"/big.kl1", b"/static/hello.txt"]:
            conn.send(get(path))
            status, headers, body = conn.response()
            check(status.startswith("HTTP/1.1 200") and body, f"TLS keep-alive {path!r}: {status}")
        conn.send(get(b"/static/hello.txt", extra=b"Connection: close\r\n"))
        conn.response()
        check(conn.closed(), "TLS connection not closed")

    # TLS: a request that stops halfway is closed after idle_timeout (2 s),
    # although OpenSSL waits for the rest of it (the socket is non-blocking)
    with Connection(secure, client_context(ssl.TLSVersion.TLSv1_3)) as conn:
        conn.send(b"GET /static/hello.txt HTTP/1.1\r\nHost: loc")
        time.sleep(3)
        check(conn.closed(), "Stalled TLS request not closed")

    # TLS: a client that never starts the handshake is dropped after 10 s
    with socket.create_connection(("127.0.0.1", secure), timeout=15) as raw:
        start = time.time()
        try:
            dropped = raw.recv(1) == b""
        except (ConnectionResetError, socket.timeout):
            dropped = False
        check(dropped and 8 < time.time() - start < 14,
              f"Stalled TLS handshake not dropped in time ({time.time() - start:.1f} s)")


PRIVATE, OLD = "Private Area", "Old Clients"


def auth_users(klin, temp):
    """The "user" blocks of the auth configuration, made by klin -c passwd
    (a few PBKDF2 iterations: the tests check the format, not the cost)."""
    def user(name, password, roles, *realms, md5=False):
        command = [str(klin), "-c", "passwd", "-n", "1000"]
        for realm in realms:
            command += ["-r", realm]
        if md5:
            command.append("-m")
        block = subprocess.run(command + [name], input=password + "\n", text=True,
                               capture_output=True, check=True).stdout
        return block.replace("    roles\n", f"    roles {roles}\n")

    inline = (user("alice", "wonder land", "admin staff", PRIVATE, OLD, md5=True) +
              user("bob", "builder", "staff", PRIVATE))
    users_file = temp / "users.conf"
    users_file.write_text(user("carol", "c4rol!", "admin", PRIVATE))
    return inline, users_file


def auth_config(inline, users_file):
    return f"""auth
    {{
        nonce_lifetime 3
        users_file {users_file}
{inline}
        realm
        {{
            name {PRIVATE}
            path /static/secret*
            path /whoami.kl1
            scheme any
            roles admin
        }}
        realm
        {{
            name {OLD}
            path /static/old*
            md5 yes
        }}
    }}"""


def digest_header(challenge, user, password, uri, realm=PRIVATE, alg="SHA-256",
                  method="GET", nc="00000001", cnonce="0a4f113b"):
    nonce = re.search(r'nonce="([^"]+)"', challenge).group(1)
    h = (lambda s: hashlib.sha256(s.encode()).hexdigest()) if alg == "SHA-256" else \
        (lambda s: hashlib.md5(s.encode()).hexdigest())
    ha1, ha2 = h(f"{user}:{realm}:{password}"), h(f"{method}:{uri}")
    response = h(f"{ha1}:{nonce}:{nc}:{cnonce}:auth:{ha2}")
    return (f'Authorization: Digest username="{user}", realm="{realm}", nonce="{nonce}", '
            f'uri="{uri}", algorithm={alg}, response="{response}", qop=auth, nc={nc}, '
            f'cnonce="{cnonce}"\r\n').encode()


def run_auth_checks(args, check, plain, secure):
    """HTTP authentication (doc/AUTH.md): Digest and Basic, realms, roles."""
    tls = client_context(ssl.TLSVersion.TLSv1_3)

    def get(port, path, extra=b"", context=None):
        reply = exchange(port, b"GET " + path.encode() + b" HTTP/1.1\r\nHost: localhost\r\n" +
                         extra + b"\r\n", context)
        head, _, body = reply.partition(b"\r\n\r\n")
        head = head.decode("latin-1")
        challenges = [line.split(":", 1)[1].strip() for line in head.split("\r\n")
                      if line.lower().startswith("www-authenticate:")]
        return head.split("\r\n")[0], challenges, body

    status, challenges, _ = get(plain, "/static/hello.txt")
    check(" 200 " in status and not challenges, "Unprotected page asked for credentials")

    # plain HTTP: Digest SHA-256 only (Basic would send the password)
    status, challenges, _ = get(plain, "/static/secret.txt")
    check(" 401 " in status and len(challenges) == 1 and
          challenges[0].startswith(f'Digest realm="{PRIVATE}"') and
          "algorithm=SHA-256" in challenges[0] and 'qop="auth"' in challenges[0],
          f"Wrong challenge: {status} {challenges}")
    first = challenges[0]
    status, _, body = get(plain, "/static/secret.txt",
                          digest_header(first, "alice", "wonder land", "/static/secret.txt"))
    check(" 200 " in status and body == b"top secret\n", f"Digest login failed: {status}")
    # the same request again (the same nonce count) is a replay: stale=true,
    # so a real client retries with a new nonce; a higher count is accepted.
    # The counts are kept per process, so only the iterative server, with
    # one, refuses the replay every time (doc/AUTH.md)
    def alice(nc):
        return get(plain, "/static/secret.txt",
                   digest_header(first, "alice", "wonder land", "/static/secret.txt", nc=nc))
    status, challenges, _ = alice("00000001")
    if not args.prefork:
        check(" 401 " in status and "stale=true" in challenges[0],
              f"Replayed Digest request accepted: {status} {challenges}")
    status, _, body = alice("00000002")
    check(" 200 " in status and body == b"top secret\n", f"A higher nonce count refused: {status}")
    if not args.prefork:
        for nc in ["00000002", "00000001"]:
            status, challenges, _ = alice(nc)
            check(" 401 " in status and "stale=true" in challenges[0],
                  f"Digest nonce count {nc} accepted after 00000002: {status}")
    for user, password, uri, want in [
            ("alice", "wonder lanD", "/static/secret.txt", " 401 "),    # wrong password
            ("nobody", "wonder land", "/static/secret.txt", " 401 "),   # unknown user
            ("alice", "wonder land", "/static/hello.txt", " 401 "),     # digest of another URI
            ("bob", "builder", "/static/secret.txt", " 403 "),          # no admin role
            ("carol", "c4rol!", "/static/secret.txt", " 200 ")]:        # from users_file
        status, _, _ = get(plain, "/static/secret.txt", digest_header(first, user, password, uri))
        check(want in status, f"Digest {user} {uri}: {status}, expected {want.strip()}")
    status, _, _ = get(plain, "/static/secret.txt",
                       digest_header(first, "alice", "wonder land", "/static/secret.txt",
                                     alg="MD5"))
    check(" 401 " in status, "MD5 Digest accepted in a realm without md5")
    forged = first.replace('nonce="', 'nonce="0', 1).replace('0', '1', 1)
    status, _, _ = get(plain, "/static/secret.txt",
                       digest_header(forged, "alice", "wonder land", "/static/secret.txt"))
    check(" 401 " in status, "Forged nonce accepted")
    basic = b"Authorization: Basic " + base64.b64encode(b"alice:wonder land") + b"\r\n"
    status, _, _ = get(plain, "/static/secret.txt", basic)
    check(" 401 " in status, "Basic accepted over plain HTTP")

    # the page sees the user (a new cnonce: within the same second the
    # nonce is the one carol has just used)
    status, challenges, _ = get(plain, "/whoami.kl1")
    status, _, body = get(plain, "/whoami.kl1",
                          digest_header(challenges[0], "carol", "c4rol!", "/whoami.kl1",
                                        cnonce="5e1f2a3b"))
    check(" 200 " in status and body.strip() == b"user=carol", f"Page did not see the user: {body!r}")

    # a realm that also offers MD5, and requires no role
    status, challenges, _ = get(plain, "/static/old.txt")
    check(" 401 " in status and [c.split("algorithm=")[1].split(",")[0] for c in challenges] ==
          ["SHA-256", "MD5"], f"Wrong MD5 realm challenges: {challenges}")
    status, _, body = get(plain, "/static/old.txt",
                          digest_header(challenges[1], "alice", "wonder land", "/static/old.txt",
                                        realm=OLD, alg="MD5"))
    check(" 200 " in status and body == b"old client\n", f"MD5 Digest failed: {status}")

    # TLS: Basic too
    status, challenges, _ = get(secure, "/static/secret.txt", context=tls)
    check(" 401 " in status and challenges[-1] == f'Basic realm="{PRIVATE}", charset="UTF-8"',
          f"No Basic challenge over TLS: {challenges}")
    status, _, body = get(secure, "/static/secret.txt", basic, tls)
    check(" 200 " in status and body == b"top secret\n", f"Basic login failed: {status}")
    for credentials, want in [(b"alice:wrong", " 401 "), (b"bob:builder", " 403 "),
                              (b"alice", " 401 "), (b"carol:c4rol!", " 200 ")]:
        status, _, _ = get(secure, "/static/secret.txt",
                           b"Authorization: Basic " + base64.b64encode(credentials) + b"\r\n", tls)
        check(want in status, f"Basic {credentials!r}: {status}, expected {want.strip()}")
    # the verified-credentials cache keeps no wrong password
    status, _, _ = get(secure, "/static/secret.txt", basic, tls)
    status2, _, _ = get(secure, "/static/secret.txt",
                        b"Authorization: Basic " + base64.b64encode(b"alice:wonder lanD") + b"\r\n", tls)
    check(" 200 " in status and " 401 " in status2, "Cached Basic credentials accepted a wrong password")

    # an expired nonce: stale=true, so the client retries without asking
    time.sleep(4)
    status, challenges, _ = get(plain, "/static/secret.txt",
                                digest_header(first, "alice", "wonder land", "/static/secret.txt"))
    check(" 401 " in status and "stale=true" in challenges[0], f"Expired nonce not stale: {challenges}")

    if not args.prefork:
        # In the iterative server both backends run in the same process.
        # Hold an HTTPS challenge while the plain-HTTP table fills.
        path = "/static/secret.txt"
        _, other, _ = get(secure, path, context=tls)
        _, challenges, _ = get(plain, path)
        original = challenges[0]

        def fill(challenge, cnonce, nc="00000001"):
            return digest_header(challenge, "alice", "wonder land", path, cnonce=cnonce, nc=nc)

        status, _, _ = get(plain, path, fill(original, "to-evict"))
        check(" 200 " in status, f"Digest login before filling the table failed: {status}")
        _, challenges, _ = get(plain, path)
        current = challenges[0]
        for i in range(129):
            status, _, _ = get(plain, path, fill(current, f"fill-{i}"))
            check(" 200 " in status, f"Digest table fill failed at slot {i}: {status}")
        status, challenges, _ = get(plain, path, fill(original, "to-evict", "00000002"))
        check(" 401 " in status and "stale=true" in challenges[0],
              f"Digest accepted an evicted nonce count: {status} {challenges}")
        status, fresh, _ = get(plain, path, fill(current, "new-client"))
        check(" 401 " in status and "stale=true" in fresh[0],
              f"Digest accepted an untracked count below the replay floor: {status} {fresh}")
        status, _, _ = get(plain, path, fill(fresh[0], "new-client"))
        check(" 200 " in status, f"Immediate Digest retry after eviction failed: {status}")
        status, _, _ = get(secure, path, fill(other[0], "other-backend"), tls)
        check(" 200 " in status, f"HTTP replay eviction affected HTTPS: {status}")


SECURITY_HEADERS = """security_headers
    {
        x_frame_options DENY
        referrer_policy no
        content_security_policy default-src 'self'; frame-ancestors 'none'
    }"""


def run_header_checks(check, plain, client, secure):
    """Security headers (doc/HEADERS.md): the defaults, a vhost's own values,
    HSTS over HTTPS only, and on error pages too."""
    def fields(port, path, context=None):
        reply = exchange(port, b"GET " + path + b" HTTP/1.1\r\nHost: localhost\r\n\r\n", context)
        head = reply.partition(b"\r\n\r\n")[0].decode("latin-1").split("\r\n")
        found = {}
        for line in head[1:]:
            name, _, value = line.partition(":")
            found.setdefault(name.strip().lower(), []).append(value.strip())
        return head[0], found

    defaults = {"x-content-type-options": ["nosniff"], "x-frame-options": ["SAMEORIGIN"],
                "referrer-policy": ["strict-origin-when-cross-origin"]}
    for path in [b"/static/hello.txt", b"/pid.kl1", b"/missing.txt"]:
        status, found = fields(plain, path)
        check(all(found.get(k) == v for k, v in defaults.items()),
              f"Default security headers missing on {path!r} ({status}): {found}")
        check("strict-transport-security" not in found and "content-security-policy" not in found,
              f"HSTS or CSP sent by default over HTTP: {found}")

    status, found = fields(secure, b"/static/hello.txt", client_context(ssl.TLSVersion.TLSv1_3))
    check(found.get("strict-transport-security") == ["max-age=31536000"] and
          found.get("x-content-type-options") == ["nosniff"], f"No HSTS over HTTPS: {found}")

    for path in [b"/static/hello.txt", b"/missing.txt"]:
        status, found = fields(client, path)
        check(found.get("x-frame-options") == ["DENY"] and "referrer-policy" not in found and
              found.get("content-security-policy") == ["default-src 'self'; frame-ancestors 'none'"]
              and found.get("x-content-type-options") == ["nosniff"],
              f"Configured security headers not sent on {path!r}: {found}")


def run_conditional_checks(check, plain, files):
    """ETag, If-None-Match and Range on embedded files (one stored as is,
    one compressed at import: ranges are cut after it is uncompressed)."""
    def get(path, extra=b"", method=b"GET"):
        reply = exchange(plain, method + b" " + path + b" HTTP/1.1\r\nHost: localhost\r\n" +
                         extra + b"\r\n")
        head, _, body = reply.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        fields = {}
        for line in lines[1:]:
            name, _, value = line.partition(":")
            fields[name.strip().lower()] = value.strip()
        return int(lines[0].split()[1]), fields, body

    for name in ["range.txt", "range-z.txt"]:
        path = b"/" + name.encode()
        data = (ROOT / "test/site/www" / name).read_bytes()
        size = len(data)
        status, fields, body = get(path)
        etag = fields.get("etag", "")
        check(status == 200 and body == data and fields.get("accept-ranges") == "bytes" and
              re.fullmatch(r'"[0-9a-f]{32}"', etag), f"{name}: no ETag: {status} {fields}")
        check(etag == f'"{hashlib.md5(data).hexdigest()}"', f"{name}: ETag is not the content's")

        for inm in [etag, f"W/{etag}", f'"nope", {etag}', "*"]:
            status, fields, body = get(path, f"If-None-Match: {inm}\r\n".encode())
            check(status == 304 and body == b"" and fields.get("etag") == etag,
                  f"{name}: If-None-Match {inm}: {status}")
        status, _, body = get(path, b'If-None-Match: "other"\r\n')
        check(status == 200 and body == data, f"{name}: a different ETag got {status}")
        # If-None-Match decides over If-Modified-Since
        status, _, _ = get(path, b'If-None-Match: "other"\r\n'
                           b"If-Modified-Since: Fri, 01 Jan 2100 00:00:00 GMT\r\n")
        check(status == 200, f"{name}: If-Modified-Since won over If-None-Match ({status})")

        for spec, first, last in [("0-99", 0, 99), ("100-", 100, size - 1), ("-50", size - 50, size - 1),
                                  (f"{size - 10}-{size + 500}", size - 10, size - 1),
                                  ("5000-5000", 5000, 5000), (f"-{size + 9}", 0, size - 1)]:
            status, fields, body = get(path, f"Range: bytes={spec}\r\n".encode())
            check(status == 206 and body == data[first:last + 1] and
                  fields.get("content-range") == f"bytes {first}-{last}/{size}" and
                  fields.get("content-length") == str(last - first + 1),
                  f"{name}: Range {spec}: {status} {fields.get('content-range')} {len(body)}")
        for spec in [f"{size}-", f"{size + 5}-{size + 9}", "-0"]:
            status, fields, body = get(path, f"Range: bytes={spec}\r\n".encode())
            check(status == 416 and body == b"" and fields.get("content-range") == f"bytes */{size}",
                  f"{name}: unsatisfiable Range {spec}: {status} {fields}")
        for spec in ["bytes=0-1,5-6", "bytes=5-2", "items=0-1", "bytes=x-1", "bytes=-"]:
            status, _, body = get(path, f"Range: {spec}\r\n".encode())
            check(status == 200 and body == data, f"{name}: Range {spec} not ignored: {status}")
        status, _, body = get(path, f"Range: bytes=0-9\r\nIf-Range: {etag}\r\n".encode())
        check(status == 206 and body == data[:10], f"{name}: If-Range with the ETag: {status}")
        status, _, body = get(path, b'Range: bytes=0-9\r\nIf-Range: "changed"\r\n')
        check(status == 200 and body == data, f"{name}: If-Range with another ETag: {status}")
        status, fields, body = get(path, b"Range: bytes=0-9\r\n", b"HEAD")
        check(body == b"", f"{name}: HEAD with Range sent a body")

    # pages have no ETag (their output changes)
    status, fields, _ = get(b"/pid.kl1")
    check("etag" not in fields and "accept-ranges" not in fields, "A page got an ETag")

    # sent as stored (send_enc_deflate): another representation, another
    # ETag, and no ranges of the compressed bytes
    data = (ROOT / "test/site/www/range-z.txt").read_bytes()
    deflate = b"Accept-Encoding: deflate\r\n"
    reply = exchange(files, b"GET /range-z.txt HTTP/1.1\r\nHost: localhost\r\n" + deflate +
                     b"Range: bytes=0-9\r\n\r\n")
    head, _, body = reply.partition(b"\r\n\r\n")
    tag = re.search(rb'(?im)^etag: *("[0-9a-f]{32}-deflate")', head)
    check(b" 200 " in head.split(b"\r\n")[0] and b"content-encoding: deflate" in head.lower() and
          tag and b"accept-ranges" not in head.lower() and
          zlib.decompress(body) == data,      # zlib format, as HTTP defines it
          f"Deflated representation: {head!r}")
    if tag:
        reply = exchange(files, b"GET /range-z.txt HTTP/1.1\r\nHost: localhost\r\n" + deflate +
                         b"If-None-Match: " + tag.group(1) + b"\r\n\r\n")
        check(b" 304 " in reply.split(b"\r\n")[0], "If-None-Match of the deflated ETag")


class EventStream:
    """An SSE client: reads the header, then events as they arrive, noting
    when each one came (chunked or, on HTTP/1.0, until the close)."""

    def __init__(self, port, path, extra=b"", context=None, version=b"HTTP/1.1"):
        raw = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock = context.wrap_socket(raw, server_hostname="localhost") if context else raw
        self.sock.sendall(b"GET " + path + b" " + version + b"\r\nHost: localhost\r\n" +
                          b"Accept: text/event-stream\r\n" + extra + b"\r\n")
        self.buf, self.text, self.ended = b"", "", False
        while b"\r\n\r\n" not in self.buf:
            self._recv()
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        self.status = lines[0]
        self.fields = {}
        for line in lines[1:]:
            name, _, value = line.partition(":")
            self.fields[name.strip().lower()] = value.strip()
        self.chunked = self.fields.get("transfer-encoding") == "chunked"

    def _recv(self):
        data = self.sock.recv(65536)
        if not data:
            raise EOFError
        self.buf += data

    def _decode(self):
        if not self.chunked:
            self.text += self.buf.decode()
            self.buf = b""
            return
        while b"\r\n" in self.buf:
            size_line, rest = self.buf.split(b"\r\n", 1)
            size = int(size_line.split(b";")[0], 16)
            if len(rest) < size + 2:
                return
            if size == 0:
                self.ended = True
            self.text += rest[:size].decode()
            self.buf = rest[size + 2:]

    def event(self):
        """The next event as {field: value} (data lines joined), or None at
        the end of the stream."""
        while True:
            self._decode()
            if "\n\n" in self.text:
                block, self.text = self.text.split("\n\n", 1)
                event = {}
                for line in block.split("\n"):
                    name, _, value = line.partition(":")
                    value = value[1:] if value.startswith(" ") else value
                    event[name] = event[name] + "\n" + value if name in event else value
                return event
            if self.ended:
                return None
            try:
                self._recv()
            except EOFError:
                return None

    def close(self):
        self.sock.close()


def run_sse_checks(args, check, plain, secure, temp):
    """Server-Sent Events (doc/SSE.md): header, events sent as written,
    reconnecting with Last-Event-ID, HTTP/1.0, TLS, and a page that sees
    the client go away."""
    stream = EventStream(plain, b"/sse.kl1")
    check(" 200 " in stream.status and stream.fields.get("content-type") == "text/event-stream" and
          stream.chunked and stream.fields.get("cache-control") == "no-store",
          f"SSE header: {stream.status} {stream.fields}")
    start = time.monotonic()
    first = stream.event()
    times, events, blocks = [], [], []
    while (event := stream.event()) is not None:
        blocks.append(event)
        if "id" in event:
            events.append(event)
            times.append(time.monotonic() - start)
    stream.close()
    check(first == {"": "klin"}, f"No opening comment: {first}")
    check(blocks and blocks[0] == {"retry": "1500"}, f"No retry field: {blocks[:1]}")
    ticks = [e for e in events if e.get("event") == "tick"]
    check([e["id"] for e in ticks] == ["1", "2", "3"] and
          ticks[0]["data"] == "event 1\nsecond line", f"SSE events: {events}")
    # sent as written: 300 ms apart, not all at the end
    check(len(times) == 3 and times[0] < 0.25 and times[2] - times[0] > 0.45,
          f"Events not streamed: {times}")

    stream = EventStream(plain, b"/sse.kl1?n=2", b"Last-Event-ID: 41\r\n")
    ids = []
    while (event := stream.event()) is not None:
        if "id" in event:
            ids.append(event["id"])
    stream.close()
    check(ids == ["42", "43"], f"Last-Event-ID not used: {ids}")

    # the stream ends, the connection stays usable (HTTP/1.1; the
    # iterative server closes every connection)
    with Connection(plain) as conn:
        conn.send(b"GET /sse.kl1?n=1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
        status, headers, body = conn.response()
        check(" 200 " in status and b"id: 1\ndata: event 1\ndata: second line\n\n" in body,
              f"SSE over keep-alive: {status} {body!r}")
        if args.prefork:
            conn.send(b"GET /static/hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n")
            status, _, body = conn.response()
            check(" 200 " in status and body == b"klin integration test\n",
                  "Connection not reusable after an event stream")

    # HTTP/1.0: no chunking, the end of the stream closes the connection
    stream = EventStream(plain, b"/sse.kl1?n=2", version=b"HTTP/1.0")
    ids = []
    while (event := stream.event()) is not None:
        if "id" in event:
            ids.append(event["id"])
    stream.close()
    check(not stream.chunked and ids == ["1", "2"], f"SSE over HTTP/1.0: {ids}")

    stream = EventStream(secure, b"/sse.kl1?n=2", context=client_context(ssl.TLSVersion.TLSv1_3))
    ids = []
    while (event := stream.event()) is not None:
        if "id" in event:
            ids.append(event["id"])
    stream.close()
    check(ids == ["1", "2"], f"SSE over TLS: {ids}")

    # the page waits in sse_wait(); closing the connection ends it at once
    mark = temp / "sse-closed"
    stream = EventStream(plain, b"/sse.kl1?n=1&mark=" + str(mark).encode())
    while (event := stream.event()) is not None and event.get("id") != "1":
        pass
    ping = stream.event()
    stream.close()
    deadline = time.monotonic() + 3
    while not mark.exists() and time.monotonic() < deadline:
        time.sleep(0.05)
    check(ping == {"": "ping"} and mark.exists() and mark.read_text() == "closed\n",
          f"Page did not see the client go away: {ping} {mark.exists()}")


class WebSocket:
    """A WebSocket client (RFC 6455) that can also send what a correct
    client would not."""
    GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self, port, path=b"/ws.kl1", extra=b"", context=None, origin=True,
                 key=None, version=b"13"):
        raw = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock = context.wrap_socket(raw, server_hostname="localhost") if context else raw
        self.key = key if key is not None else base64.b64encode(os.urandom(16))
        host = b"127.0.0.1:" + str(port).encode()
        request = (b"GET " + path + b" HTTP/1.1\r\nHost: " + host + b"\r\n"
                   b"Upgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
                   b"Sec-WebSocket-Version: " + version + b"\r\n")
        if self.key:
            request += b"Sec-WebSocket-Key: " + self.key + b"\r\n"
        if origin is True:
            request += b"Origin: http" + (b"s" if context else b"") + b"://" + host + b"\r\n"
        elif origin:
            request += b"Origin: " + origin + b"\r\n"
        self.sock.sendall(request + extra + b"\r\n")
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            self._recv()
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        self.status = int(lines[0].split()[1])
        self.fields = {}
        for line in lines[1:]:
            name, _, value = line.partition(":")
            self.fields[name.strip().lower()] = value.strip()

    def accepted(self):
        want = base64.b64encode(hashlib.sha1(self.key + self.GUID.encode()).digest()).decode()
        return (self.status == 101 and self.fields.get("upgrade", "").lower() == "websocket" and
                self.fields.get("sec-websocket-accept") == want)

    def _recv(self):
        data = self.sock.recv(65536)
        if not data:
            raise EOFError
        self.buf += data

    def _take(self, n):
        while len(self.buf) < n:
            self._recv()
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def send_frame(self, op, payload=b"", fin=True, mask=True, rsv=0):
        head = bytes([(0x80 if fin else 0) | rsv | op])
        n = len(payload)
        bit = 0x80 if mask else 0
        if n < 126:
            head += bytes([bit | n])
        elif n < 65536:
            head += bytes([bit | 126]) + n.to_bytes(2, "big")
        else:
            head += bytes([bit | 127]) + n.to_bytes(8, "big")
        if mask:
            key = os.urandom(4)
            payload = bytes(b ^ key[i % 4] for i, b in enumerate(payload))
            head += key
        self.sock.sendall(head + payload)

    def send(self, data):
        self.send_frame(1 if isinstance(data, str) else 2,
                        data.encode() if isinstance(data, str) else data)

    def frame(self):
        """(opcode, payload) of the next frame from the server, None at EOF."""
        try:
            b0, b1 = self._take(2)
            if b1 & 0x80:
                return ("masked", b"")       # a server must not mask
            n = b1 & 0x7F
            if n == 126:
                n = int.from_bytes(self._take(2), "big")
            elif n == 127:
                n = int.from_bytes(self._take(8), "big")
            return (b0 & 0x0F, self._take(n))
        except (EOFError, ConnectionResetError, socket.timeout, ssl.SSLError):
            return None

    def close_code(self):
        """The code of the close frame the server sends next (skipping data)."""
        while (frame := self.frame()) is not None:
            if frame[0] == 8:
                return int.from_bytes(frame[1][:2], "big") if len(frame[1]) >= 2 else 1005
        return None

    def close(self):
        self.sock.close()


def run_websocket_checks(check, plain, secure):
    """WebSockets (doc/WEBSOCKET.md): handshake, echo of every frame form,
    control frames, protocol errors and limits, refused handshakes, TLS."""
    ws = WebSocket(plain, extra=b"Sec-WebSocket-Protocol: echo, chat\r\n")
    check(ws.accepted() and ws.fields.get("sec-websocket-protocol") == "chat",
          f"WebSocket handshake: {ws.status} {ws.fields}")
    for message in ["hello", "héllo ☃ \U0001F600", "x" * 125, "y" * 126, "z" * 70000]:
        ws.send(message)
        check(ws.frame() == (1, message.encode()), f"Text echo of {len(message)} chars")
    ws.send(bytes(range(256)) * 300)
    check(ws.frame() == (2, bytes(range(256)) * 300), "Binary echo")
    ws.send("")
    check(ws.frame() == (1, b""), "Empty message echo")
    # a fragmented message with a ping between its parts
    ws.send_frame(1, b"frag", fin=False)
    ws.send_frame(9, b"are you there")
    check(ws.frame() == (10, b"are you there"), "Ping not answered with its payload")
    ws.send_frame(0, b"men", fin=False)
    ws.send_frame(0, b"ted")
    check(ws.frame() == (1, b"fragmented"), "Fragmented message")
    ws.send_frame(10, b"unsolicited pong")    # ignored
    ws.send("protocol")
    check(ws.frame() == (1, b"chat"), "Chosen subprotocol not seen by the page")
    # the client closes: the server answers with the same code
    ws.send_frame(8, (4001).to_bytes(2, "big") + b"client done")
    check(ws.close_code() == 4001, "Close not echoed")
    check(ws.frame() is None, "Connection not closed after the close handshake")
    ws.close()
    # an empty first message (there is no buffer yet: found by fuzzing, it
    # crashed the worker); after the first connection: -d serves one at a time
    first = WebSocket(plain)
    first.send("")
    check(first.frame() == (1, b""), "Empty first message not echoed")
    first.send("bye")
    first.close()

    # the page closes
    ws = WebSocket(plain)
    ws.send("bye")
    code = ws.close_code()
    ws.send_frame(8, (1000).to_bytes(2, "big"))
    check(code == 1000 and ws.frame() is None, f"Page close: {code}")
    ws.close()

    # protocol errors: the server closes with the right code
    for name, send, want in [
            ("unmasked frame", lambda w: w.send_frame(1, b"x", mask=False), 1002),
            ("RSV bit", lambda w: w.send_frame(1, b"x", rsv=0x40), 1002),
            ("unknown opcode", lambda w: w.send_frame(3, b"x"), 1002),
            ("fragmented ping", lambda w: w.send_frame(9, b"x", fin=False), 1002),
            ("long ping", lambda w: w.send_frame(9, b"x" * 126), 1002),
            ("stray continuation", lambda w: w.send_frame(0, b"x"), 1002),
            ("new message inside a message",
             lambda w: (w.send_frame(1, b"a", fin=False), w.send_frame(1, b"b")), 1002),
            ("invalid UTF-8", lambda w: w.send_frame(1, b"\xc3\x28"), 1007),
            ("surrogate in UTF-8", lambda w: w.send_frame(1, b"\xed\xa0\x80"), 1007),
            ("overlong UTF-8", lambda w: w.send_frame(1, b"\xc0\xaf"), 1007),
            ("UTF-8 cut short", lambda w: w.send_frame(1, b"ok \xe2\x82"), 1007)]:
        ws = WebSocket(plain)
        send(ws)
        code = ws.close_code()
        ws.close()
        check(code == want, f"{name}: close code {code}, expected {want}")
    ws = WebSocket(plain, b"/ws.kl1?max=1000")
    ws.send("a" * 1000)
    ok = ws.frame() == (1, b"a" * 1000)
    ws.send_frame(1, b"b" * 600, fin=False)
    ws.send_frame(0, b"c" * 600)
    code = ws.close_code()
    ws.close()
    check(ok and code == 1009, f"Message limit: {ok} {code}")

    # refused handshakes
    for name, kwargs, want in [
            ("another site's page", {"origin": b"http://evil.example"}, 403),
            ("version 8", {"version": b"8"}, 426),
            ("no key", {"key": b""}, 400),
            ("short key", {"key": base64.b64encode(b"x" * 10)}, 400)]:
        ws = WebSocket(plain, **kwargs)
        ws.close()
        check(ws.status == want and not ws.accepted(), f"Handshake with {name}: {ws.status}")
    ws = WebSocket(plain, origin=False)
    check(ws.accepted(), "Handshake without Origin (not a browser) refused")
    ws.close()
    reply = exchange(plain, b"GET /ws.kl1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
    check(b" 400 " in reply.split(b"\r\n")[0], "A plain GET of a WebSocket page was not refused")

    # TLS
    ws = WebSocket(secure, context=client_context(ssl.TLSVersion.TLSv1_3))
    ws.send("over tls")
    ok = ws.accepted() and ws.frame() == (1, b"over tls")
    ws.send("z" * 70000)
    ok = ok and ws.frame() == (1, b"z" * 70000)
    ws.close()
    check(ok, "WebSocket over TLS")


VALIDATION_ROUTES = """
        route
        {
            path /v/{id}
            page /echo.kl1
        }
        route
        {
            path /d
            page /echo.kl1
        }
        route
        {
            path /r
            page /echo.kl1
        }
        route
        {
            path /h
            page /echo.kl1
        }
        route
        {
            path /u1
            page /upload.kl1
        }
        route
        {
            path /u2
            page /upload.kl1
        }
        route
        {
            path /u3
            page /upload.kl1
        }"""


ROUTES = """routes
    {
        route
        {
            method GET
            path /api/items/{id}
            page /api.kl1
        }
        route
        {
            method PUT, PATCH DELETE
            path /api/items/{id}
            page /api.kl1
        }
        route
        {
            method POST
            path /api/items
            page /api.kl1
        }
        route
        {
            path /api/files/{path*}
            page /api.kl1
        }""" + VALIDATION_ROUTES + """
    }"""


def run_route_checks(args, check, plain):
    """REST routes and JSON (doc/ROUTES.md, doc/JSON.md): methods, parameters,
    405 with Allow, JSON bodies and their errors."""
    def call(method, path, body=None, ctype=b"application/json"):
        extra = b""
        if body is not None:
            extra = b"Content-Type: " + ctype + b"\r\nContent-Length: " + \
                str(len(body)).encode() + b"\r\n"
        reply = exchange(plain, method + b" " + path + b" HTTP/1.1\r\nHost: localhost\r\n" +
                         extra + b"\r\n" + (body or b""))
        head, _, data = reply.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        fields = {}
        for line in lines[1:]:
            name, _, value = line.partition(":")
            fields[name.strip().lower()] = value.strip()
        try:
            doc = json.loads(data) if data else None
        except ValueError:
            doc = data
        return int(lines[0].split()[1]), fields, doc

    status, fields, doc = call(b"GET", b"/api/items/42")
    check(status == 200 and fields.get("content-type") == "application/json" and
          doc == {"method": "GET", "id": "42"}, f"GET route: {status} {doc}")
    status, fields, doc = call(b"HEAD", b"/api/items/42")
    check(status == 200 and doc is None, f"HEAD on a GET route: {status}")
    for method in [b"PUT", b"PATCH"]:
        status, _, doc = call(method, b"/api/items/7", b'{"name": "x", "tags": [1, 2]}')
        check(status == 200 and doc == {"method": method.decode(), "id": "7",
                                        "body": {"name": "x", "tags": [1, 2]}},
              f"{method!r} with a JSON body: {status} {doc}")
    status, _, doc = call(b"DELETE", b"/api/items/7")
    check(status == 200 and doc == {"method": "DELETE", "id": "7"}, f"DELETE: {doc}")
    status, _, doc = call(b"POST", b"/api/items", b'{"new": true}')
    check(status == 200 and doc == {"method": "POST", "body": {"new": True}}, f"POST: {doc}")

    # the path has routes, not for this method: 405 and what it takes
    status, fields, _ = call(b"POST", b"/api/items/42", b"{}")
    check(status == 405 and fields.get("allow") == "GET, HEAD, PUT, DELETE, PATCH",
          f"405: {status} {fields.get('allow')}")
    status, fields, _ = call(b"GET", b"/api/items")
    check(status == 405 and fields.get("allow") == "POST", f"405 on /api/items: {status}")

    for path in [b"/api/items/42/more", b"/api/items/", b"/api/itemsx/1"]:
        status, _, _ = call(b"GET", path)
        check(status == 404, f"Unrouted {path!r}: {status}")

    # parameters are URL-decoded, and written back escaped
    status, _, doc = call(b"GET", b"/api/items/a%20b%3Cc")
    check(doc == {"method": "GET", "id": "a b<c"}, f"Decoded parameter: {doc}")
    reply = exchange(plain, b"GET /api/items/%3Cx%3E HTTP/1.1\r\nHost: localhost\r\n\r\n")
    check(b'"id":"\\u003cx\\u003e"' in reply, "Route parameter not escaped in JSON output")
    status, _, doc = call(b"GET", b"/api/files/a/b/c.txt")
    check(doc == {"method": "GET", "path": "a/b/c.txt"}, f"Catch-all parameter: {doc}")
    status, _, doc = call(b"GET", b"/api/files/")
    check(doc == {"method": "GET", "path": ""}, f"Empty catch-all: {doc}")

    # JSON body errors
    for body, ctype, want in [(b"{}", b"text/plain", 415), (b'{"a":1,}', b"application/json", 400),
                              (b'{"a":1,"a":2}', b"application/json", 400),
                              (b"[" + b"1," * 600 + b"1]", b"application/json", 413),
                              (b'{"x":1}', b"application/vnd.api+json; charset=utf-8", 200)]:
        status, _, doc = call(b"PUT", b"/api/items/1", body, ctype)
        check(status == want, f"JSON body {body[:20]!r} {ctype!r}: {status}, expected {want}")

    # a request body read by the page leaves the connection usable
    if args.prefork:
        with Connection(plain) as conn:
            body = b'{"k": "v"}'
            conn.send(b"PUT /api/items/9 HTTP/1.1\r\nHost: localhost\r\nContent-Type: "
                      b"application/json\r\nContent-Length: " + str(len(body)).encode() +
                      b"\r\n\r\n" + body)
            status, _, data = conn.response()
            conn.send(b"GET /api/items/10 HTTP/1.1\r\nHost: localhost\r\n\r\n")
            status2, _, data2 = conn.response()
            check(" 200 " in status and json.loads(data)["body"] == {"k": "v"} and
                  json.loads(data2) == {"method": "GET", "id": "10"},
                  "Keep-alive after a JSON PUT")


ACCESS = """access
    {
        rule
        {
            path /static/private*
            allow 10.0.0.0/8, 2001:db8::/32
            deny all
        }
        rule
        {
            path /static/local*
            allow 127.0.0.0/8 ::1
            deny all
        }
        rule
        {
            path /pid.kl1
            deny 127.0.0.1/32
        }
        rule
        {
            deny 192.0.2.0/24
        }
    }"""

LIMITS = """max_conn_per_ip 3
    max_streams 2
    worker_max_memory 256
    worker_max_files 64
    worker_max_procs 0
    keep_alive_timeout 8
    prefork.start_child 4
    prefork.max_child 6"""

BLOCKED = """access
    {
        rule
        {
            allow 10.0.0.0/8
            deny 127.0.0.1
        }
    }"""


def run_access_checks(check, client, blocked, temp):
    """IP access control (doc/ACCESS.md): rules per path, the first matching
    rule decides, networks, and clients dropped at connection time."""
    (temp / "private.txt").write_text("private\n")
    (temp / "local.txt").write_text("local\n")

    def status(path):
        reply = exchange(client, b"GET " + path + b" HTTP/1.1\r\nHost: localhost\r\n\r\n")
        return int(reply.split(b"\r\n")[0].split()[1]), reply.partition(b"\r\n\r\n")[2]

    for path, want, body in [(b"/static/private.txt", 403, None),
                             (b"/static/local.txt", 200, b"local\n"),
                             (b"/pid.kl1", 403, None),
                             (b"/static/hello.txt", 200, b"klin integration test\n")]:
        code, data = status(path)
        check(code == want and (body is None or data == body),
              f"Access to {path!r}: {code}, expected {want}")

    # the all-paths rule of 'blocked' refuses 127.0.0.1: no HTTP at all
    with socket.create_connection(("127.0.0.1", blocked), timeout=3) as raw:
        try:
            raw.sendall(b"GET /static/hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n")
            dropped = raw.recv(100) == b""
        except (ConnectionResetError, BrokenPipeError):
            dropped = True
    check(dropped, "A denied client was not dropped at connection time")


VALIDATION = """validators
    {
        validator
        {
            name id
            regex [0-9]{1,6}
        }
        validator
        {
            name page
            int 1 100
        }
        validator
        {
            name word
            length 1 8
        }
        validator
        {
            name order
            enum asc desc
        }
        validator
        {
            name token
            regex [a-f0-9]{8}
        }
    }
    params
    {
        path /echo.kl1
        param
        {
            name page
            validator page
            required yes
        }
        param
        {
            name order
            validator order
        }
        param
        {
            name q
            validator word
            source query
        }
    }
    params
    {
        path /v/*
        param
        {
            name id
            source route
            validator id
            required yes
        }
    }
    params
    {
        path /d
        on_invalid drop
        unknown allow
        param
        {
            name page
            validator page
        }
    }
    params
    {
        path /r
        unknown reject
        param
        {
            name page
            validator page
        }
    }
    params
    {
        path /h
        method GET
        param
        {
            name X-Token
            source header
            validator token
            required yes
        }
    }
    params
    {
        path /u1
        unknown reject
        param
        {
            name field
        }
        upload
        {
            name file
            max_size 16
            type text/plain, image/*
        }
    }
    params
    {
        path /u2
        on_invalid drop
        upload
        {
            name file
            max_size 16
            type text/plain
            max_count 2
            required yes
        }
    }
    params
    {
        path /u3
        unknown allow
    }"""

def run_validation_checks(check, plain):
    """Declarative validation (doc/VALIDATE.md): sources, validator types,
    required, repeated values, NUL smuggling, drop/reject, unknown."""
    def get(path, extra=b"", body=None):
        method = b"POST" if body is not None else b"GET"
        if body is not None:
            extra += b"Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " + \
                str(len(body)).encode() + b"\r\n"
        reply = exchange(plain, method + b" " + path + b" HTTP/1.1\r\nHost: localhost\r\n" +
                         extra + b"\r\n" + (body or b""))
        head, _, data = reply.partition(b"\r\n\r\n")
        return int(head.split(b"\r\n")[0].split()[1]), sorted(data.decode().split())

    for path, want, seen in [
            (b"/echo.kl1?page=5&order=asc&q=abc", 200, ["order=asc", "page=5", "q=abc"]),
            (b"/echo.kl1?page=100", 200, ["page=100"]),
            (b"/echo.kl1?page=5&extra=1&x=y", 200, ["page=5"]),     # unknown: dropped
            (b"/echo.kl1?page=0", 400, None),
            (b"/echo.kl1?page=101", 400, None),
            (b"/echo.kl1?page=abc", 400, None),
            (b"/echo.kl1?page=+5", 400, None),
            (b"/echo.kl1?page=5%00x", 400, None),                    # a NUL hiding the rest
            (b"/echo.kl1?order=asc", 400, None),                     # required page missing
            (b"/echo.kl1?page=5&order=up", 400, None),
            (b"/echo.kl1?page=5&q=", 400, None),                     # length 1-8
            (b"/echo.kl1?page=5&q=ninechars", 400, None),
            (b"/echo.kl1?page=5&page=500", 400, None),               # each value checked
            (b"/v/123456", 200, ["route:id=123456"]),
            (b"/v/1234567", 400, None),
            (b"/v/12a", 400, None),
            (b"/d?page=0&keep=1", 200, ["keep=1"]),                  # drop mode, unknown allowed
            (b"/d?page=7", 200, ["page=7"]),
            (b"/r?page=1", 200, ["page=1"]),
            (b"/r?page=1&zzz=1", 400, None)]:                        # unknown rejected
        status, body = get(path)
        check(status == want and (seen is None or body == seen),
              f"Validation of {path!r}: {status} {body}, expected {want} {seen}")

    # the source counts: q is a query parameter, a form q is unknown
    status, body = get(b"/echo.kl1?page=5", body=b"q=abc&order=desc")
    check(status == 200 and body == ["order=desc", "page=5"],
          f"Form/query sources: {status} {body}")
    status, body = get(b"/echo.kl1", body=b"page=7")
    check(status == 200 and body == ["page=7"], f"Form value: {status} {body}")

    for token, want in [(b"deadbeef", 200), (b"DEADBEEF", 400), (b"dead", 400), (None, 400)]:
        extra = b"X-Token: " + token + b"\r\n" if token else b""
        status, _ = get(b"/h", extra)
        check(status == want, f"Header validation {token!r}: {status}, expected {want}")

    # upload rules: size (413), Content-Type, count, unknown fields
    def upload(path, *parts):
        body = b""
        for name, filename, ctype, data in parts:
            body += b"--XyZ\r\nContent-Disposition: form-data; name=\"" + name + b"\""
            if filename is not None:
                body += b"; filename=\"" + filename + b"\""
            body += b"\r\n" + (b"Content-Type: " + ctype + b"\r\n" if ctype else b"") + \
                b"\r\n" + data + b"\r\n"
        body += b"--XyZ--\r\n"
        reply = exchange(plain, b"POST " + path + b" HTTP/1.1\r\nHost: localhost\r\n"
                         b"Content-Type: multipart/form-data; boundary=XyZ\r\n"
                         b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
        head, _, data = reply.partition(b"\r\n\r\n")
        return int(head.split(b"\r\n")[0].split()[1]), data

    text = (b"file", b"a.txt", b"text/plain", b"file body")
    big = (b"file", b"b.txt", b"text/plain", b"x" * 17)
    for name, path, parts, want, seen in [
            ("allowed file", b"/u1", [(b"field", None, None, b"v"), text], 200,
             b"file=a.txt text/plain 9 [file body]\nuploads=1\n"),
            ("type/* match", b"/u1", [(b"file", b"p.png", b"image/png", b"png")], 200,
             b"file=p.png image/png 3"),
            ("type case and parameters", b"/u1",
             [(b"file", b"c.txt", b"TEXT/Plain; charset=utf-8", b"abc")], 200, b"uploads=1"),
            ("size limit exactly", b"/u1", [(b"file", b"s.txt", b"text/plain", b"y" * 16)], 200,
             b"uploads=1"),
            ("too large", b"/u1", [big], 413, None),
            ("wrong type", b"/u1", [(b"file", b"a.pdf", b"application/pdf", b"%PDF")], 400, None),
            ("no Content-Type", b"/u1", [(b"file", b"a.txt", None, b"abc")], 400, None),
            ("type with junk after it", b"/u1",
             [(b"file", b"a.txt", b"text/plain junk", b"abc")], 400, None),
            ("type/* with an empty subtype", b"/u1", [(b"file", b"p", b"image//", b"x")], 400,
             None),
            ("type without a subtype", b"/u1", [(b"file", b"p", b"image/", b"x")], 400, None),
            ("blank before parameters", b"/u1",
             [(b"file", b"c.txt", b"text/plain ; charset=utf-8", b"abc")], 200, b"uploads=1"),
            ("two files, max_count 1", b"/u1", [text, text], 400, None),
            ("unnamed field, unknown reject", b"/u1", [text, (b"other", b"o.txt", b"text/plain",
                                                             b"o")], 400, None),
            ("empty file input", b"/u1", [(b"file", b"", b"application/octet-stream", b"")], 200,
             b"file=(none)\nuploads=0\n"),
            ("no file, not required", b"/u1", [(b"field", None, None, b"v")], 200, b"uploads=0"),
            ("plain field with an upload's name", b"/u1", [(b"file", None, None, b"x")], 400,
             None),
            ("empty filename but data", b"/u1", [(b"file", b"", b"text/plain", b"x")], 400,
             None),
            ("drop: bad file removed", b"/u2", [big, text], 200,
             b"file=a.txt text/plain 9 [file body]\nuploads=1\n"),
            ("drop: required file dropped", b"/u2", [big], 400, None),
            ("drop: wrong type dropped", b"/u2", [(b"file", b"p.png", b"image/png", b"png"), text],
             200, b"file=a.txt"),
            ("drop: max_count 2", b"/u2", [text, text], 200, b"uploads=2"),
            ("drop: too many files", b"/u2", [text, text, text], 400, None),
            ("drop: unnamed field dropped", b"/u2", [(b"other", b"o.txt", b"text/plain", b"o"),
                                                    text], 200, b"uploads=1"),
            ("unknown allow keeps files", b"/u3", [(b"other", b"o.txt", b"text/plain", b"o")], 200,
             b"uploads=1")]:
        status, data = upload(path, *parts)
        check(status == want and (seen is None or seen in data),
              f"Upload validation ({name}): {status} {data[:200]!r}, expected {want} {seen!r}")
    # an upload rule's name does not cover a URL-encoded argument either
    status, _ = get(b"/u1", body=b"file=x")
    check(status == 400, f"A URL-encoded argument named as an upload passed: {status}")

    # pages without params rules are not affected
    status, _ = get(b"/pid.kl1?anything=at%00all")
    check(status == 200, f"A page without rules was validated: {status}")


def key_needles(key):
    """Byte strings that only a copy of the private key contains: a line of
    its PEM text, and part of a prime in both byte orders (OpenSSL keeps
    big numbers as little-endian words)."""
    text = subprocess.run([OPENSSL, "pkey", "-in", str(key), "-text", "-noout"],
                          capture_output=True, text=True, check=True).stdout
    prime = re.search(r"prime1:\n((?:\s+[0-9a-f:]+\n)+)", text).group(1)
    p = bytes.fromhex(re.sub(r"[\s:]", "", prime)).lstrip(b"\0")
    pem = [line for line in key.read_text().splitlines() if not line.startswith("-----")]
    return [pem[len(pem) // 2].encode(), p[8:40], p[::-1][8:40]]


def process_has(pid, needles):
    """Does the memory of process pid hold any of the needles? Read in
    chunks that overlap by the longest needle."""
    chunk, overlap = 1 << 22, max(len(n) for n in needles)
    with open(f"/proc/{pid}/maps") as maps, open(f"/proc/{pid}/mem", "rb", 0) as mem:
        for line in maps:
            addr, perms = line.split()[:2]
            if "r" not in perms or "[vvar]" in line or "[vsyscall]" in line:
                continue
            start, end = (int(x, 16) for x in addr.split("-"))
            tail = b""
            for at in range(start, end, chunk):
                try:
                    mem.seek(at)
                    data = tail + mem.read(min(chunk, end - at))
                except (OSError, OverflowError, ValueError):
                    break
                if any(n in data for n in needles):
                    return True
                tail = data[-overlap:]
    return False


def run_channel_checks(args, check, plain):
    """Prefork: a worker holds only its own IPC channel to klind, not the
    parent's ends of its siblings' channels (it could read or answer their
    messages to klind), and no channel survives into a CGI script
    (close-on-exec). Run after the sandbox checks, whose killed worker was
    replaced by one started after all the others."""
    if not args.prefork:
        return
    reply = exchange(plain, b"GET /channels.kl1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
    body = reply.partition(b"\r\n\r\n")[2].strip()
    check(body == b"channels=1 exec=0", f"Worker's IPC channels: {body!r}")


def run_bad_server_checks(check, daemon, env, temp):
    """Top-level settings out of range stop klind at startup: max_child
    below 1 (it became a huge unsigned number: no limit on workers) or so
    large that klind's select() could not watch one channel per worker."""
    port = free_port()

    def starts(settings):
        config = temp / "server-start.conf"
        config.write_text(f"server_list one\nallow_root yes\n{settings}\none\n{{\ntype http\n"
                          f"addr tcp4://127.0.0.1:{port}\ndir_root /www\n}}\n")
        process = subprocess.Popen([str(daemon), "-F", "-f", str(config)], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            process.wait(timeout=2)
            return False
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            return True

    check(starts("max_child 10"), "A valid max_child was refused")
    for value in ["-1", "0", "100000"]:
        check(not starts(f"max_child {value}"), f"klind started with max_child {value}")


def run_orphan_checks(check, daemon, env, temp):
    """Linux: klind killed (SIGKILL, so it cannot stop its workers): the
    workers end too, instead of serving on without it."""
    if not Path("/proc/self/stat").exists():
        return
    port = free_port()
    config = temp / "orphan.conf"
    config.write_text(f"server_list one\nallow_root yes\none\n{{\ntype http\n"
                      f"addr tcp4://127.0.0.1:{port}\ndir_root /www\nprefork.start_child 2\n}}\n")
    process = subprocess.Popen([str(daemon), "-F", "-f", str(config)], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               start_new_session=True)
    try:
        deadline = time.monotonic() + 10
        workers = set()
        while time.monotonic() < deadline and not workers:
            time.sleep(0.2)
            workers = klind_processes(process.pid) - {process.pid}
        check(workers, "No workers started")
        process.kill()
        process.wait()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and any(Path(f"/proc/{p}").exists() for p in workers):
            time.sleep(0.1)
        alive = [p for p in workers if Path(f"/proc/{p}").exists()
                 and "Z" not in Path(f"/proc/{p}/stat").read_text().rsplit(")", 1)[1].split()[0]]
        check(not alive, f"Workers {alive} outlived klind")
    finally:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def run_pid_file_checks(check, daemon, env, temp):
    """-p: the PID file holds klind's PID and is created mode 0644 even
    under umask 0 (LibU's u_savepid() left it to the umask)."""
    port = free_port()
    config = temp / "pidfile.conf"
    pid_file = temp / "klind.pid"
    config.write_text(f"server_list one\nallow_root yes\none\n{{\ntype http\n"
                      f"addr tcp4://127.0.0.1:{port}\ndir_root /www\n}}\n")
    process = subprocess.Popen([str(daemon), "-F", "-f", str(config), "-p", str(pid_file)],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               start_new_session=True, umask=0)
    try:
        deadline = time.monotonic() + 10
        text = ""
        while time.monotonic() < deadline and not text.endswith("\n"):
            time.sleep(0.1)
            text = pid_file.read_text() if pid_file.exists() else ""
        check(text == f"{process.pid}\n", f"PID file holds {text!r}, not {process.pid}")
        mode = pid_file.stat().st_mode & 0o777 if pid_file.exists() else None
        check(mode == 0o644, f"PID file mode {mode and oct(mode)}, not 0o644")
    finally:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def run_blind_chroot_checks(check, daemon, env, temp):
    """blind_chroot (root only): klind enters a new, empty directory of
    mode 0100 under 'chroot', through the descriptor it checked, and the
    directory is removed once klind is inside: it serves its embedded
    pages, runs in a deleted directory, and leaves nothing behind."""
    if not hasattr(os, "geteuid") or os.geteuid() != 0 or not Path("/proc/self/root").exists():
        return
    jail = temp / "jail"
    jail.mkdir()
    port = free_port()
    config = temp / "blind.conf"
    config.write_text(f"server_list one\nuid 65534\ngid 65534\nchroot {jail}\nblind_chroot yes\n"
                      f"one\n{{\ntype http\naddr tcp4://127.0.0.1:{port}\ndir_root /www\n}}\n")
    process = subprocess.Popen([str(daemon), "-F", "-d", "-f", str(config)], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        reply = b""
        deadline = time.monotonic() + 10
        while process.poll() is None and time.monotonic() < deadline:
            try:
                reply = exchange(port, b"GET /pid.kl1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
                break
            except OSError:
                time.sleep(0.2)
        check(b"200 OK" in reply, f"klind in a blind chroot did not serve: {reply[:200]!r}")
        root = os.readlink(f"/proc/{process.pid}/root")
        check(root.startswith(str(jail) + "/klind_blind_chroot_") and root.endswith("(deleted)"),
              f"klind's root is not the removed blind directory: {root}")
        check(not list(jail.iterdir()), f"blind_chroot left {list(jail.iterdir())}")
    finally:
        process.kill()
        process.wait()


def run_keymgr_checks(check, root_pid, key, sanitized):
    """The TLS key manager (doc/KEYMGR.md): after TLS requests, only the key
    manager's memory holds the private key, not klind's or any worker's;
    and processes of the same user (a worker) cannot read the key
    manager's memory at all (it is not dumpable)."""
    if not Path("/proc/self/mem").exists() or sanitized:
        # no /proc (macOS); or a sanitizer build, whose terabytes of shadow
        # and allocator mappings make a scan meaningless (release checks it)
        return
    needles = key_needles(key)
    procs = klind_processes(root_pid)
    # one key manager per TLS server: 'secure' and 'mtls' share this key, so
    # both hold it, and nothing else does (klind decoding the key itself,
    # for the second server with a ca_file, left a copy in the workers)
    managers = key_managers(root_pid, procs)
    check(len(managers) == 2, f"Key managers {sorted(managers)} in {sorted(procs)}")
    holders = set()
    for pid in sorted(procs):
        try:
            if process_has(pid, needles):
                holders.add(pid)
        except PermissionError:
            if pid not in managers:
                check(False, f"Cannot read the memory of klind process {pid}")
        except FileNotFoundError:
            pass    # a worker that exited meanwhile
    if os.geteuid() == 0:
        # root reads every process: the key is in the managers only
        check(holders == managers, f"Private key found in {sorted(holders)}; "
              f"key manager {sorted(managers)}, processes {sorted(procs)}")
    else:
        check(not holders, f"Private key found in {sorted(holders)} (key manager "
              f"{sorted(managers)}, processes {sorted(procs)})")
        for pid in managers:
            try:
                open(f"/proc/{pid}/mem", "rb").close()
                readable = True
            except PermissionError:
                readable = False
            check(not readable, f"A process of klind's user can read key manager {pid}'s memory")


def key_managers(root_pid, procs):
    """The key managers among klind's processes. They are not dumpable,
    so (unless we are root) the files in their /proc directory belong to
    root (the directory itself stays the user's); as root, they are the
    processes with only their control socket (and /dev/null) open."""
    found = set()
    for pid in procs:
        if pid == root_pid:
            continue
        try:
            if os.geteuid() == 0:
                if few_fds(pid):
                    found.add(pid)
            elif Path(f"/proc/{pid}/status").stat().st_uid == 0:
                found.add(pid)
        except OSError:
            pass    # exited meanwhile
    return found


def run_keymgr_policy_checks(check, plain):
    """A worker that asks the key manager for more than a TLS handshake
    signs (test/site/www/keymgr.kl1): other digests, no digest, raw RSA,
    a digest of the wrong length and X9.31 padding are refused; extra
    descriptors sent with a request are closed (not kept open in the key
    manager); PSS and PKCS #1 v1.5 handshake signatures still work."""
    reply = exchange(plain, b"GET /keymgr.kl1 HTTP/1.1\r\nHost: localhost\r\n\r\n", timeout=15)
    body = reply.partition(b"\r\n\r\n")[2].decode(errors="replace").split()
    expected = ["md5=refused", "nodigest=refused", "raw=refused", "short=refused",
                "x931=refused", "twofds=none", "closed", "pss=signed", "pkcs1=signed"]
    check(body == expected, f"Key manager signing policy: {body}")


def run_keymgr_start_checks(check, daemon, env, temp, cert, key):
    """klind started with its standard output and error closed: the key
    manager's control socket may then be descriptor 1 or 2, which its own
    /dev/null replaced; HTTPS must still work. Then klind's exit status: a
    failure when its key manager dies, 0 when SIGTERM or SIGINT stops it."""
    port = free_port()
    config = temp / "keymgr-start.conf"
    config.write_text(f"server_list s\nallow_root yes\ns\n{{\ntype https\n"
                      f"addr tcp4://127.0.0.1:{port}\ndir_root /www\n"
                      f"cert_file {cert}\nkey_file {key}\n}}\n")
    context = client_context(ssl.TLSVersion.TLSv1_3)

    def close_output():
        os.close(1)
        os.close(2)

    def start():
        process = subprocess.Popen([str(daemon), "-F", "-d", "-f", str(config)], env=env,
                                   stdin=subprocess.DEVNULL, preexec_fn=close_output)
        deadline = time.monotonic() + 10
        reply = b""
        while process.poll() is None and time.monotonic() < deadline:
            try:
                reply = exchange(port, b"GET /pid.kl1 HTTP/1.1\r\nHost: localhost\r\n\r\n", context)
                break
            except OSError:
                time.sleep(0.2)
        return process, reply

    def wait(process):
        try:
            return process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            return None

    process, reply = start()
    try:
        check(b"200 OK" in reply, f"HTTPS failed with klind's output closed: {reply[:200]!r}")
        # a key manager that dies stops klind with a failure status (an exit
        # code, not a signal), so that systemd's Restart=on-failure starts it
        if Path("/proc/self/stat").exists():
            managers = key_managers(process.pid, klind_processes(process.pid))
            check(len(managers) == 1, f"Key managers: {sorted(managers)}")
            os.kill(managers.pop(), signal.SIGKILL)
            status = wait(process)
            check(status is not None and status > 0,
                  f"klind exit status {status} after its key manager died")
    finally:
        process.kill()
        process.wait()

    # stopping klind stops its key manager too: that is not a failure
    for sig in (signal.SIGTERM, signal.SIGINT):
        process, reply = start()
        try:
            check(b"200 OK" in reply, f"HTTPS failed before {sig.name}: {reply[:200]!r}")
            process.send_signal(sig)
            status = wait(process)
            check(status == 0, f"klind exit status {status} after {sig.name} with a key manager")
        finally:
            process.kill()
            process.wait()


def ipv6_loopback():
    """Can this host listen on ::1? (Not every container has IPv6.)"""
    if not socket.has_ipv6:
        return False
    try:
        with socket.socket(socket.AF_INET6) as sock:
            sock.bind(("::1", 0))
        return True
    except OSError:
        return False


def run_ipv6_checks(args, check, daemon, env, temp, cert, key):
    """IPv6 listeners: HTTP and HTTPS servers on tcp6://[::1] serve over
    IPv6, an IPv4 client does not reach them, and access rules match the
    client's IPv6 address (::1 allowed, a rule for other networks denies)."""
    if not ipv6_loopback():
        print("IPv6 checks skipped: no ::1 on this host")
        return
    site = temp / "ipv6"
    site.mkdir()
    (site / "hello.txt").write_text("over ipv6\n")
    (site / "local.txt").write_text("local client\n")
    (site / "private.txt").write_text("private network\n")
    plain, secure = free_port(), free_port()
    access = ("access\n{\nrule\n{\npath /v6/local*\nallow ::1\ndeny all\n}\n"
              "rule\n{\npath /v6/private*\nallow 10.0.0.0/8, 2001:db8::/32\ndeny all\n}\n}\n")
    blocks = []
    for name, port, tls in [("v6plain", plain, False), ("v6secure", secure, True)]:
        blocks.append(f"{name}\n{{\ntype {'https' if tls else 'http'}\n"
                      f"addr tcp6://[::1]:{port}\ndir_root /www\ndir_alias /v6 {site}\n"
                      f"prefork.start_child 1\nprefork.max_child 2\n"
                      + (f"cert_file {cert}\nkey_file {key}\n" if tls else access) + "}\n")
    config = temp / "ipv6.conf"
    config.write_text("server_list v6plain v6secure\nallow_root yes\n" + "".join(blocks))
    command = [str(daemon), "-F", "-f", str(config)] + ([] if args.prefork else ["-d"])
    process = subprocess.Popen(command, env=env, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, start_new_session=True)

    def get(port, path, context=None, host="::1"):
        reply = exchange(port, b"GET " + path + b" HTTP/1.1\r\nHost: localhost\r\n\r\n",
                         context, host=host)
        status = reply.split(b" ", 2)[1] if reply.startswith(b"HTTP/") else b""
        return status, reply.partition(b"\r\n\r\n")[2]

    try:
        deadline = time.monotonic() + 10
        while process.poll() is None and time.monotonic() < deadline:
            try:
                socket.create_connection(("::1", plain), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        check(get(plain, b"/v6/hello.txt") == (b"200", b"over ipv6\n"), "HTTP over IPv6 failed")
        check(get(secure, b"/v6/hello.txt", client_context(ssl.TLSVersion.TLSv1_3)) ==
              (b"200", b"over ipv6\n"), "HTTPS over IPv6 failed")
        check(get(plain, b"/v6/local.txt") == (b"200", b"local client\n"),
              "An access rule did not let ::1 in")
        check(get(plain, b"/v6/private.txt")[0] == b"403",
              "An access rule for other networks let ::1 in")
        try:
            get(plain, b"/v6/hello.txt", host="127.0.0.1")
            reached = True
        except OSError:
            reached = False
        check(not reached, "An IPv4 client reached a tcp6://[::1] listener")
    finally:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def run_bad_tls_checks(check, daemon, env, temp, cert, key):
    """verify_depth must be a number from 0 to 100: a block (no value: it
    crashed klind), a partly numeric value or a negative one (ignored
    before, leaving OpenSSL's depth) stop klind at startup."""
    port = free_port()

    def starts(setting):
        config = temp / "tls-start.conf"
        config.write_text(f"server_list s\nallow_root yes\ns\n{{\ntype https\n"
                          f"addr tcp4://127.0.0.1:{port}\ndir_root /www\n"
                          f"cert_file {cert}\nkey_file {key}\n{setting}\n}}\n")
        process = subprocess.Popen([str(daemon), "-F", "-d", "-f", str(config)], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            process.wait(timeout=2)
            return False
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            return True

    for setting in ["verify_depth 3", "verify_depth 0"]:
        check(starts(setting), f"A valid {setting!r} was refused")
    for setting in ["verify_depth\n{\nx 1\n}", "verify_depth 3x", "verify_depth -2",
                    "verify_depth 101"]:
        check(not starts(setting), f"klind started with {setting!r}")


def run_verify_depth_checks(check, daemon, env, temp, cert, key, ca, client_cert):
    """verify_depth 0 (no intermediate CA) still takes a client certificate
    signed directly by a CA in ca_file."""
    port = free_port()
    config = temp / "depth0.conf"
    config.write_text(f"server_list s\nallow_root yes\ns\n{{\ntype https\n"
                      f"addr tcp4://127.0.0.1:{port}\ndir_root /www\n"
                      f"cert_file {cert}\nkey_file {key}\nca_file {ca}\n"
                      f"verify_mode require\nverify_depth 0\n}}\n")
    process = subprocess.Popen([str(daemon), "-F", "-d", "-f", str(config)], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE  # ephemeral local test certificate
    context.load_cert_chain(*client_cert)
    try:
        reply = b""
        deadline = time.monotonic() + 10
        while process.poll() is None and time.monotonic() < deadline:
            try:
                reply = exchange(port, b"GET /pid.kl1 HTTP/1.1\r\nHost: localhost\r\n\r\n", context)
                break
            except OSError:
                time.sleep(0.2)
        check(b"200 OK" in reply, f"verify_depth 0 refused a certificate signed by ca_file: {reply[:200]!r}")
    finally:
        process.kill()
        process.wait()


# run as 'nobody': start klind, then try to open the memory of each of its
# processes, as a worker of the same user could
NOBODY_READER = r"""
import json, os, signal, socket, subprocess, sys, time
from pathlib import Path
daemon, config, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
klind = subprocess.Popen([daemon, "-F", "-f", config], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
try:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    time.sleep(0.5)
    procs = {klind.pid}
    for _ in range(3):
        for entry in Path("/proc").iterdir():
            if entry.name.isdigit():
                try:
                    ppid = int((entry / "stat").read_text().rsplit(")", 1)[1].split()[1])
                except (OSError, ValueError, IndexError):
                    continue
                if ppid in procs:
                    procs.add(int(entry.name))
    result = {}
    for pid in procs - {klind.pid}:
        try:
            open(f"/proc/{pid}/mem", "rb").close()
            result[pid] = "readable"
        except PermissionError:
            result[pid] = "denied"
        except OSError:
            pass
    print(json.dumps(result))
finally:
    os.killpg(klind.pid, signal.SIGKILL)    # klind and its workers
    klind.wait()
"""


def run_keymgr_dump_checks(check, daemon, env, temp, cert, key):
    """klind started as an ordinary user from the start (systemd User=):
    nothing resets dumpability, so the key manager must make itself
    undumpable, or a worker of the same user could read the key from its
    memory. As root (WSL), run klind as nobody and try, as nobody: the
    workers are readable (the check would see it), the key manager not.
    Without root, run_keymgr_checks checks the same as the user."""
    if not Path("/proc/self/mem").exists() or os.geteuid() != 0:
        return
    share = temp / "nobody"
    share.mkdir()
    temp.chmod(0o711)
    share.chmod(0o755)
    for name, src in [("cert.pem", cert), ("key.pem", key)]:
        (share / name).write_bytes(src.read_bytes())
        (share / name).chmod(0o644)
    port = free_port()
    config = share / "klind.conf"
    config.write_text(f"server_list s\nallow_root no\ns\n{{\ntype https\n"
                      f"addr tcp4://127.0.0.1:{port}\ndir_root /www\n"
                      f"prefork.start_child 1\ncert_file {share / 'cert.pem'}\n"
                      f"key_file {share / 'key.pem'}\n}}\n")
    config.chmod(0o644)
    result = subprocess.run([sys.executable, "-c", NOBODY_READER, str(daemon), str(config), str(port)],
                            user=65534, group=65534, extra_groups=[], env=env,
                            capture_output=True, text=True, timeout=60)
    try:
        access = json.loads(result.stdout)
    except ValueError:
        access = {}
    check(list(access.values()).count("denied") == 1 and "readable" in access.values(),
          f"As klind's user: key manager memory not protected, or no worker to compare: "
          f"{access} {result.stderr[-1500:]}")


def klind_processes(root_pid):
    """klind and the processes it started (and theirs)."""
    procs = {root_pid}
    for _ in range(3):
        for entry in Path("/proc").iterdir():
            if entry.name.isdigit():
                try:
                    ppid = int((entry / "stat").read_text().rsplit(")", 1)[1].split()[1])
                except (OSError, ValueError, IndexError):
                    continue
                if ppid in procs:
                    procs.add(int(entry.name))
    return procs


def few_fds(pid):
    """The key manager keeps only its control socket (and /dev/null). A
    worker may exit (or be a zombie) between the listing and this."""
    try:
        return len(list(Path(f"/proc/{pid}/fd").iterdir())) <= 4
    except OSError:
        return False


def seccomp_mode(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("Seccomp:"):
            return int(line.split()[1])
    return -1


def run_sandbox_checks(args, check, root_pid, plain, unsandboxed, cgi_port=None, sanitized=False):
    """Sandboxes (doc/SANDBOX.md, Linux seccomp): workers and the key manager
    are filtered, a call outside the list fails with EPERM, one the
    sandbox kills for ends the worker; 'sandbox no' turns it off."""
    if not Path("/proc/self/status").exists():
        return
    procs = klind_processes(root_pid)
    check(seccomp_mode(root_pid) == 0, "klind itself (not a worker) is sandboxed")
    managers = key_managers(root_pid, procs)
    check(managers and all(seccomp_mode(pid) == 2 for pid in managers),
          f"Key manager not sandboxed: {[(p, seccomp_mode(p)) for p in managers]}")
    if not args.prefork:
        return  # the single-process server has no workers

    def get(port, query):
        reply = exchange(port, b"GET /sandbox.kl1?" + query + b" HTTP/1.1\r\nHost: localhost\r\n\r\n")
        return reply.partition(b"\r\n\r\n")[2].strip()

    check(get(plain, b"try=link") == b"EPERM", "A call outside the list did not fail with EPERM")
    check(get(unsandboxed, b"try=link") == b"ENOENT", "sandbox no did not turn the sandbox off")
    # argument checks: socket families, W^X memory, signals to klind (a
    # worker without CGI: only itself; with CGI ('files'): not klind;
    # sanitizer builds do not check signals: LeakSanitizer sends them)
    for what in [b"netlink", b"wx"] + ([] if sanitized else [b"parent"]):
        check(get(plain, b"try=" + what) == b"EPERM", f"The sandbox allowed try={what.decode()}")
        check(get(unsandboxed, b"try=" + what) == b"OK", f"try={what.decode()} failed without a sandbox")
    for what in [b"inet6", b"self"]:
        check(get(plain, b"try=" + what) == b"OK", f"The sandbox refused try={what.decode()}")
    if cgi_port and not sanitized:
        check(get(cgi_port, b"try=parent") == b"EPERM", "A CGI worker may signal klind")
        check(get(cgi_port, b"try=self") == b"OK", "A CGI worker may not signal itself")
    workers = {pid for pid in klind_processes(root_pid) if pid != root_pid and pid not in managers}
    modes = {seccomp_mode(pid) for pid in workers}
    check(modes == {0, 2}, f"Workers' seccomp modes {modes}: expected filtered and unfiltered")
    try:
        killed = get(plain, b"try=unshare") == b""
    except (ConnectionResetError, OSError):
        killed = True
    check(killed, "unshare(2) did not kill the sandboxed worker")
    check(get(unsandboxed, b"try=unshare") == b"survived", "unshare killed an unsandboxed worker")
    reply = exchange(plain, b"GET /static/hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n")
    check(reply.endswith(b"klin integration test\n"), "Server not serving after a worker was killed")


def run_limit_checks(args, check, plain, limits, temp):
    """Request limits and request smuggling (doc/LIMITS.md): each refused
    request gets its status and the connection is closed (a request sent
    after it on the connection is not answered); then the header deadline,
    and (prefork) the caps on connections per address and on streams."""
    get = b"GET /static/hello.txt HTTP/1.1\r\nHost: localhost\r\n"
    after = b"GET /static/hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n"

    def status(header, line=get):
        reply = exchange(plain, line + header + b"\r\n" + after)
        check(reply.count(b"HTTP/1.1 ") <= 1 and b"klin integration test" not in reply,
              f"A request after a refused one was answered: {header[:60]!r}")
        return int(reply.split(b" ", 2)[1]) if reply.startswith(b"HTTP/1.") else None

    long_line = b"GET /static/" + b"a" * 9000 + b" HTTP/1.1\r\nHost: localhost\r\n"
    for name, header, line, want in [
            ("100 fields", b"X-A: 1\r\n" * 98, get, 200),
            ("101 fields", b"X-A: 1\r\n" * 99, get, 431),
            ("a field of 8192 bytes", b"X: " + b"x" * 8187 + b"\r\n", get, 200),
            ("a field over 8192 bytes", b"X: " + b"x" * 8188 + b"\r\n", get, 431),
            ("a header over 32 KB", (b"X: " + b"x" * 995 + b"\r\n") * 33, get, 431),
            ("a request line over 8 KB", b"", long_line, 414),
            ("a body over post_maxsize", b"Content-Length: 6000000\r\n", get, 413),
            ("token characters", b"X-!#$%&'*+.^_`|~: v\r\n", get, 200),
            ("obs-text in a value", b"X: caf\xe9\r\n", get, 200),
            ("a blank in the name", b"Bad Header: v\r\n", get, 400),
            ("a blank before the colon", b"Content-Length : 0\r\n", get, 400),
            ("an empty name", b": v\r\n", get, 400),
            ("a control character in the name", b"X\x01Y: v\r\n", get, 400),
            ("a control character in the value", b"X: a\x01b\r\n", get, 400),
            ("DEL in the value", b"X: a\x7fb\r\n", get, 400),
            ("a bare CR", b"X: a\rb\r\n", get, 400),
            ("obs-fold", b"X: a\r\n folded\r\n", get, 400),
            ("obs-fold with a tab", b"X: a\r\n\tfolded\r\n", get, 400),
            ("Content-Length and Transfer-Encoding",
             b"Content-Length: 0\r\nTransfer-Encoding: chunked\r\n", get, 400),
            ("Transfer-Encoding and Content-Length",
             b"Transfer-Encoding: chunked\r\nContent-Length: 0\r\n", get, 400),
            ("equal Content-Lengths", b"Content-Length: 0\r\nContent-Length: 0\r\n", get, 400),
            ("conflicting Content-Lengths", b"Content-Length: 0\r\nContent-Length: 5\r\n", get, 400),
            ("a Content-Length list", b"Content-Length: 0, 0\r\n", get, 400),
            ("a signed Content-Length", b"Content-Length: +0\r\n", get, 400),
            ("chunked", b"Transfer-Encoding: chunked\r\n", get, 501),
            ("an unknown transfer-coding", b"Transfer-Encoding: x-smuggle\r\n", get, 501),
            ("repeated transfer-codings", b"Transfer-Encoding: chunked, chunked\r\n", get, 501),
            ("two Transfer-Encoding fields",
             b"Transfer-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n", get, 501),
            ("a Transfer-Encoding look-alike", b"Transfer-Encoding\x0b: chunked\r\n", get, 400)]:
        if want == 200:
            reply = exchange(plain, line + header + b"\r\n")
            check(b"200 OK" in reply, f"Refused a valid request ({name}): {reply[:80]!r}")
        else:
            got = status(header, line)
            check(got == want, f"{name}: expected {want}, got {got}")

    # slowloris: the whole header must arrive within idle_timeout (2 s here)
    # however slowly it trickles in
    start = time.monotonic()
    with socket.create_connection(("127.0.0.1", plain), timeout=10) as sock:
        reply = b""
        try:
            sock.sendall(get)
            for _ in range(40):
                sock.sendall(b"X: a\r\n")
                time.sleep(0.25)
                sock.setblocking(False)
                try:
                    data = sock.recv(4096)
                    if not data:
                        break
                    reply += data
                except BlockingIOError:
                    pass
                finally:
                    sock.setblocking(True)
        except (BrokenPipeError, ConnectionResetError):
            pass
    elapsed = time.monotonic() - start
    check(elapsed < 5, f"A trickled header held the connection for {elapsed:.1f} s")
    check(b"200 OK" not in reply, "A trickled header was answered")

    if not args.prefork:
        return  # one connection at a time: no caps to reach

    def wait_closed(sock):
        """Read until the server closes the connection. klind takes a
        connection off the max_conn_per_ip count before it closes it, so
        the next one from this address is let in. One the client closes
        stays counted until its worker notices, which under load can be
        after the next one arrives: CI saw that refuse a connection."""
        try:
            while sock.recv(65536):
                pass
        except ConnectionResetError:
            pass
        except socket.timeout:
            return False
        return True

    def server_close(conn):
        conn.send(get + b"Connection: close\r\n\r\n")
        conn.response()
        check(wait_closed(conn.sock), "Connection: close did not close the connection")
        conn.sock.close()

    # three connections at once per address on 'limits': a fourth is closed
    held = []
    try:
        for _ in range(3):
            conn = Connection(limits)
            conn.send(after)
            check(conn.response()[0].startswith("HTTP/1.1 200"), "A connection under max_conn_per_ip was refused")
            held.append(conn)
        reply = exchange(limits, after)
        check(reply == b"", f"A connection over max_conn_per_ip was answered: {reply[:80]!r}")
        server_close(held.pop())
        check(b"200 OK" in exchange(limits, after), "max_conn_per_ip did not free a closed connection")
        while held:
            server_close(held.pop())
    finally:
        for conn in held:
            conn.sock.close()

    # two SSE/WebSocket connections at once on 'limits': a third gets 503
    streams = []
    try:
        for i in range(2):
            stream = EventStream(limits, f"/sse.kl1?n=1&mark={temp}/cap{i}".encode())
            check(stream.status.startswith("HTTP/1.1 200"), f"A stream under max_streams was refused: {stream.status}")
            streams.append(stream)
        third = EventStream(limits, b"/sse.kl1?n=1", extra=b"Connection: close\r\n")
        check(third.status.startswith("HTTP/1.1 503") and third.fields.get("retry-after") == "5",
              f"A third SSE stream was not refused with 503: {third.status}")
        # its place is free once the server has closed it (CI saw the
        # WebSocket refused at accept when the client closed it instead)
        check(wait_closed(third.sock), "A refused SSE stream was not closed")
        third.close()
        ws = WebSocket(limits)
        check(ws.status == 503, f"A WebSocket over max_streams was not refused: {ws.status}")
        ws.sock.close()
        streams.pop().close()
        # the worker leaves its slot once the page has ended (after the
        # mark file is written): try for a few seconds
        # (meanwhile max_conn_per_ip may still count the closed connections'
        # workers too, and close a new one at once: that is a retry as well)
        deadline, status = time.monotonic() + 5, ""
        while True:
            try:
                again = EventStream(limits, b"/sse.kl1?n=1")
                again.close()
                status = again.status
            except (EOFError, ConnectionResetError):
                status = "closed"
            if status.startswith("HTTP/1.1 200") or time.monotonic() > deadline:
                break
            time.sleep(0.2)
        check(status.startswith("HTTP/1.1 200"), f"max_streams did not free a closed stream: {status}")
    finally:
        for stream in streams:
            stream.close()


def run_session_checks(args, check, plain, client, files, limits, sessions, memlog):
    """Session hardening (doc/SESSIONS.md), on memory (plain), file (files)
    and client-side (client) sessions: an unknown id is not taken on
    (session fixation), session_regenerate_id() at login, CSRF tokens,
    client sessions sealed with AES-256-GCM, changes on pages larger than
    the response buffer; then the idle and absolute timeouts (limits: 2 s
    and 4 s)."""

    def call(port, query, cookies, method=b"GET", body=b"", extra=b""):
        head = (method + b" /sessec.kl1?" + query + b" HTTP/1.1\r\nHost: localhost\r\n" + extra +
                b"Cookie: " + "; ".join(f"{k}={v}" for k, v in cookies.items()).encode() + b"\r\n")
        if method == b"POST":
            head += (b"Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
                     str(len(body)).encode() + b"\r\n")
        reply = exchange(port, head + b"\r\n" + body)
        header, _, text = reply.partition(b"\r\n\r\n")
        status = int(header.split(b" ", 2)[1]) if header.startswith(b"HTTP/") else 0
        new = dict(cookies)
        for line in header.decode("latin-1").split("\r\n"):
            if line.lower().startswith("set-cookie:"):
                for k, v in SimpleCookie(line.split(":", 1)[1].strip()).items():
                    if v.value:
                        new[k] = v.value
                    else:
                        new.pop(k, None)
        return status, new, text.decode("latin-1").strip()

    planted = "0123456789abcdef0123456789abcdef"
    for port, kind in [(plain, "memory"), (files, "file"), (client, "client")]:
        # an id the client chose is not taken on: a new one is drawn
        status, cookies, text = call(port, b"op=set&v=a", {"klin_sid": planted})
        check(status == 200 and cookies.get("klin_sid") not in (None, planted),
              f"{kind}: a planted session id was kept: {cookies}")
        check(call(port, b"", {"klin_sid": planted})[2] == "v=- user=-",
              f"{kind}: the planted id holds the session")
        if kind == "file":
            check(not list(sessions.glob(f"*{planted}*")),
                  f"file: an unknown id created a session file: {list(sessions.iterdir())}")
        check(call(port, b"", cookies)[2] == "v=a user=-", f"{kind}: session not kept")

        # login: a new id, the data kept, the old id worthless (server side)
        old = dict(cookies)
        status, cookies, text = call(port, b"op=login", cookies)
        check(status == 200 and cookies["klin_sid"] != old["klin_sid"],
              f"{kind}: no new id at login: {old} {cookies}")
        check(call(port, b"", cookies)[2] == "v=a user=alice", f"{kind}: data lost at login")
        if kind != "client":
            check(call(port, b"", old)[2] == "v=- user=-", f"{kind}: the id before login still works")

        # CSRF: GET passes; POST needs the token, in a field or a form value
        status, cookies, token = call(port, b"op=csrf", cookies)
        check(status == 200 and len(token) == 64, f"{kind}: bad CSRF token {token!r}")
        check(call(port, b"op=change", cookies)[0] == 200, f"{kind}: GET refused by the CSRF check")
        check(call(port, b"op=change", cookies, b"POST", b"x=1")[0] == 403,
              f"{kind}: POST without a CSRF token accepted")
        check(call(port, b"op=change", cookies, b"POST", b"x=1",
                   b"X-CSRF-Token: " + ("0" * 64).encode() + b"\r\n")[0] == 403,
              f"{kind}: a wrong CSRF token accepted")
        check(call(port, b"op=change", cookies, b"POST", b"x=1",
                   b"X-CSRF-Token: " + token.encode() + b"\r\n")[0] == 200,
              f"{kind}: the CSRF token in X-CSRF-Token refused")
        check(call(port, b"op=change", cookies, b"POST", b"csrf_token=" + token.encode())[0] == 200,
              f"{kind}: the CSRF token as a form value refused")

        if kind == "client":
            # sealed with AES-256-GCM: no HMAC cookie, and any change to the
            # data, the mtime or the id is refused
            check("KL1_CLISES_HMAC" not in cookies and "KL1_CLISES_IV" not in cookies,
                  f"client: old-format cookies written: {sorted(cookies)}")
            data = cookies["KL1_CLISES_DATA"]
            for name, value in [("KL1_CLISES_DATA", data[:40] + ("0" if data[40] != "0" else "1") + data[41:]),
                                ("KL1_CLISES_MTIME", str(int(cookies["KL1_CLISES_MTIME"]) + 1)),
                                ("klin_sid", planted)]:
                forged = dict(cookies, **{name: value})
                check(call(port, b"", forged)[2] == "v=- user=-",
                      f"client: a session with a changed {name} was accepted")
            check(call(port, b"", cookies)[2] == "v=a user=alice", "client: sealed session not read")
            # The plain data fits in 4 KB, but the sealed hex cookie does not.
            oversized = b"&v=" + b"x" * 2048
            for op in (b"set", b"big"):
                status, fresh, _ = call(port, b"op=" + op + oversized, {})
                check(status == 200 and not fresh,
                      f"client: {op!r} published a partial session: {sorted(fresh)}")
            status, fresh, _ = call(port, b"op=set&v=" + b"x" * 1975, {})
            check(status == 200 and not fresh,
                  "client: published a session whose hex and attributes exceed the limit")
            status, rotated, _ = call(port, b"op=login" + oversized, cookies)
            check(status == 200 and rotated == cookies,
                  "client: a failed save replaced the cookies during ID rotation")
            check(call(port, b"", rotated)[2] == "v=a user=alice",
                  "client: failed rotation lost the previously saved session")

        # a page that prints more than the 4,096-byte buffer commits its
        # headers while it runs: a change made before that is kept, on a
        # new session as well as an old one
        status, cookies, text = call(port, b"op=big&v=b", cookies)
        check(status == 200 and len(text) > 4096, f"{kind}: large page failed: {status} {len(text)}")
        check(call(port, b"", cookies)[2] == "v=b user=alice",
              f"{kind}: a change on a large page was lost")
        status, fresh, _ = call(port, b"op=big&v=n", {})
        check(call(port, b"", fresh)[2] == "v=n user=-", f"{kind}: a new session on a large page was lost")
        # a change after the commit: kept on the server, but the cookies of
        # a client session have gone (a warning is logged instead)
        status, cookies, _ = call(port, b"op=late&v=c", cookies)
        check(call(port, b"", cookies)[2] == ("v=b" if kind == "client" else "v=c") + " user=alice",
              f"{kind}: wrong session after a change made once the headers had gone")

    if args.prefork:
        # a client session changed after the headers had gone: the warning
        # is in the log ('memlog' keeps client sessions, and its log in
        # memory under prefork)
        late_warning = b"client session changed after the response headers were sent"
        call(memlog, b"op=big&v=" + b"x" * 2048, {})
        reply = exchange(memlog, b"GET /memlog.kl1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
        check(b"could not save the client session" in reply and late_warning not in reply,
              f"Client session save failure was misreported: {reply[-600:]!r}")
        status, cookies, _ = call(memlog, b"op=set&v=a", {})
        call(memlog, b"op=late&v=c", cookies)
        reply = exchange(memlog, b"GET /memlog.kl1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
        check(late_warning in reply,
              f"No warning for a client session changed too late: {reply[-600:]!r}")

    # idle (2 s) and absolute (4 s) timeouts on 'limits'
    status, cookies, _ = call(limits, b"op=set&v=idle", {})
    time.sleep(3)
    check(call(limits, b"", cookies)[2] == "v=- user=-", "The idle timeout did not end the session")
    # used every second (never idle), it must end between 4 s and 6 s (the
    # creation time is kept in whole seconds)
    status, cookies, _ = call(limits, b"op=set&v=busy", {})
    start, ended = time.monotonic(), None
    while ended is None and time.monotonic() - start < 6.5:
        time.sleep(1)
        status, cookies, text = call(limits, b"", cookies)
        if text != "v=busy user=-":
            ended = time.monotonic() - start
    check(ended is not None and ended >= 3.9,
          f"The absolute timeout ended a session in use at {ended} s (4 s to 6 s)")


def make_client_ca(temp):
    """A test CA and a client certificate it signs (mutual TLS)."""
    def ssl_cmd(*args):
        subprocess.run([OPENSSL, *args], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ca, ca_key = temp / "ca.pem", temp / "ca-key.pem"
    ccert, ckey, csr = temp / "client.pem", temp / "client-key.pem", temp / "client.csr"
    ssl_cmd("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(ca_key),
            "-out", str(ca), "-days", "1", "-subj", "/CN=Klin test CA")
    ssl_cmd("req", "-newkey", "rsa:2048", "-nodes", "-keyout", str(ckey), "-out", str(csr),
            "-subj", "/CN=alice")
    ssl_cmd("x509", "-req", "-in", str(csr), "-CA", str(ca), "-CAkey", str(ca_key),
            "-CAcreateserial", "-out", str(ccert), "-days", "1")
    return ca, ccert, ckey


def run_tls_checks(check, secure, mtls, client_cert):
    """TLS settings (doc/TLS.md): TLS 1.3 when the client has it; for TLS 1.2,
    forward-secret AEAD ciphers only; no renegotiation; session tickets
    whose keys change (2 s on 'mtls'); client certificates ('mtls'
    requires one)."""

    def handshake(port, context):
        with socket.create_connection(("127.0.0.1", port), timeout=5) as raw:
            with context.wrap_socket(raw, server_hostname="localhost") as tls:
                return tls.version(), tls.cipher()[0]

    def context(minimum=ssl.TLSVersion.TLSv1_2, maximum=ssl.TLSVersion.TLSv1_3, ciphers=None,
                cert=False):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE  # ephemeral local test certificate
        ctx.minimum_version, ctx.maximum_version = minimum, maximum
        if ciphers:
            ctx.set_ciphers(ciphers)
        if cert:
            ctx.load_cert_chain(*client_cert)
        return ctx

    version, _ = handshake(secure, context())
    check(version == "TLSv1.3", f"TLS 1.3 not chosen: {version}")
    tls12 = ssl.TLSVersion.TLSv1_2
    version, cipher = handshake(secure, context(tls12, tls12))
    check(version == "TLSv1.2" and cipher.startswith(("ECDHE-", "DHE-")) and
          ("GCM" in cipher or "CHACHA20" in cipher), f"TLS 1.2 cipher not modern: {cipher}")
    for weak in ["AES128-SHA:AES256-SHA:AES128-GCM-SHA256",  # no forward secrecy
                 "ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES256-SHA384"]:  # CBC
        try:
            handshake(secure, context(tls12, tls12, weak))
            check(False, f"A TLS 1.2 client with only {weak} was accepted")
        except (ssl.SSLError, ConnectionResetError):
            check(True, "")

    # renegotiation (TLS 1.2: openssl s_client's R command) is refused
    # (not -quiet: it sends R as data); stdin stays open for the attempt
    client = subprocess.Popen([OPENSSL, "s_client", "-connect", f"127.0.0.1:{secure}", "-tls1_2"],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    client.stdin.write(b"R\n")
    client.stdin.flush()
    time.sleep(2)
    client.stdin.close()
    client.stdin = None     # closed: communicate() must not flush it (Python 3.12)
    out = client.communicate(timeout=20)[0].decode("latin-1")
    check("RENEGOTIATING" in out and "no renegotiation" in out,
          f"Renegotiation not refused: {out[-600:]}")

    # mutual TLS: no certificate, no connection; with one, the handshake
    # and a request work
    try:
        with socket.create_connection(("127.0.0.1", mtls), timeout=5) as raw:
            with context().wrap_socket(raw, server_hostname="localhost") as tls:
                tls.sendall(b"GET /static/hello.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
                got = tls.recv(100)
        check(not got.startswith(b"HTTP/1.1 200"), "A client without a certificate was served")
    except (ssl.SSLError, ConnectionResetError, BrokenPipeError):
        check(True, "")
    reply = exchange(mtls, b"GET /static/hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n",
                     context(cert=True))
    check(b"200 OK" in reply, f"A client with a certificate was refused: {reply[:200]!r}")

    # session tickets (openssl s_client keeps the session in a file): resumed
    # by any worker, until the key has changed twice (2 s each)
    saved = Path(client_cert[0]).with_name("tls-session.pem")

    def visit(version, resume):
        command = [OPENSSL, "s_client", "-connect", f"127.0.0.1:{mtls}", version,
                   "-cert", str(client_cert[0]), "-key", str(client_cert[1]),
                   "-sess_in" if resume else "-sess_out", str(saved)]
        if not resume:
            command.append("-ign_eof")   # read the response (and the ticket)
        out = subprocess.run(command, input=b"GET /static/hello.txt HTTP/1.0\r\n\r\n",
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20).stdout
        return out.decode("latin-1")

    for version in ["-tls1_2", "-tls1_3"]:
        first = visit(version, False)
        check("\nNew, " in first, f"{version}: no handshake: {first[-400:]}")
        again = [visit(version, True) for _ in range(3)]   # prefork: several workers
        check(all("\nReused, " in out for out in again),
              f"{version} session not resumed: {[o[-300:] for o in again]}")
        # a ticket of the period before is still taken: made just after a
        # period began, used in the next (the keys were stepped forward)
        while time.time() % 2 > 0.2:
            time.sleep(0.02)
        visit(version, False)
        time.sleep(2.3)
        previous = visit(version, True)
        check("\nReused, " in previous,
              f"{version} ticket of the previous period not taken: {previous[-300:]}")
        # two periods on, its key is gone: made just after a period began,
        # used about 4.5 s later, within the session's own timeout (3
        # periods), so only the retired key can refuse it
        while time.time() % 2 > 0.2:
            time.sleep(0.02)
        visit(version, False)
        time.sleep(4.3)
        late = visit(version, True)
        check("\nNew, " in late, f"{version} session resumed after its ticket key expired")


def run_worker_limit_checks(args, check, plain, limits, sanitized):
    """Worker resource limits (doc/LIMITS.md): the defaults on 'plain' (1024
    MB, 1024 files, max_child 2 + 64 processes), 256 MB, 64 files and no
    process limit on 'limits'; past them, malloc() and open() fail and the
    worker goes on. Prefork only (-d has no workers); the memory limit is
    not set in sanitizer builds."""
    if not args.prefork:
        return

    def page(port, query=b""):
        reply = exchange(port, b"GET /limits.kl1?" + query + b" HTTP/1.1\r\nHost: localhost\r\n\r\n")
        return reply.partition(b"\r\n\r\n")[2].decode().strip()

    def limits_of(port):
        return dict(kv.split("=") for kv in page(port).split())

    got = limits_of(plain)
    # macOS refuses RLIMIT_AS, so klind does not set it there
    no_as = sanitized or sys.platform == "darwin"
    check(got["nofile"] == "1024" and got["nproc"] == "66" and
          got["as"] == ("-1" if no_as else str(1024 * 1024 * 1024)),
          f"Wrong default worker limits: {got}")
    got = limits_of(limits)
    check(got["nofile"] == "64" and got["as"] == ("-1" if no_as else str(256 * 1024 * 1024)) and
          got["nproc"] != "70", f"Wrong configured worker limits: {got}")
    check(page(limits, b"files=20") == "opened 20", "A worker could not open 20 files under 64")
    check(page(limits, b"files=200").startswith("Too many open files"),
          f"worker_max_files not enforced: {page(limits, b'files=200')}")
    if not no_as:
        check(page(limits, b"alloc=16") == "allocated", "A worker could not allocate 16 MB under 256")
        big = page(limits, b"alloc=512")
        check(big == "ENOMEM", f"worker_max_memory not enforced: {big!r}")
        check(page(limits, b"alloc=16") == "allocated", "The worker did not go on after ENOMEM")


BIG_FILE = 32 << 20     # more than the socket buffers hold


def make_file_tree(temp):
    """Files for run_file_checks (doc/FILES.md): /pub (pub) with links whose
    targets are inside and outside it, a FIFO, the CGI scripts (cgi-bin)
    and the files of the open.test vhost (pub-open)."""
    pub, cgi, other = temp / "pub", temp / "cgi-bin", temp / "pub-open"
    for d in (pub / "sub", cgi, other):
        d.mkdir(parents=True)
    (pub / "hello.txt").write_text("public file\n")
    (pub / "big.bin").write_bytes(b"x" * BIG_FILE)
    (pub / "sub/s.txt").write_text("in a subdirectory\n")
    (pub / "in").symlink_to("hello.txt")
    (pub / "out").symlink_to("../secret.txt")
    (pub / "dirlink").symlink_to("..")
    (pub / "subl").symlink_to("sub")
    os.mkfifo(pub / "fifo")
    (other / "only.txt").write_text("open vhost\n")
    (other / "out").symlink_to("../secret.txt")
    scripts = {
        "env.sh": "printf 'Content-Type: text/plain\\r\\n\\r\\n'\nenv\n",
        "limits.sh": "printf 'Content-Type: text/plain\\r\\n\\r\\n'\n"
                     "echo \"cpu=$(ulimit -t) as=$(ulimit -v) core=$(ulimit -c)\"\n",
        "sleep.sh": "sleep 31\n",
        "late.sh": "printf 'Content-Type: text/plain\\r\\n\\r\\npartial\\n'\nsleep 32 &\nwait\n",
        "spin.sh": "printf 'Content-Type: text/plain\\r\\n\\r\\nspinning\\n'\nwhile :; do :; done\n",
        "closed.sh": "printf 'Content-Type: text/plain\\r\\n\\r\\nclosed\\n'\nexec >&-\nsleep 33\n",
        "unread.sh": "exec sleep 47\n",
    }
    for name, body in scripts.items():
        (cgi / name).write_text("#!/bin/sh\n" + body)
        (cgi / name).chmod(0o755)
    (temp / "outside.sh").write_text("#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\noutside\\n'\n")
    (temp / "outside.sh").chmod(0o755)
    (cgi / "linked.sh").symlink_to("../outside.sh")


def files_config(temp):
    """The fs and CGI settings of the 'files' server, and its vhosts: the
    symlink policies, and the suppliers turned off."""
    return f"""dir_alias /pub {temp / 'pub'}
    post_timeout 5
    send_timeout 2
    cgi
    {{
        script_alias /cgi-bin {temp / 'cgi-bin'}
        timeout 3
        max_cpu 1
        max_memory 64
        pass_env KLIN_TEST_PASS
    }}
    strict.test
    {{
        dir_root /www
        dir_alias /pub {temp / 'pub'}
        follow_symlinks no
    }}
    open.test
    {{
        dir_root /www
        dir_alias /pub {temp / 'pub-open'}
        follow_symlinks yes
    }}
    off.test
    {{
        dir_root /www
        dir_alias /pub {temp / 'pub'}
        cgi
        {{
            script_alias /cgi-bin {temp / 'cgi-bin'}
        }}
        serve_files no
        run_cgi no
    }}
    slow.test
    {{
        dir_root /www
        cgi
        {{
            script_alias /cgi-bin {temp / 'cgi-bin'}
            timeout 0
        }}
    }}"""


def run_tls_send_timeout_checks(check, port):
    """An HTTPS client that stops reading leaves the server free to serve
    another request before the stalled client drains its receive buffer."""
    context = client_context(ssl.TLSVersion.TLSv1_3)
    with socket.create_connection(("127.0.0.1", port), timeout=10) as raw:
        with context.wrap_socket(raw, server_hostname="localhost") as sock:
            sock.sendall(b"GET /static/pub/big.bin HTTP/1.1\r\nHost: localhost\r\n\r\n")
            head = sock.recv(16384)
            check(head.startswith(b"HTTP/1.1 200"), f"TLS large file: {head[:80]!r}")
            # Give the two-second write deadline time to expire before
            # checking the server. Prefork may serve this with a spare.
            time.sleep(5)
            reply = exchange(port, b"GET /static/hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n",
                             context)
            check(reply.endswith(b"klin integration test\n"),
                  f"HTTPS stopped serving during a send_timeout: {reply[:200]!r}")
            got = len(head)
            try:
                while chunk := sock.recv(65536):
                    got += len(chunk)
            except (ConnectionResetError, ssl.SSLEOFError):
                pass
            check(got < BIG_FILE, f"TLS send_timeout: a stalled client got {got} bytes")


def run_file_checks(check, files, temp, cgi):
    """File system and CGI hardening (doc/FILES.md, doc/CGI.md): paths that try to
    leave the document root, encoded and Windows-style; the symlink policies
    (inside by default, no, yes); a raw file system path is never looked
    up (it was: If-Modified-Since told whether a file existed); the path
    info split for the vhost's own files; serve_files and run_cgi; and, when
    the build has CGI, the environment allowlist and the limits."""

    def get(path, host=b"localhost", extra=b""):
        request = b"GET " + path + b" HTTP/1.1\r\nHost: " + host + b"\r\n" + extra + b"\r\n"
        start = time.monotonic()
        reply = exchange(files, request, timeout=10)
        status = reply.split(b" ", 2)[1] if reply.startswith(b"HTTP/") else b"none"
        return status, reply.partition(b"\r\n\r\n")[2], time.monotonic() - start

    status, body, _ = get(b"/pub/hello.txt")
    check(status == b"200" and body == b"public file\n", f"/pub not served: {status} {body!r}")

    # out of the root: secret.txt is next to pub, served if the path got out
    escapes = [b"/pub/../secret.txt", b"/pub/%2e%2e/secret.txt", b"/pub/%2E%2E%2Fsecret.txt",
               b"/pub/..%2fsecret.txt", b"/pub/.%2e/secret.txt", b"/pub/sub/..%2f..%2fsecret.txt",
               b"/pub/..\\secret.txt", b"/pub/..%5csecret.txt", b"/pub/%2e%2e%5csecret.txt",
               b"/pub/sub\\..\\..\\secret.txt", b"/pub/%252e%252e/secret.txt",
               b"/pub/..%c0%afsecret.txt", b"/pub/..%25%32%66secret.txt", b"/pub//../secret.txt",
               b"/pub/./../secret.txt", b"/pub/" + str(temp / "secret.txt").encode(),
               b"/pub/%2f" + str(temp / "secret.txt").lstrip("/").encode(),
               b"/pub/C:%5cWindows%5cwin.ini", b"/pub/hello.txt::$DATA", b"/pub/hello.txt.",
               b"/pub/HELLO~1.TXT", b"/pub/dirlink/secret.txt", b"/pub/out",
               str(temp / "secret.txt").encode(), b"//etc/passwd", b"/pub/../../../../etc/passwd"]
    for path in escapes:
        status, body, _ = get(path)
        check(status in (b"400", b"404") and b"top secret" not in body and b"root:" not in body,
              f"{path!r} left the document root: {status} {body[:80]!r}")
    status, _, _ = get(b"/pub/hello.txt%00.txt")
    check(status == b"400", f"A NUL in the path was not refused: {status}")

    # a raw file system path: with If-Modified-Since, 304 said it existed
    for path in [b"/etc/passwd", str(temp / "secret.txt").encode()]:
        status, _, _ = get(path, extra=b"If-Modified-Since: Fri, 01 Jan 2100 00:00:00 GMT\r\n")
        check(status == b"404", f"{path!r} looked up from the file system's root: {status}")

    # follow_symlinks inside (default), no and yes
    for host, path, want in [
            (b"localhost", b"/pub/in", b"public file\n"),
            (b"localhost", b"/pub/subl/s.txt", b"in a subdirectory\n"),
            (b"localhost", b"/pub/out", None),
            (b"localhost", b"/pub/dirlink/secret.txt", None),
            (b"localhost", b"/pub/fifo", None),
            (b"strict.test", b"/pub/hello.txt", b"public file\n"),
            (b"strict.test", b"/pub/in", None),
            (b"strict.test", b"/pub/subl/s.txt", None),
            (b"open.test", b"/pub/out", b"top secret\n")]:
        status, body, _ = get(path, host)
        if want is None:
            check(status == b"404", f"{host!r} {path!r}: link followed: {status} {body[:80]!r}")
        else:
            check(status == b"200" and body == want, f"{host!r} {path!r}: {status} {body[:80]!r}")

    # the path info is split against the files of the request's vhost (the
    # default one has no only.txt)
    status, body, _ = get(b"/pub/only.txt/extra", b"open.test")
    check(status == b"200" and body == b"open vhost\n",
          f"Path info not split for the vhost's files: {status} {body[:80]!r}")

    # serve_files no, run_cgi no: embedded pages are still served
    for path in [b"/pub/hello.txt", b"/cgi-bin/env.sh"]:
        status, _, _ = get(path, b"off.test")
        check(status == b"404", f"{path!r} served with serve_files/run_cgi no: {status}")
    status, _, _ = get(b"/pid.kl1", b"off.test")
    check(status == b"200", f"Embedded page not served with serve_files no: {status}")

    # send_timeout 2: a client that stops reading is dropped, and gets only
    # what the socket buffers held, not the whole file
    with socket.create_connection(("127.0.0.1", files), timeout=10) as sock:
        sock.sendall(b"GET /pub/big.bin HTTP/1.1\r\nHost: localhost\r\n\r\n")
        time.sleep(5)
        got, head = 0, b""
        try:
            while chunk := sock.recv(1 << 20):
                head = head or chunk[:16]
                got += len(chunk)
        except OSError:
            pass
    check(head.startswith(b"HTTP/1.1 200") and got < BIG_FILE,
          f"send_timeout: a client that stopped reading got {got} bytes ({head!r})")
    status, _, _ = get(b"/pub/hello.txt")
    check(status == b"200", "The server did not go on after a send_timeout")

    if not cgi:
        return

    # the environment: request fields but the credentials, Proxy (httpoxy)
    # and names with an underscore; klind's own variables pass_env lists
    status, body, _ = get(b"/cgi-bin/env.sh", extra=b"X-Fine: yes\r\nProxy: http://evil:1/\r\n"
                          b"Authorization: Basic YTpi\r\nX_Under: x\r\nCookie: c=1\r\n")
    variables = dict(line.split("=", 1) for line in body.decode().splitlines() if "=" in line)
    check(status == b"200" and variables.get("HTTP_X_FINE") == "yes" and
          variables.get("HTTP_COOKIE") == "c=1" and variables.get("KLIN_TEST_PASS") == "passed",
          f"CGI environment lost a variable: {status} {body[:400]!r}")
    for name in ["HTTP_PROXY", "HTTP_AUTHORIZATION", "HTTP_X_UNDER", "KLIN_TEST_SECRET"]:
        check(name not in variables, f"{name} passed to a CGI")
    check(variables.get("REQUEST_METHOD") == "GET" and variables.get("GATEWAY_INTERFACE") == "CGI/1.1",
          f"CGI meta-variables missing: {sorted(variables)}")

    status, body, _ = get(b"/cgi-bin/limits.sh")
    # macOS refuses RLIMIT_AS, so a CGI has none there
    as_limit = b"unlimited" if sys.platform == "darwin" else b"65536"
    check(status == b"200" and body.strip() == b"cpu=1 as=" + as_limit + b" core=0",
          f"CGI limits not set: {status} {body!r}")

    # a script out of its directory through a link
    status, body, _ = get(b"/cgi-bin/linked.sh")
    check(status == b"404" and b"outside" not in body, f"CGI link out of script_alias run: {status}")

    # timeout 3: no header yet gives 504; one killed in its output (with
    # what it started) ends it and the connection
    status, body, took = get(b"/cgi-bin/sleep.sh")
    check(status == b"504" and took < 6, f"CGI timeout: {status} after {took:.1f}s")
    status, body, took = get(b"/cgi-bin/late.sh")
    check(status == b"200" and body.startswith(b"partial") and took < 6,
          f"CGI timeout in its output: {status} {body[:80]!r} after {took:.1f}s")
    time.sleep(0.3)
    left = subprocess.run(["pgrep", "-f", "sleep 3[12]"], capture_output=True, text=True).stdout
    check(not left.strip(), f"Processes of killed CGIs left: {left}")

    # a script that closes its output and runs on is still killed by the
    # timeout, and the response then goes out, on a connection that is not
    # kept open (the client asked for keep-alive)
    with socket.create_connection(("127.0.0.1", files), timeout=8) as sock:
        start = time.monotonic()
        sock.sendall(b"GET /cgi-bin/closed.sh HTTP/1.1\r\nHost: localhost\r\n\r\n")
        reply = b""
        try:
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                reply += chunk
        except OSError:
            pass
        took = time.monotonic() - start
    head, _, body = reply.partition(b"\r\n\r\n")
    check(head.startswith(b"HTTP/1.1 200") and body == b"closed\n" and took < 6 and
          b"\r\nconnection: close" in head.lower(),
          f"CGI that closed its output: {reply[:300]!r} after {took:.1f}s")
    time.sleep(0.3)
    left = subprocess.run(["pgrep", "-f", "sleep 33"], capture_output=True, text=True).stdout
    check(not left.strip(), f"A CGI that closed its output still runs: {left}")

    # a POST body that stops arriving: post_timeout (5) closes the
    # connection (the script's timeout kills only the script)
    with socket.create_connection(("127.0.0.1", files), timeout=15) as sock:
        start = time.monotonic()
        sock.sendall(b"POST /cgi-bin/env.sh HTTP/1.1\r\nHost: localhost\r\n"
                     b"Content-Type: text/plain\r\nContent-Length: 100\r\n\r\npartial")
        try:
            while sock.recv(65536):
                pass
            closed = True
        except OSError:
            closed = False
        took = time.monotonic() - start
    check(closed and 4 < took < 9, f"CGI POST body: connection not closed by post_timeout "
          f"({'closed' if closed else 'open'} after {took:.1f}s)")

    # a script that never reads its stdin, with no timeout of its own
    # (slow.test: timeout 0), while more is POSTed than the pipe holds:
    # post_timeout cuts the relay to the script too, which is killed
    body = b"x" * (1 << 20)
    with socket.create_connection(("127.0.0.1", files), timeout=15) as sock:
        start = time.monotonic()
        try:
            sock.sendall(b"POST /cgi-bin/unread.sh HTTP/1.1\r\nHost: slow.test\r\n"
                         b"Content-Type: application/octet-stream\r\n"
                         b"Content-Length: %d\r\n\r\n" % len(body) + body)
            while sock.recv(65536):
                pass
            closed = True
        except (ConnectionResetError, BrokenPipeError):
            closed = True
        except OSError:
            closed = False
        took = time.monotonic() - start
    time.sleep(0.3)
    left = subprocess.run(["pgrep", "-f", "sleep 47"], capture_output=True, text=True).stdout
    check(closed and 4 < took < 9 and not left.strip(),
          f"CGI that does not read its POST body: {'closed' if closed else 'open'} "
          f"after {took:.1f}s, left running: {left.strip()!r}")

    # max_cpu 1: killed for its CPU time before the timeout
    status, body, took = get(b"/cgi-bin/spin.sh")
    check(status == b"200" and body.startswith(b"spinning") and took < 2.8,
          f"CGI max_cpu not enforced: {status} after {took:.1f}s")

    status, _, _ = get(b"/pub/hello.txt")
    check(status == b"200", "The server did not go on after the CGI checks")


def run_bad_auth_checks(check, daemon, env, temp):
    """Unsafe or broken auth (and route) configurations stop klind at
    startup, instead of serving the realm unprotected (the reason goes to
    the log)."""
    port = free_port()
    hashed = auth_users(daemon.parent / "klin", temp)[0]

    def starts(auth, block="auth"):
        config = temp / "auth-start.conf"
        config.write_text(f"server_list one\nallow_root yes\none\n{{\ntype http\n"
                          f"addr tcp4://127.0.0.1:{port}\ndir_root /www\n"
                          f"{block}\n{{\n{auth}\n}}\n}}\n")
        process = subprocess.Popen([str(daemon), "-F", "-d", "-f", str(config)], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            process.wait(timeout=2)
            return False
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            return True

    realm = "realm\n{\nname R\npath /x*\n%s}\n"
    check(starts(hashed + realm % ""), "A valid auth configuration was refused")
    for name, auth in [
            ("plaintext password", "user\n{\nname eve\npassword hunter2\n}\n" + realm % ""),
            ("Basic over HTTP", hashed + realm % "scheme basic\n"),
            ("realm without path", hashed + "realm\n{\nname R\n}\n"),
            ("unknown scheme", hashed + realm % "scheme ntlm\n"),
            ("quote in realm", hashed + realm.replace("name R", 'name R"') % ""),
            ("missing users_file", hashed + "users_file /nonexistent/users\n" + realm % "")]:
        check(not starts(auth), f"klind started with an unsafe auth configuration ({name})")

    route = "route\n{\n%s\n}\n"
    check(starts(route % "path /a/{id}/{rest*}\npage /p.kl1\nmethod GET, post", "routes"),
          "A valid route was refused")
    for name, body in [("relative path", "path a/{id}\npage /p.kl1"),
                       ("bad parameter name", "path /a/{i-d}\npage /p.kl1"),
                       ("catch-all not last", "path /a/{rest*}/b\npage /p.kl1"),
                       ("unbalanced brace", "path /a/{id\npage /p.kl1"),
                       ("no page", "path /a/{id}"),
                       ("relative page", "path /a\npage p.kl1"),
                       ("unknown method", "path /a\npage /p.kl1\nmethod GET FETCH")]:
        check(not starts(route % body, "routes"), f"klind started with a bad route ({name})")

    check(starts(route % "allow 10.0.0.0/8, ::1 fe80::/10\ndeny all", "access"),
          "A valid access rule was refused")
    for name, body in [("prefix too long", "allow 10.0.0.0/33"),
                       ("IPv6 prefix too long", "allow ::1/129"),
                       ("short address", "allow 10.1.2"),
                       ("host name", "allow localhost"),
                       ("unknown key", "permit 10.0.0.0/8"),
                       ("no allow or deny", "path /x")]:
        check(not starts(route.replace("route", "rule") % body, "access"),
              f"klind started with a bad access rule ({name})")

    def validated(validators, params):
        return starts(validators + "\n}\nparams\n{\n" + params, "validators")
    good = "validator\n{\nname n\nint 1 9\n}"
    check(validated(good, "path /x\nparam\n{\nname a\nvalidator n\n}"),
          "A valid validation configuration was refused")
    for name, validators, params in [
            ("no type", "validator\n{\nname n\n}", "path /x"),
            ("bad regex", "validator\n{\nname n\nregex [a-\n}", "path /x"),
            ("int range reversed", "validator\n{\nname n\nint 9 1\n}", "path /x"),
            ("negative length", "validator\n{\nname n\nlength -1 5\n}", "path /x"),
            ("defined twice", good + "\n" + good, "path /x"),
            ("unknown validator", good, "path /x\nparam\n{\nname a\nvalidator m\n}"),
            ("bad source", good, "path /x\nparam\n{\nname a\nsource cookie\n}"),
            ("bad unknown", good, "path /x\nunknown ignore"),
            ("no path", good, "param\n{\nname a\n}")]:
        check(not validated(validators, params), f"klind started with bad validation ({name})")
    upload = "path /x\nupload\n{\nname f\n%s\n}"
    check(validated(good, upload % "max_size 1024\ntype text/plain, image/*\nmax_count 3\n"
                    "required yes"), "A valid upload rule was refused")
    for name, body in [("no name", "path /x\nupload\n{\nmax_size 5\n}"),
                       ("named twice", upload % "" + "\nupload\n{\nname f\n}"),
                       ("negative max_size", upload % "max_size -1"),
                       ("max_size not a number", upload % "max_size 1M"),
                       ("max_count 0", upload % "max_count 0"),
                       ("type without subtype", upload % "type text"),
                       ("any type", upload % "type */*"),
                       ("type not a token", upload % "type text/pl@in"),
                       ("type with two slashes", upload % "type text/plain/x"),
                       ("empty type list", upload % "type ,"),
                       ("unknown key", upload % "maxsize 5")]:
        check(not validated(good, body), f"klind started with a bad upload rule ({name})")


def run_root_checks(check, daemon, env, temp):
    """klind will not keep root credentials: started as root it must drop
    to a configured uid and gid, or be told 'allow_root yes' (a chroot alone
    is not enough, root can leave one). Run as root only (WSL, CI with sudo)."""
    if not hasattr(os, "geteuid"):
        return
    port = free_port()

    def started(settings):
        """The pid of a klind that came up with 'settings', or None."""
        config = temp / "root-start.conf"
        config.write_text(f"server_list one\n{settings}\none\n{{\ntype http\n"
                          f"addr tcp4://127.0.0.1:{port}\ndir_root /www\n}}\n")
        process = subprocess.Popen([str(daemon), "-F", "-d", "-f", str(config)], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 5
        while process.poll() is None and time.monotonic() < deadline:
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                return process
            except OSError:
                time.sleep(0.1)
        if process.poll() is None:
            process.kill()
        process.wait()
        return None

    def stop(process):
        """Kill 'process'; its user and group ids where /proc has them
        (Linux), else None."""
        uids = gids = None
        status = Path(f"/proc/{process.pid}/status")
        if status.exists():
            text = status.read_text()
            uids = text.split("Uid:")[1].split()[:4]
            gids = text.split("Gid:")[1].split()[:4]
        process.kill()
        process.wait()
        return uids, gids

    if os.geteuid() != 0:
        process = started("")
        check(process is not None, "klind did not start as an ordinary user")
        stop(process)
        return
    check(started("") is None, "klind kept root credentials without allow_root")
    check(started(f"chroot {temp}") is None, "klind kept root credentials in a chroot")
    check(started("uid 65534") is None, "klind kept group root (uid without gid)")
    process = started("uid 65534\ngid 65534")
    check(process is not None, "klind did not start with uid and gid configured")
    if process is not None:
        uids, gids = stop(process)
        check(uids is None or uids == ["65534"] * 4 and gids == ["65534"] * 4,
              f"klind did not drop every user and group id: {uids} {gids}")
    process = started("allow_root yes")
    check(process is not None, "allow_root yes did not let klind run as root")
    if process is not None:
        stop(process)


def run_server_checks(args, env, output, daemon):
    checks = 0

    def check(condition, message):
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(message)

    with tempfile.TemporaryDirectory(prefix="klin-server-test-") as directory:
        temp = Path(directory)
        (temp / "hello.txt").write_text("klin integration test\n")
        (temp / "a+b.txt").write_text("literal plus\n")
        (temp / "a?b.txt").write_text("encoded question\n")
        (temp / "secret.txt").write_text("top secret\n")
        (temp / "old.txt").write_text("old client\n")
        make_file_tree(temp)
        has_cgi = cmake_cache_value(daemon.parent.parent, "KLIN_ENABLE_SUP_CGI") == "ON"
        env["KLIN_TEST_PASS"] = "passed"
        env["KLIN_TEST_SECRET"] = "hidden"
        auth = auth_config(*auth_users(daemon.parent / "klin", temp))
        run_bad_auth_checks(check, daemon, env, temp)
        run_root_checks(check, daemon, env, temp)
        cert, key = temp / "cert.pem", temp / "key.pem"
        subprocess.run([OPENSSL, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=localhost"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        run_keymgr_start_checks(check, daemon, env, temp, cert, key)
        run_keymgr_dump_checks(check, daemon, env, temp, cert, key)
        run_bad_tls_checks(check, daemon, env, temp, cert, key)
        run_ipv6_checks(args, check, daemon, env, temp, cert, key)
        run_bad_server_checks(check, daemon, env, temp)
        run_orphan_checks(check, daemon, env, temp)
        run_pid_file_checks(check, daemon, env, temp)
        run_blind_chroot_checks(check, daemon, env, temp)
        plain, client, secure, files, memlog, blocked, limits, mtls = (free_port() for _ in range(8))
        ca, client_cert, client_key = make_client_ca(temp)
        run_verify_depth_checks(check, daemon, env, temp, cert, key, ca, (client_cert, client_key))
        sessions = temp / "sessions"
        sessions.mkdir()
        config = temp / "klind.conf"
        blocks = []
        # memlog: a log on this server only (none at the top level), kept in
        # memory, which pages read back with server_foreach_memlog_line()
        for name, port, tls, session, extra in [
                ("plain", plain, False, "memory",
                 auth + "\n    " + ROUTES + "\n    " + VALIDATION),
                ("client", client, False, "client", SECURITY_HEADERS + "\n    " + ACCESS),
                ("secure", secure, True, "client", "send_timeout 2\n    " + auth),
                ("files", files, False, f"file\n        file.path {sessions}",
                 # worker_max_procs 0: RLIMIT_NPROC counts all the processes
                 # of the user running the tests (hundreds on a desktop),
                 # and the CGIs here must fork
                 "index missing.kl1 pid.kl1\n    send_enc_deflate yes\n    worker_max_procs 0\n    "
                 + files_config(temp)),
                ("memlog", memlog, False, "client",
                 "log\n    {\n        type memory\n    }\n    sandbox no"),
                ("blocked", blocked, False, "memory", BLOCKED),
                ("limits", limits, False,
                 "memory\n        idle_timeout 2s\n        absolute_timeout 4s", LIMITS),
                ("mtls", mtls, True, "memory",
                 f"ca_file {ca}\n    verify_mode require\n    tls_ticket_key_lifetime 2s")]:
            blocks.append(f"""{name}
{{
    type {'https' if tls else 'http'}
    addr tcp4://127.0.0.1:{port}
    dir_root /www
    dir_alias /static {temp}
    idle_timeout 2
    keep_alive_timeout 1
    prefork.start_child 1
    prefork.max_child 2
    prefork.idle_timeout 1
    session
    {{
        type {session}
    }}
    {f'cert_file {cert}' if tls else ''}
    {f'key_file {key}' if tls else ''}
    {extra}
}}
""")
        # 'limits' sets its own keep-alive and worker counts
        at = next(i for i, b in enumerate(blocks) if b.startswith("limits\n"))
        blocks[at] = re.sub(r"\n    (keep_alive_timeout|prefork\.start_child|prefork\.max_child) \d+",
                            "", blocks[at])
        # blanks of every kind between the names: all servers must start (a
        # doubled or leading blank used to drop the ones after it)
        config.write_text("server_list  plain client\tsecure  files memlog blocked limits mtls\n"
                          "allow_root yes\n" +
                          "\n".join(blocks))
        log_path = output / ("server-prefork-test.log" if args.prefork else "server-test.log")
        with log_path.open("w") as log:
            command = [str(daemon), "-F", "-f", str(config)]
            if not args.prefork:
                command.append("-d")
            process = subprocess.Popen(command,
                                       env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                deadline = time.monotonic() + 15
                while True:
                    if process.poll() is not None:
                        raise RuntimeError(log_path.read_text()[-6000:])
                    try:
                        with socket.create_connection(("127.0.0.1", plain), timeout=0.2):
                            break
                    except OSError:
                        if time.monotonic() > deadline:
                            raise TimeoutError(log_path.read_text()[-6000:])
                        time.sleep(0.1)
                request = b"GET /static/hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n"
                response = exchange(plain, request)
                check(b"200 OK" in response and response.endswith(b"klin integration test\n"), "GET failed")
                check(b"\r\nServer: klin/" in response, f"Wrong Server header: {response[:300]!r}")
                head = exchange(plain, request.replace(b"GET", b"HEAD", 1))
                check(b"200 OK" in head and head.endswith(b"\r\n\r\n"), "HEAD returned a body")
                for path, body in [(b"a+b.txt", b"literal plus\n"), (b"a%3Fb.txt", b"encoded question\n")]:
                    result = exchange(plain, request.replace(b"hello.txt", path))
                    check(result.endswith(body), f"URI decoding failed: {path!r}")
                malformed = [
                    b"Content-Length: -1\r\n", b"Content-Length: 1x\r\n",
                    b"Content-Length: 9999999999999999999999999999\r\n",
                    b"Content-Length: 0\r\nContent-Length: 0\r\n",
                    b"Transfer-Encoding: chunked\r\n",
                    b"Transfer-Encoding: chunked\r\nContent-Length: 0\r\n",
                    b"Bad Header: value\r\n", b"X: a\r\n folded\r\n",
                    b"X: " + b"x" * 8192 + b"\r\n",
                    b"X: a\r\n" * 257,
                    (b"X: " + b"x" * 1000 + b"\r\n") * 70,
                ]
                for header in malformed:
                    reply = exchange(plain, b"GET /static/hello.txt HTTP/1.1\r\n" + header + b"\r\n")
                    check(b"200 OK" not in reply, f"Accepted malformed headers: {header[:80]!r}")
                for path in [b"hello.txt%00suffix", b"%", b"%4", b"%GG"]:
                    reply = exchange(plain, request.replace(b"hello.txt", path))
                    check(b"200 OK" not in reply, f"Accepted bad URI: {path!r}")
                incomplete = exchange(plain, b"GET /static/hello.txt HTTP/1.1\r\nHost: x\r\n", half_close=True)
                check(b"200 OK" not in incomplete, "Accepted incomplete headers")
                for version in [ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3]:
                    reply = exchange(secure, request, client_context(version))
                    check(b"200 OK" in reply, f"TLS failed: {version.name}")
                for version in [ssl.TLSVersion.TLSv1, ssl.TLSVersion.TLSv1_1]:
                    context = client_context(version)
                    context.set_ciphers("ALL:@SECLEVEL=0")
                    try:
                        exchange(secure, request, context)
                    except (ssl.SSLError, OSError):
                        check(True, "")
                    else:
                        check(False, f"Accepted obsolete TLS: {version.name}")
                for port, context in [(plain, None), (client, None), (files, None),
                                      (secure, client_context(ssl.TLSVersion.TLSv1_3))]:
                    cookies = {}
                    for expected in [b"new", b"persisted"]:
                        cookie_header = "; ".join(f"{k}={v}" for k, v in cookies.items()).encode()
                        reply = exchange(port, b"GET /session.kl1 HTTP/1.1\r\nHost: localhost\r\nCookie: " +
                                         cookie_header + b"\r\n\r\n", context)
                        header, _, body = reply.partition(b"\r\n\r\n")
                        check(b"200 OK" in header and expected in body, f"Session round-trip failed: {reply[:400]!r}")
                        for line in header.decode().split("\r\n"):
                            if line.lower().startswith("set-cookie:"):
                                check("HttpOnly" in line and "SameSite=Lax" in line and "Path=/" in line,
                                      f"Missing cookie attributes: {line}")
                                check(("; Secure" in line) == (context is not None), "Wrong Secure attribute")
                                parsed = SimpleCookie(line.split(":", 1)[1].strip())
                                cookies.update({k: v.value for k, v in parsed.items()})
                    check(len(cookies.get("klin_sid", "")) == 32, "Wrong session ID length")
                    if port == plain:
                        # the cookie's name before KLone was renamed Klin is
                        # still read, so an upgrade keeps users logged in
                        old = f"klone_sid={cookies['klin_sid']}".encode()
                        reply = exchange(port, b"GET /session.kl1 HTTP/1.1\r\nHost: localhost\r\nCookie: " +
                                         old + b"\r\n\r\n", context)
                        check(b"persisted" in reply.partition(b"\r\n\r\n")[2],
                              f"Old klone_sid cookie not accepted: {reply[:400]!r}")
                    if port == files:
                        # a session file saved before the rename (klone_sess_)
                        # is taken over under the new name
                        sid = cookies["klin_sid"]
                        saved = list(sessions.glob(f"klin_sess_{sid}_*"))
                        check(len(saved) == 1, f"no session file: {list(sessions.iterdir())}")
                        old_file = saved[0].with_name(saved[0].name.replace("klin_sess_", "klone_sess_", 1))
                        saved[0].rename(old_file)
                        reply = exchange(port, b"GET /session.kl1 HTTP/1.1\r\nHost: localhost\r\nCookie: klone_sid=" +
                                         sid.encode() + b"\r\n\r\n", context)
                        check(b"persisted" in reply.partition(b"\r\n\r\n")[2],
                              f"Old session file not loaded: {reply[:400]!r}")
                        check(saved[0].exists() and not old_file.exists(),
                              f"Old session file not renamed: {list(sessions.iterdir())}")
                    if "KL1_CLISES_DATA" in cookies:
                        # the last hex digit is in the GCM tag
                        data = cookies["KL1_CLISES_DATA"]
                        cookies["KL1_CLISES_DATA"] = data[:-1] + ("0" if data[-1] != "0" else "1")
                        cookie_header = "; ".join(f"{k}={v}" for k, v in cookies.items()).encode()
                        reply = exchange(port, b"GET /session.kl1 HTTP/1.1\r\nHost: localhost\r\nCookie: " +
                                         cookie_header + b"\r\n\r\n", context)
                        check(b"persisted" not in reply.partition(b"\r\n\r\n")[2],
                              "Accepted a forged client-session tag")
                # a multipart/form-data POST: a field and an uploaded file
                body = (b"--XyZ\r\nContent-Disposition: form-data; name=\"field\"\r\n\r\n"
                        b"value 1\r\n--XyZ\r\nContent-Disposition: form-data; name=\"file\"; "
                        b"filename=\"a.txt\"\r\nContent-Type: text/plain\r\n\r\nfile body\r\n"
                        b"--XyZ--\r\n")
                reply = exchange(plain, b"POST /upload.kl1 HTTP/1.1\r\nHost: localhost\r\n"
                                 b"Content-Type: multipart/form-data; boundary=XyZ\r\n"
                                 b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
                page = reply.partition(b"\r\n\r\n")[2]
                check(b"200 OK" in reply and b"field=value 1\n" in page and
                      b"file=a.txt text/plain 9 [file body]\n" in page,
                      f"multipart upload: {reply[:600]!r}")
                # a configured index list: the first page that exists is served
                reply = exchange(files, b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
                body = reply.partition(b"\r\n\r\n")[2].strip()
                check(b"200 OK" in reply and body.isdigit(), f"index pid.kl1 not served: {reply[:400]!r}")
                if args.prefork:
                    # a worker logs to the server's own memory log (there is
                    # no top-level log) and reads it back through the parent:
                    # the worker used to crash on the missing top-level log,
                    # and lines came back cut to 7 characters
                    reply = exchange(memlog, b"GET /memlog.kl1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    check(b"200 OK" in reply and
                          b"memlog probe line 0123456789" in reply.partition(b"\r\n\r\n")[2],
                          f"memory log not read back whole: {reply[:600]!r}")
                run_auth_checks(args, check, plain, secure)
                run_header_checks(check, plain, client, secure)
                run_conditional_checks(check, plain, files)
                run_sse_checks(args, check, plain, secure, temp)
                run_websocket_checks(check, plain, secure)
                run_limit_checks(args, check, plain, limits, temp)
                run_session_checks(args, check, plain, client, files, limits, sessions, memlog)
                run_tls_checks(check, secure, mtls, (client_cert, client_key))
                run_worker_limit_checks(args, check, plain, limits,
                                        "-fsanitize" in cmake_cache_value(daemon.parent.parent,
                                                                          "CMAKE_C_FLAGS"))
                run_route_checks(args, check, plain)
                run_file_checks(check, files, temp, has_cgi)
                run_tls_send_timeout_checks(check, secure)
                run_access_checks(check, client, blocked, temp)
                run_validation_checks(check, plain)
                run_sandbox_checks(args, check, process.pid, plain, memlog, files if has_cgi else None,
                                   "-fsanitize" in cmake_cache_value(daemon.parent.parent,
                                                                     "CMAKE_C_FLAGS"))
                run_channel_checks(args, check, plain)
                run_keymgr_checks(check, process.pid, key,
                                  "-fsanitize" in cmake_cache_value(daemon.parent.parent,
                                                                    "CMAKE_C_FLAGS"))
                run_keep_alive_checks(args, check, plain, secure, request)
                # last: a page busy for over a second makes prefork start a
                # spare worker, which the keep-alive checks would then see
                run_keymgr_policy_checks(check, plain)
                check(b"200 OK" in exchange(plain, request), "Server failed after malformed requests")
            finally:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                    stopped = True
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                    stopped = False
            # SIGTERM stops the server by itself (the handler only sets the
            # stop flag; the main loop logs it and shuts down the children)
            check(stopped, "SIGTERM did not stop the server within 5 s")
        log_text = log_path.read_text(errors="replace")
        check("ERROR: AddressSanitizer" not in log_text and "ERROR: LeakSanitizer" not in log_text
              and "AddressSanitizer:DEADLYSIGNAL" not in log_text and "runtime error:" not in log_text,
              "Sanitizer errors in server-test.log")
        if args.prefork:
            # a negative count is refused at startup (it used to be written
            # as an int into a size_t, i.e. a huge number of children)
            bad = temp / "negative.conf"
            bad.write_text(f"server_list b\nallow_root yes\nb\n{{\n type http\n"
                           f" addr tcp4://127.0.0.1:{free_port()}\n dir_root /www\n"
                           " model prefork\n prefork.max_child -1\n}\n")
            try:
                result = subprocess.run([str(daemon), "-F", "-f", str(bad)], env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
                refused = result.returncode != 0
            except subprocess.TimeoutExpired:
                refused = False     # it started and kept running
            check(refused, "prefork.max_child -1 was accepted")
    print(f"{checks} full-server checks passed; log: {log_path}")


if __name__ == "__main__":
    main()
