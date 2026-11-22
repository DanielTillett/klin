# Coding conventions

New C code in Klin returns errors one way, frees what it creates, and
keeps global state to the short list below. Old code that does not follow
these rules is a bug, to fix when you next touch it.

The examples below are sketches written to show a rule, not code copied
from the tree, though the functions they call are real.

## Return values

A function that can fail returns a status. Its results go out through
pointer arguments, and those are written only on success.

```c
/* the length of the token at the start of 's' (0 if there is none) */
int token_len(const char *s, size_t *pn)
{
    size_t n;

    dbg_err_if(s == NULL || pn == NULL);

    n = strcspn(s, " \t");
    *pn = n;            /* written only on success */

    return 0;
err:
    return ~0;
}
```

Each kind of function has its own convention:

| Kind | Returns |
| --- | --- |
| `int` status functions | 0 on success, `~0` on error |
| `ssize_t` / size functions | the count, or -1 on error |
| predicates (`is_*`, `has_*`) | 1 or 0. They cannot fail: a bad argument is "no" |
| pointer getters | NULL when absent. Say in the comment whether NULL can also mean an error |

Never return 0 for both "no" and "error". If a predicate can genuinely
fail, write a status function instead.

```c
/* a predicate: a NULL request is simply "no" */
int request_is_head(request_t *rq)
{
    return rq != NULL && request_get_method(rq) == HM_HEAD;
}
```

## Error macros (LibU, `toolbox/carpal.h`)

The error macros log (or not) and jump to the function's `err:` label.
Pick the one that matches how surprised you would be.

| Macro | Use it for |
| --- | --- |
| `dbg_err_if(e)` | an error that should not happen: log (debug builds) and `goto err` |
| `warn_err_if(e)`, `warn_err_ifm()` | an error worth a warning in production (bad configuration, a client error we report) |
| `crit_err_if(e)` | the process cannot usefully go on |
| `nop_err_if(e)` | an expected outcome that uses the error path (for example "no match"): no log |
| `dbg_if(e)` | log and carry on. Only where ignoring the failure is intended; say why in a comment |

```c
int load_page(const char *uri, io_t **pio)
{
    io_t *io = NULL;

    dbg_err_if(uri == NULL || pio == NULL);         /* a caller bug */
    warn_err_ifm(emb_open(uri, &io),                /* worth a warning */
        "no embedded resource for %s", uri);

    *pio = io;
    return 0;
err:
    if(io)
        dbg_if(io_free(io));    /* never dbg_err_if() in here */
    return ~0;
}
```

Take care in the `err:` block itself. Inside it, use `dbg_if()`, never
`dbg_err_if()`, because the latter jumps back to `err` and you loop.

## Ownership

The creator of an object frees it with the matching `*_free()`. A function
that keeps a pointer it was given says so in its comment: "takes
ownership", or "the caller keeps it, and it must outlive x".

```c
/* header_del_field() unlinks the field and hands it back: it is ours now */
dbg_err_if(header_del_field(h, f));
field_free(f);
```

Who owns each of the main types:

| Type | Owner and lifetime |
| --- | --- |
| `request_t`, `response_t` | one per request, created and freed by `http_serve_request()`. Free the request first (its io flush uses the response) |
| `header_t` | owned by its request or response |
| `field_t` | owned by the header it was added to. `header_del_field()` hands it back to the caller, who frees it with `field_free()` |
| `session_t` | created per request by `session_create()`, freed with `session_free()` |
| `vars_t` / `var_t` | a request's args list owns its vars. `args_get` and `args_post` hold the same `var_t` pointers without owning them (see `request_vars()`) |
| `io_t` | `io_free()` frees the codecs attached to it, and closes the descriptor only if the io was created with `IO_FD_CLOSE` |
| `codec_t` | owned by the `io_t` it was added to |
| `vhost_t` | owned by the `http_t`. Its cached strings (`alias`, `rq_temp_dir`) are copies it frees. It also frees its routes, access rules, validators and `params` blocks, parsed when it is created; they point into the configuration for their strings |
| `talarm_t` | `timerm_add()` returns it and `timerm_del()` frees it. A fired alarm is off the list but still allocated: `timerm_del()` it (or reschedule it) |

