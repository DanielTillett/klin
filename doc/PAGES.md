# Writing a site

A Klin site is a directory that is compiled into `klind`. This page shows
how a site is laid out, how a page is written, and how to build and run
it. The example site in `doc/example-site` follows the same layout.

## The layout

```
mysite/
  etc/klind.conf      the configuration klind uses (klind.conf(5))
  www/index.kl1       a page: HTML with C code in it
  www/style.css       static files: served as they are
  www/images/...
```

Everything under the site directory becomes klind's embedded file system.
With `dir_root /www` in a server block (as in the example site), the URL
`/index.kl1` is served from `www/index.kl1`, `/style.css` from
`www/style.css`, and `/` from the index page (`index` in klind.conf(5)).
Files with the extensions `.kl1` (and `.klin`, or `.klone` from KLone) are
pages; every other file is static and served with a type taken from its
extension and an ETag. With `-DKLIN_IMPORT_ARGS=-z`, static files are
stored compressed and sent compressed to clients that accept it.

## A page

A page is HTML with C in it. Four tags hold the C:

| Tag | What it holds | Where it goes |
| --- | --- | --- |
| `<%! ... %>` | declarations: `#include` lines, helper functions, file-scope variables | outside the page's function |
| `<% ... %>` | statements | in the page's function, in order with the HTML |
| `<%= expr %>` | an expression giving a string (`const char *`), printed as it is | `io_printf(out, "%s", expr)` |
| `<%@ include "file" %>` | another file of the site, included before translation | in its place |

The HTML around the tags is written out as it is, in order. Every page
has these variables:

| Variable | Type | What it is |
| --- | --- | --- |
| `request` | `request_t *` | the request: arguments, fields, cookies, uploads (`klin/request.h`) |
| `response` | `response_t *` | the response: status, header fields, cookies (`klin/response.h`) |
| `session` | `session_t *` | the client's session ([SESSIONS.md](SESSIONS.md)) |
| `out` | `io_t *` | the response body: write to it with `io_printf()`, `io_write()` |
| `in` | `io_t *` | the request body, for a page that reads it raw |
| `SCRIPT_NAME` | `const char *` | the page's file name |

## Hello, world

```html
<%!
    #include <string.h>
%>
<%
    /* ?name=...: escape it before it goes into HTML */
    const char *name = request_get_arg(request, "name");
    char safe[256];

    if (name == NULL || u_htmlncpy_s(safe, sizeof safe, name, strlen(name),
                HTMLCPY_ENCODE) < 0)
        (void) u_strlcpy(safe, "world", sizeof safe);
%>
<!DOCTYPE html>
<html>
  <body>
    <p>Hello, <%= safe %>!</p>
  </body>
</html>
```

`<%= %>` and `io_printf()` print exactly what they are given. Anything
that came from the client (arguments, header fields, cookies, uploaded
data) must be escaped before it is written into HTML, as above, or the
page is open to cross-site scripting. Klin does not escape for you (TODO
lists an escaping output tag as planned work). For values that should
only ever be numbers, names or other known shapes, declare them in a
`validators` block and Klin refuses anything else before the page runs
([VALIDATE.md](VALIDATE.md)).

## Includes

`<%@ include "header.inc" %>` copies `header.inc` (a path relative to the
page) into the page before it is translated, so shared HTML and C can live
in one place. Editing an included file rebuilds the pages that include it.

## The response

A page sets the status and header fields before it writes its body:

```c
response_set_status(response, HTTP_STATUS_NOT_FOUND);
response_set_content_type(response, "application/json");
response_set_field(response, "Cache-Control", "no-store");
```

For JSON, routes such as `/api/users/{id}`, Server-Sent Events and
WebSockets, see [JSON.md](JSON.md), [ROUTES.md](ROUTES.md), [SSE.md](SSE.md)
and [WEBSOCKET.md](WEBSOCKET.md).

## Building and running

```sh
cmake -S . -B out -G Ninja -DKLIN_SITE_DIR=/path/to/mysite
cmake --build out
out/bin/klind -F
```

`-F` keeps klind in the foreground; `-d` also logs for debugging and
serves requests one at a time in a single process. Without `-f`, klind
uses the `etc/klind.conf` embedded with the site; `-f file` uses that
file instead (see klind(8)). Editing a page and running `cmake --build out`
again retranslates only that page.

Pages can be encrypted in the build: give the import the pages to encrypt
and a key file with an absolute path, e.g.
`-DKLIN_IMPORT_ARGS="-e;*.kl1;-k;/path/to/key"`. Pages are re-encrypted
when the key file changes; a key given in the `KLIN_CIPHER_KEY`
environment variable is not tracked, so rebuild from clean after changing
it (klin(1)).

## Application code and hooks

C code shared by many pages (a device's own library, for example) can be
compiled into klind with `-DKLIN_WEBAPP_SOURCES="a.c;b.c"`; a page then
declares and calls its functions (`webapp/webapp.c` is a small example).

With `-DKLIN_ENABLE_HOOKS=ON`, klind calls a `hooks_setup()` function you
provide, which registers functions to run when the server starts and
stops, when each worker starts and stops, and for every request
(`klin/hook.h`: `hook_server_init()`, `hook_child_init()`,
`hook_request()` and others). A worker's `child_init` hook runs before its
sandbox ([SANDBOX.md](SANDBOX.md)), so it can open what the pages need.
