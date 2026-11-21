# CGI scripts

klind can run Common Gateway Interface (CGI) scripts, under time, CPU and
memory limits and with a pared-down environment. They run only in a build with `-DKLIN_ENABLE_SUP_CGI=ON` (not
available on Windows). They run from the directories a `cgi` block names,
or through the handler of a file extension:

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
| `timeout` | 60 | Seconds of wall-clock time the script may run. It is then killed (SIGKILL), with its process group. |
| `max_cpu` | 30 | Seconds of CPU time (RLIMIT_CPU): SIGXCPU, then SIGKILL a second later. |
| `max_memory` | 512 | MB of address space (RLIMIT_AS). |

0 sets no limit. What a timeout looks like to the client depends on when
it happens. Before the script has sent its header, the client gets 504
Gateway Timeout. In the middle of its output, the output stops there and
the connection is closed. The script runs in a process group of its own,
and the kill goes to the script and that group. A child that moves to a
group or session of its own (with setsid(2), say) escapes it.

The catch with the timeout is that it is wall-clock time, including the
time a slow client takes to read the output.

The limits are set in the script's process, soft and hard, so it cannot
raise them. It gets no core dumps (RLIMIT_CORE 0), and no descriptor of
the worker's except its standard input, output and error. All others are
closed, with close_range(2) where the system has it, or by closing every
descriptor number up to 65536 (or the hard limit, if that is higher)
elsewhere. Its processes also count towards the worker's
`worker_max_procs` ([LIMITS](LIMITS.md)).

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

## Tested

test/run_server_tests.py runs on both server models, so prefork with the
sandbox. It runs scripts that:

- print their environment, while the client sends Proxy, Authorization,
  an underscored name, and klind has a variable that is not listed (none
  of these may arrive);
- print their limits;
- sleep past the timeout (504), or past it after their header (cut short,
  and the sleep they started is gone too);
- spin on the CPU (killed after `max_cpu`, before the timeout);
- are reached through a link out of the script directory (404).

See also: [FILES](FILES.md) for where scripts may live,
[SANDBOX](SANDBOX.md), [LIMITS](LIMITS.md).