The alarm is the one that catches people out, so here it is in code:

```c
talarm_t *a = NULL;

dbg_err_if(timerm_add(30, on_timeout, conn, &a));
/* ... the connection is served; the alarm may or may not have fired ... */
timerm_del(a);      /* needed either way: a fired alarm is still allocated */
```

Configuration (`u_config_t`) is read when the server starts and is not
changed afterwards. That is why caches such as `vhost_t`'s may keep values
read from it.

## Global state

klind is single-threaded, and a worker serves one connection at a time.
This is the process-wide state, and what happens to it in a forked child:

| State | In a forked child |
| --- | --- |
| `ctx` (context.h) | shared; the child sets `ctx->pipc` |
| timer list (timer.c) | the child drops the parent's alarms the first time it looks at the list (`timerm_adopt`). The cached pid is reset by `pthread_atfork` (glibc/macOS) |
| field and response-filter caches (field.c, rsfilter.c) | per process, inherited as they are (they only hold free memory) |
| Digest nonce key and replay state (auth.c, http_s.h) | each HTTP backend's signing key is inherited. The nonce counts, eviction floor and last stamp start empty in the child and are updated only in that process |
| server watchdog and spare flags (server.c) | set in the child by `server_spawn_child` |

### Signals

Signal handlers do only async-signal-safe work: set flags (`volatile
sig_atomic_t`), `close(2)` a descriptor, `write(2)`, `_exit(2)`. The main
loop acts on the flags, and does the logging. SIGINT and SIGTERM set
`server->stop` (closing a child's listening socket, so that a blocked
accept fails), and SIGCHLD sets a flag.

```c
static volatile sig_atomic_t children_changed = 0;

static void on_sigchld(int sig)
{
    u_unused_args(sig);
    children_changed = 1;   /* no logging, no malloc: server_loop() reacts */
}
```

### Timeouts

Timeouts do not use signals. An alarm (timer.h) runs in ordinary code,
when it is due and the process is waiting for a descriptor in
`timerm_wait()`. The io drivers wait there while an alarm is set, and
`server_loop` runs the alarms between selects. So a callback may log,
allocate and close the connection (`io_close()` makes the waiting read or
write fail). TLS sockets are non-blocking, so that OpenSSL never waits
inside a read where no alarm can end it.

The one exception is the prefork worker's own SIGALRM (`setitimer`, in
server.c), which must be able to interrupt any code, a slow page or
`accept(2)`. It drives two things: the watchdog, which asks the parent for
one more worker with a single `write(2)` to its PPC channel (the
parent-process IPC channel, whose messages carry at most 8,192 bytes), and
the idle timeout of a spare worker (`server_stop()`).

## Cryptography

Hashes, MACs, ciphers and random bytes go through `klin/crypto.h`
(`kc_*`), and TLS through `klin/tls.h`. Only `src/libutils/crypto.c` and
the TLS layer (`src/libtls`, `src/libio/iossl.c`, `klin/tlsprv.h`) call
OpenSSL, and they use no API deprecated in OpenSSL 3.

OpenSSL is the only backend. I will add a second one only when a device
cannot take OpenSSL, and it would be mbedTLS (Apache 2.0), not wolfSSL
(GPL/commercial). See [CRYPTO](CRYPTO.md).

## Names and text

Public functions are prefixed with their module (`request_`, `response_`,
`header_`, `field_`, `http_`, `server_` ...). The code is C99, with
4-space indents and lines of up to 80 columns. Comments say why, not what.
Messages and comments that mention the KLone name are history, and keep
it.

See also: [CONTRIBUTING](../CONTRIBUTING.md) for getting a change merged,
[CRYPTO](CRYPTO.md) for the `kc_*` API, and [FUZZING](FUZZING.md) for the
fuzz targets and static analysers new code has to pass.
