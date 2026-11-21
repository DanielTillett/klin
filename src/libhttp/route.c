/*
 * Copyright (c) 2026 Daniel Tillett
 * All rights reserved.
 *
 * This file is part of Klin (derived from KLone), and as such it is subject to the license stated
 * in the LICENSE file which you have received as part of this distribution.
 */

/* REST-style routes (doc/ROUTES.md): a vhost's "routes" block maps methods
 * and path patterns such as /api/users/{id} or /files/{path*} to the page
 * that serves them. The page reads the parameters with
 * request_get_route_param(). A path that matches a route for other methods
 * only gets 405 with an Allow header. */

#include "klin_conf.h"
#include <string.h>
#include <u/libu.h>
#include <klin/http.h>
#include <klin/request.h>
#include <klin/response.h>
#include <klin/route.h>
#include <klin/vhost.h>

enum { ROUTE_PATH_MAX = 4096 };

static const char *route_methods[] = { "GET", "HEAD", "POST", "PUT",
    "DELETE", "PATCH", NULL };

/* a route, parsed once by route_load(): the methods it takes are bits,
 * 1 << i for route_methods[i], with ROUTE_ANY ("*" or no "method" key)
 * also taking methods outside the table */
#define ROUTE_ANY   (~0U)

struct route_s
{
    const char *path;       /* the configuration's strings                   */
    const char *page;
    unsigned methods;
};

/* the bit of a method; one outside the table only matches ROUTE_ANY */
static unsigned route_method_bit(const char *method, size_t n)
{
    unsigned i;

    for(i = 0; route_methods[i]; ++i)
        if(strlen(route_methods[i]) == n &&
                !strncasecmp(route_methods[i], method, n))
            return 1U << i;
    return 1U << 31;
}

/* the methods of a "method" list (NULL: any; GET also serves HEAD); ~0 if
 * it names one outside the table */
static int route_method_mask(const char *list, unsigned *pmask)
{
    const char *p, *e;
    unsigned mask = 0, bit;
    int any = (list == NULL);

    for(p = list; p && *p; p = e)
    {
        p += strspn(p, " \t,");
        e = p + strcspn(p, " \t,");
        if(e == p)
            continue;
        if(e - p == 1 && *p == '*')
            any = 1;
        else if((bit = route_method_bit(p, (size_t) (e - p))) == 1U << 31)
            return ~0;
        else if(bit == route_method_bit("GET", 3))
            mask |= bit | route_method_bit("HEAD", 4);
        else
            mask |= bit;
    }
    *pmask = any ? ROUTE_ANY : mask;
    return 0;
}

/* a {name} or {name*} segment of a pattern: the name's length, and *rest
 * set for {name*}; 0 if seg is a literal */
static size_t route_param(const char *seg, size_t len, int *rest)
{
    if(len < 3 || seg[0] != '{' || seg[len - 1] != '}')
        return 0;
    *rest = (seg[len - 2] == '*');
    return len - 2 - (size_t) *rest;
}

/* match 'path' against 'pat' (both start with '/'); with rq, add the
 * parameters to it. A trailing '/' counts: "/a" does not match "/a/" */
static int route_match(const char *pat, const char *path, request_t *rq)
{
    char name[64];
    const char *ps = pat, *pe, *qs = path, *qe;
    size_t n, plen, qlen;
    int rest = 0;

    for(;;)
    {
        if(*ps == '\0' && *qs == '\0')
            return 1;
        if(*ps != '/' || *qs != '/')
            return 0;
        ps++;
        qs++;
        pe = ps + strcspn(ps, "/");
        qe = qs + strcspn(qs, "/");
        plen = (size_t) (pe - ps);
        qlen = (size_t) (qe - qs);

        if((n = route_param(ps, plen, &rest)) == 0)
        {   /* a literal segment */
            if(plen != qlen || strncmp(ps, qs, plen))
                return 0;
        } else {
            if(rest)
                qlen = strlen(qs);  /* the rest, slashes included */
            else if(qlen == 0)
                return 0;           /* {name} is one non-empty segment */
            if(rq)
            {
                memcpy(name, ps + 1, n);
                name[n] = '\0';
                dbg_err_if(request_add_route_param(rq, name, qs, qlen));
            }
            if(rest)
                return 1;
        }
        ps = pe;
        qs = qe;
    }
err:
    return 0;
}

/* the full request path: filename and path_info */
static int route_path(request_t *rq, char *buf, size_t size)
{
    const char *fn = request_get_filename(rq), *pi = request_get_path_info(rq);

    nop_err_if(fn == NULL);
    nop_err_if(u_snprintf(buf, size, "%s%s", fn, pi ? pi : ""));

    return 0;
err:
    return ~0;
}

/**
 * \brief   Route a request to the page that serves it
 *
 * The first route of the vhost whose path and method match: the request
 * is given the route's page and parameters, and *routed is set.
 *
 * \return \c 0 (routed, or no route for the path) or
 *         ::HTTP_STATUS_METHOD_NOT_ALLOWED (the path has routes for other
 *         methods only; see route_allow())
 */
