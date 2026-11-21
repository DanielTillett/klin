# CGI scripts

klind can run Common Gateway Interface (CGI) scripts, under time, CPU and
memory limits and with a pared-down environment. They run only in a build
with `-DKLIN_ENABLE_SUP_CGI=ON` (not available on Windows). They run from
the directories a `cgi` block names, or through the handler of a file
extension:

```conf
cgi
{
    script_alias    /cgi-bin /srv/www/cgi-bin
    php.handler     /usr/bin/php-cgi

    timeout         60
    max_cpu         30
    max_memory      512
    pass_env        PATH LANG
    pass_headers    default
}
```

`run_cgi no` turns CGI off for a server or a virtual host, and
`follow_symlinks` applies to scripts too ([FILES](FILES.md)).

## Limits

| Setting | Default | Meaning |
|---|---|---|
| `timeout` | 60 | Seconds of wall-clock time, checked while klind waits on the script or the client (below). When it fires, the script and its process group are killed (SIGKILL). |
| `max_cpu` | 30 | Seconds of CPU time (RLIMIT_CPU): SIGXCPU, then SIGKILL a second later. |
| `max_memory` | 512 | MB of address space (RLIMIT_AS). |

0 means klind sets no limit of its own, so the script keeps the limits
it inherits from the worker ([LIMITS](LIMITS.md)). A value above the
worker's hard limit cannot be granted: the script keeps the inherited
limit, and the worker logs a warning the first time (`cgi max_memory is
above the worker's hard limit`). On macOS `max_memory` cannot be applied
at all, as with `worker_max_memory`, and the worker says so once. The
script runs in a process group of its own, and when the timeout fires
klind kills the script and that group. A child that moves to a group or
session of its own (with setsid(2), say) escapes it.

klind checks the timeout only while the worker is waiting: writing the
POST body to the script, reading the script's output, writing that
output to the client, or waiting for the script to exit once its output
has ended. So it is wall-clock time, and time spent waiting on a slow
client counts towards it. A script that closes its standard output and
carries on running is killed when the timeout fires, and klind then
sends what is left in its buffer.

The timeout kills only the script, though. The client has deadlines of
its own. The rest of a POST body, which klind relays from the client's
socket, must arrive within the server's `post_timeout` (10 minutes by
default), or klind closes the connection. A write to the client that
makes no progress for `send_timeout` seconds (60 by default) closes it
too ([LIMITS](LIMITS.md)). A client that keeps reading, however slowly,
still holds the worker for as long as it reads.

What a timeout looks like to the client depends on when it happens:

| When the timeout fires | What the client gets |
|---|---|
| While klind is still reading the script's header | 504 Gateway Timeout, with no body |
| After the header, or at any point for an `nph-` script (its output is not parsed) | The script's status and whatever output it had sent, then the connection is closed |
| Never (the script ends its output in time) | The script's response, as normal |

A 504 needs the timeout itself: a script that exits without a header
klind can parse gets an error response, not a 504.

A client usually cannot tell a cut response from a complete one. klind
closes the connection after the cut, but it frames what it has as usual.
Output still in its 4,096-byte buffer goes out with a Content-Length that
matches it (the measured length replaces any the script gave), and a
chunked body is ended normally. The client can tell only when more than
the buffer had already gone out under a Content-Length the script set
itself, and the body falls short of it. The server tests' own example gets
a plain 200 with the first line of output. An `nph-` script's output is
passed on as it is, so its framing is the script's own.

The limits are set in the script's process. For `max_memory` the soft and
hard limits are the same, so the script cannot raise them; for `max_cpu`
the hard limit is one second above the soft one, which is what lets
SIGKILL follow SIGXCPU. It gets no core dumps (RLIMIT_CORE 0), and no
descriptor of the worker's except its standard input, output and error.
All others are closed, with close_range(2) where the system has it, or by
closing every descriptor number up to 65536 (or the hard limit, if that is
higher, up to 1,048,576) elsewhere. Its processes also count towards the
worker's `worker_max_procs` ([LIMITS](LIMITS.md)).

## Environment

A script gets the CGI meta-variables of RFC 3875 (`REQUEST_METHOD`,
`QUERY_STRING`, `CONTENT_TYPE`, `CONTENT_LENGTH`, `PATH_INFO`,
`SCRIPT_NAME`, `REMOTE_ADDR`, `REMOTE_USER`, `SERVER_NAME`, `HTTPS`, ...),
and nothing of klind's own environment unless you list it:

```conf
pass_env      PATH LANG TZ
```

Without it, a script has no `PATH`, and a shell then uses its built-in
default path.

The body of a POST with a non-zero Content-Length is copied to the
script's standard input; other methods get an empty standard input. The
copy starts after validation. If a `params` block ([VALIDATE](VALIDATE.md))
matches the script's path and method, klind has already parsed a
URL-encoded or multipart form body by then, so the script does not get
that body on its standard input. Leave such scripts out of your `params`
blocks.

The request's header fields become `HTTP_<NAME>` variables
(Accept-Language becomes `HTTP_ACCEPT_LANGUAGE`). Some never do:

- **Proxy.** `HTTP_PROXY` is read as a proxy setting by many HTTP
  libraries, so a client could send the script's own requests through its
  server ("httpoxy", CVE-2016-5385 and others).
- **Names with characters other than letters, digits and `-`.** `X-User`
  and `X_User` would both become `HTTP_X_USER`, and a proxy in front that
  checks one may pass the other.

`pass_headers` chooses the rest:

```conf
pass_headers  default   # all but Authorization and Proxy-Authorization
pass_headers  all       # those too
pass_headers  Cookie User-Agent Accept    # only these
```

The default holds back the credentials because they are for klind's own
realms ([AUTH](AUTH.md)); the script gets the user in `REMOTE_USER`.

## Upgrading

These settings are new in 4.0. KLone passed every header field (Proxy and
Authorization included) and set no limits. A script that reads the
Authorization field itself needs `pass_headers all`, or a list that
includes Authorization. One that runs longer than a minute, or needs more
CPU or memory, needs higher limits (or 0).

## Sandbox

A server with a `script_alias` lets its workers fork, chdir and execute
([SANDBOX](SANDBOX.md)), and the scripts run under the worker's system
call filter. Scripts that need a call outside it (an interpreter's own,
for example) need `sandbox_allow`, or `sandbox no`.

## What the server tests run

test/run_server_tests.py runs on both server models, so prefork with the
sandbox. It runs scripts that:

- print their environment, while the client sends Proxy, Authorization,
  an underscored name, and klind has a variable that is not listed (none
  of these may arrive);
- print their limits;
- sleep past the timeout (504), or past it after their header (cut short,
  and the sleep they started is gone too), or close their output and
  sleep on (killed by the timeout);
- spin on the CPU (killed after `max_cpu`, before the timeout);
- get a POST body that stops arriving (closed by `post_timeout`);
- are reached through a link out of the script directory (404).

See also: [FILES](FILES.md) for where scripts may live,
[SANDBOX](SANDBOX.md), [LIMITS](LIMITS.md).