int route_dispatch(http_t *h, request_t *rq, int *routed)
{
    char path[ROUTE_PATH_MAX];
    const char *method;
    const route_t *route;
    vhost_t *vhost;
    unsigned bit;
    size_t i;
    int other = 0;

    dbg_return_if (h == NULL || rq == NULL || routed == NULL, ~0);

    *routed = 0;
    if((vhost = http_get_vhost(h, rq)) == NULL || vhost->nroutes == 0)
        return 0;
    if(route_path(rq, path, sizeof path))
        return 0;

    method = request_get_method_name(rq);
    bit = route_method_bit(method, strlen(method));
    for(i = 0; i < vhost->nroutes; ++i)
    {
        route = &vhost->routes[i];
        if(!route_match(route->path, path, NULL))
            continue;
        if(!(route->methods & bit))
        {
            other = 1;
            continue;
        }
        dbg_err_if(request_set_route(rq, route->page));
        (void) route_match(route->path, path, rq);
        *routed = 1;
        return 0;
    }

    return other ? HTTP_STATUS_METHOD_NOT_ALLOWED : 0;
err:
    return HTTP_STATUS_INTERNAL_SERVER_ERROR;
}

/**
 * \brief   The Allow header of a 405: the methods the path's routes take
 */
int route_allow(http_t *h, request_t *rq, response_t *rs)
{
    char path[ROUTE_PATH_MAX], allow[128] = "";
    unsigned mask = 0, i;
    vhost_t *vhost;
    size_t r;

    dbg_return_if (h == NULL || rq == NULL || rs == NULL, ~0);

    nop_err_if((vhost = http_get_vhost(h, rq)) == NULL || vhost->nroutes == 0);
    nop_err_if(route_path(rq, path, sizeof path));

    for(r = 0; r < vhost->nroutes; ++r)
        if(route_match(vhost->routes[r].path, path, NULL))
            mask |= vhost->routes[r].methods;

    for(i = 0; route_methods[i]; ++i)
    {
        if(!(mask & (1U << i)))
            continue;
        if(allow[0])
            (void) u_strlcat(allow, ", ", sizeof allow);
        (void) u_strlcat(allow, route_methods[i], sizeof allow);
    }

    return response_set_field(rs, "Allow", allow);
err:
    return ~0;
}

/* a pattern: starts with '/', parameters {name} or {name*} (letters,
 * digits, '_'), {name*} only last */
static int route_pattern_ok(const char *pat)
{
    const char *s, *e;
    size_t n, i;
    int rest = 0;

    if(pat == NULL || pat[0] != '/' || strlen(pat) >= ROUTE_PATH_MAX)
        return 0;
    for(s = pat + 1;; s = e + 1)
    {
        e = s + strcspn(s, "/");
        if(memchr(s, '{', (size_t) (e - s)) || memchr(s, '}', (size_t) (e - s)))
        {
            if((n = route_param(s, (size_t) (e - s), &rest)) == 0 || n >= 64)
                return 0;
            for(i = 1; i <= n; ++i)
                if(!(s[i] == '_' || (s[i] >= 'a' && s[i] <= 'z') ||
                            (s[i] >= 'A' && s[i] <= 'Z') ||
                            (s[i] >= '0' && s[i] <= '9')))
                    return 0;
            if(rest && *e)
                return 0;
        }
        if(*e == '\0')
            return 1;
    }
}

/**
 * \brief   Load and check a vhost's "routes" block when it is created (a bad
 *          route stops the server); route_free() frees what it makes
 */
int route_load(vhost_t *vhost)
{
    u_config_t *routes, *rc;
    route_t *route;
    int i, n;

    dbg_return_if (vhost == NULL, ~0);

    if((routes = u_config_get_child(vhost->config, "routes")) == NULL)
        return 0;

    for(n = 0; u_config_get_child_n(routes, "route", n); ++n)
        ;
    if(n)
        dbg_err_if((vhost->routes = u_zalloc(n * sizeof *route)) == NULL);

    for(i = 0; (rc = u_config_get_child_n(routes, "route", i)); ++i)
    {
        route = &vhost->routes[vhost->nroutes];
        route->path = u_config_get_subkey_value(rc, "path");
        warn_err_ifm(!route_pattern_ok(route->path), "routes: bad path "
                "pattern '%s' (a '/', then segments or {name}, {name*} last)",
                route->path ? route->path : "(none)");
        route->page = u_config_get_subkey_value(rc, "page");
        warn_err_ifm(route->page == NULL || route->page[0] != '/',
                "routes: route %s needs a page (an absolute path)",
                route->path);
        warn_err_ifm(route_method_mask(u_config_get_subkey_value(rc,
                        "method"), &route->methods),
                "routes: route %s: unknown method", route->path);
        vhost->nroutes++;
    }

    return 0;
err:
    return ~0;
}

/* free what route_load() made */
void route_free(vhost_t *vhost)
{
    if(vhost == NULL)
        return;
    U_FREE(vhost->routes);
    vhost->nroutes = 0;
}
