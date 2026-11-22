/*
 * Copyright (c) 2026 Daniel Tillett
 * All rights reserved.
 *
 * This file is part of Klin (derived from KLone), and as such it is subject to the license stated
 * in the LICENSE file which you have received as part of this distribution.
 */

/* Declarative request validation (doc/VALIDATE.md): a vhost's "validators"
 * define named checks (a regular expression, an integer range, a length
 * range, a list of values) and its "params" blocks, per path and method,
 * say which parameters a page takes, from where, checked by which
 * validator, and which files it takes, how large, of which types and how
 * many. A page then only sees what passed: an invalid or missing required
 * parameter gets 400 (or, with on_invalid drop, is removed), and the
 * arguments and files no rule names are dropped (or refused).
 *
 * validate_load() parses the blocks once, when the vhost is created, so a
 * request only walks arrays. */

#include "klin_conf.h"
#include <errno.h>
#include <regex.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <u/libu.h>
#include <klin/http.h>
#include <klin/request.h>
#include <klin/validate.h>
#include <klin/vars.h>
#include <klin/vhost.h>

enum { VALID_REGEX, VALID_INT, VALID_LENGTH, VALID_ENUM };

struct validator_s
{
    const char *name;
    int type;
    regex_t re;             /* VALID_REGEX, matched against the whole value */
    long long min, max;     /* VALID_INT, VALID_LENGTH                      */
    const char *values;     /* VALID_ENUM, blank separated                  */
};

enum { SRC_QUERY = 1, SRC_FORM = 2, SRC_ROUTE = 4, SRC_HEADER = 8 };
enum { UNKNOWN_DROP, UNKNOWN_REJECT, UNKNOWN_ALLOW };

/* a "param" block */
typedef struct valid_rule_s
{
    const char *name;
    int src;                /* SRC_* bits                                    */
    const validator_t *v;   /* NULL: only presence is checked                */
    int required;
} valid_rule_t;

/* an "upload" block: the files sent in one multipart form field */
typedef struct valid_upload_s
{
    const char *name;
    long long max_size;     /* bytes per file, -1: no limit                  */
    const char *types;      /* allowed Content-Types, blank separated; NULL:
                               any                                           */
    long long max_count;
    int required;
} valid_upload_t;

/* a "params" block; strings point into the configuration */
struct valid_params_s
{
    const char **paths;
    size_t npaths;
    const char *methods;    /* NULL: any                                     */
    int unknown;            /* UNKNOWN_*                                     */
    int drop;               /* on_invalid drop                               */
    valid_rule_t *rules;
    size_t nrules;
    valid_upload_t *uploads;
    size_t nuploads;
};

/* a query or form argument of the request being checked: var NULL once it
   has been removed */
typedef struct valid_arg_s
{
    var_t *var;
    int src;                /* SRC_QUERY or SRC_FORM                         */
} valid_arg_t;

/* parse a whole decimal integer (optional '-', digits, nothing else) */
static int valid_int(const char *s, long long *pv)
{
    char *end;

    if(s == NULL || *s == '\0' || (*s != '-' && (*s < '0' || *s > '9')))
        return ~0;
    errno = 0;
    *pv = strtoll(s, &end, 10);
    return (errno || *end) ? ~0 : 0;
}

/* does 'value' pass validator 'v'? */
static int valid_check(const validator_t *v, const char *value, size_t len)
{
    long long n;
    const char *p, *e;

    switch(v->type)
    {
    case VALID_REGEX:
        return regexec(&v->re, value, 0, NULL, 0) == 0;
    case VALID_INT:
        return valid_int(value, &n) == 0 && n >= v->min && n <= v->max;
    case VALID_LENGTH:
        return (long long) len >= v->min && (long long) len <= v->max;
    case VALID_ENUM:
        for(p = v->values; *p; p = e)
        {
            p += strspn(p, " \t");
            e = p + strcspn(p, " \t");
            if(e > p && (size_t) (e - p) == len && !memcmp(p, value, len))
                return 1;
        }
        return 0;
    }
    return 0;
}

static const validator_t *valid_find(vhost_t *vhost, const char *name)
{
    size_t i;

    for(i = 0; name && i < vhost->nvalidators; ++i)
        if(!strcmp(vhost->validators[i].name, name))
            return &vhost->validators[i];
    return NULL;
}

/* the params block for this path and method, or NULL */
static const valid_params_t *valid_find_params(vhost_t *vhost,
        const char *path, const char *method)
{
    const valid_params_t *pb;
    const char *m, *e;
    size_t n = strlen(method), i, j;
    int ok;

    for(i = 0; i < vhost->nparams; ++i)
    {
        pb = &vhost->params[i];
        for(ok = 0, j = 0; !ok && j < pb->npaths; ++j)
            ok = !fnmatch(pb->paths[j], path, 0);
        if(!ok)
            continue;
        if((m = pb->methods) == NULL)
            return pb;
        for(; *m; m = e)
        {
            m += strspn(m, " \t,");
            e = m + strcspn(m, " \t,");
            if((size_t) (e - m) == n && !strncasecmp(m, method, n))
                return pb;
        }
    }
    return NULL;
}

static int valid_source(const char *s)
{
    if(s == NULL || !strcasecmp(s, "any"))
        return SRC_QUERY | SRC_FORM | SRC_ROUTE;
    if(!strcasecmp(s, "query"))
        return SRC_QUERY;
    if(!strcasecmp(s, "form"))
        return SRC_FORM;
    if(!strcasecmp(s, "route"))
        return SRC_ROUTE;
    if(!strcasecmp(s, "header"))
        return SRC_HEADER;
    return 0;
}

static int valid_yes(u_config_t *c, const char *key, int def)
{
    int b = def;

    (void) u_config_get_subkey_value_b(c, key, def, &b);
    return b;
}

/* does a rule of the block name the argument 'a'? An upload field's name
   also covers a file input left empty (a browser sends it as a part with
   an empty filename and no data), but no other argument of that name */
static int valid_named(request_t *rq, const valid_params_t *pb,
        const valid_arg_t *a)
{
    const char *name = var_get_name(a->var);
    size_t i;

    for(i = 0; i < pb->nrules; ++i)
        if((pb->rules[i].src & a->src) && !strcmp(pb->rules[i].name, name))
            return 1;
    for(i = 0; (a->src & SRC_FORM) && i < pb->nuploads; ++i)
        if(!strcmp(pb->uploads[i].name, name))
            return request_is_empty_file_input(rq, a->var);
    return 0;
}

/* does an argument pass 'v'? A value with a NUL inside (%00) never does:
   what follows the NUL would not be checked */
static int valid_arg_ok(const validator_t *v, var_t *var)
{
    const char *value = var_get_value(var);
    size_t len = value ? strlen(value) : 0, size = var_get_value_size(var);

    if(value == NULL)
        return valid_check(v, "", 0);
    if(size > len + 1)
        return 0;
    return valid_check(v, value, len);
}

/* copy a list's var_t pointers to an array (vars_getn() walks the list,
   so indexing it in a loop would be quadratic) */
typedef struct valid_vec_s
{
    var_t **v;
    size_t n;
} valid_vec_t;

static int valid_cb_collect(var_t *v, void *arg)
{
    valid_vec_t *vec = arg;

    vec->v[vec->n++] = v;
    return 0;
}

static int valid_vec(vars_t *vs, valid_vec_t *vec)
{
    vec->v = NULL;
    vec->n = 0;
    if(vs == NULL || vars_count(vs) == 0)
        return 0;
    dbg_err_if((vec->v = u_malloc(vars_count(vs) * sizeof *vec->v)) == NULL);
    vars_foreach(vs, valid_cb_collect, vec);

    return 0;
err:
    return ~0;
}

static int valid_ptr_cmp(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t) *(var_t * const *) a,
              y = (uintptr_t) *(var_t * const *) b;

    return x < y ? -1 : x > y;
}

/* the request's query and form arguments, each with its source (a POST
   value, or one from the query string), in *pargs (to u_free) */
static int valid_args(request_t *rq, valid_arg_t **pargs, size_t *pn)
{
    valid_vec_t all = { NULL, 0 }, post = { NULL, 0 };
    valid_arg_t *args = NULL;
    size_t i;

    dbg_err_if(valid_vec(request_get_args(rq), &all));
    dbg_err_if(valid_vec(request_get_postargs(rq), &post));
    if(post.n)
        qsort(post.v, post.n, sizeof *post.v, valid_ptr_cmp);
    if(all.n)
        dbg_err_if((args = u_malloc(all.n * sizeof *args)) == NULL);
    for(i = 0; i < all.n; ++i)
    {
        args[i].var = all.v[i];
        args[i].src = (post.n && bsearch(&all.v[i], post.v, post.n,
                    sizeof *post.v, valid_ptr_cmp)) ? SRC_FORM : SRC_QUERY;
    }
    U_FREE(all.v);
    U_FREE(post.v);

    *pargs = args;
    *pn = all.n;
    return 0;
err:
    U_FREE(all.v);
    U_FREE(post.v);
    U_FREE(args);
    return ~0;
}

/* check one rule; 0, or the status to answer with */
static int valid_rule(request_t *rq, const valid_rule_t *r, valid_arg_t *args,
        size_t nargs, int drop)
{
    vars_t *route = request_get_route_params(rq);
    const char *value;
    var_t *var;
    int found = 0;
    size_t i;

    if(r->src & SRC_HEADER)
    {
        if((value = request_get_field_value(rq, r->name)) != NULL)
        {
            if(r->v && !valid_check(r->v, value, strlen(value)))
                return HTTP_STATUS_BAD_REQUEST;     /* not removed: refuse */
            found = 1;
        }
    }

    /* the request's arguments (each value of a repeated name), then the
       route's */
    for(i = 0; (r->src & (SRC_QUERY | SRC_FORM)) && i < nargs; ++i)
    {
        if((var = args[i].var) == NULL || !(args[i].src & r->src) ||
                strcmp(var_get_name(var), r->name))
            continue;
        if(r->v && !valid_arg_ok(r->v, var))
        {
            if(!drop)
                return HTTP_STATUS_BAD_REQUEST;
            dbg_err_if(request_del_arg(rq, var));
            args[i].var = NULL;
            continue;
        }
        found = 1;
    }
    for(i = 0; (r->src & SRC_ROUTE) && route && i < vars_count(route);)
    {
        var = vars_getn(route, i);
        if(strcmp(var_get_name(var), r->name))
        {
            ++i;
            continue;
        }
        if(r->v && !valid_check(r->v, var_get_value(var),
                    strlen(var_get_value(var))))
        {
            if(!drop)
                return HTTP_STATUS_BAD_REQUEST;
            dbg_err_if(vars_del(route, var));
            var_free(var);
            continue;
        }
        found = 1;
        ++i;
    }

    if(!found && r->required)
        return HTTP_STATUS_BAD_REQUEST;

    return 0;
err:
    return HTTP_STATUS_INTERNAL_SERVER_ERROR;
}

/* the length of the run of token characters (RFC 9110, less '*') at the
   start of the 'n' bytes at 's' */
static size_t valid_token(const char *s, size_t n)
{
    size_t i;

    for(i = 0; i < n && s[i] && ((s[i] >= 'a' && s[i] <= 'z') ||
                (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') ||
                strchr("!#$%&'+-.^_`|~", s[i])); ++i)
        ;
    return i;
}

/* does the Content-Type 'mime' match one of 'types', "type/subtype" or
   "type/ *", compared without regard to case? 'mime' must be a whole
   type/subtype, followed only by blanks or by ';' and parameters (which
   are ignored) */
static int valid_type_ok(const char *types, const char *mime)
{
    size_t t = valid_token(mime, SIZE_MAX), n, k;
    const char *p, *e;

    if(t == 0 || mime[t] != '/' ||
            (n = valid_token(mime + t + 1, SIZE_MAX)) == 0)
        return 0;
    n += t + 1;
    p = mime + n + strspn(mime + n, " \t");
    if(*p && *p != ';')
        return 0;

    for(p = types; *p; p = e)
    {
        p += strspn(p, " \t,");
        e = p + strcspn(p, " \t,");
        if((k = (size_t) (e - p)) == 0)
            continue;
        if(k == n && !strncasecmp(p, mime, n))
            return 1;
        if(k == t + 2 && p[k - 1] == '*' && !strncasecmp(p, mime, t + 1))
            return 1;               /* "image/" then any subtype */
    }
    return 0;
}

/* is 'name' an upload field of the block? */
static int valid_upload_named(const valid_params_t *pb, const char *name)
{
    size_t i;

    for(i = 0; i < pb->nuploads; ++i)
        if(!strcmp(pb->uploads[i].name, name))
            return 1;
    return 0;
}

/* check the uploaded files against the block's upload rules, then apply
   unknown to the files in fields no rule names; 0, or the status to answer
   with */
static int valid_uploads(request_t *rq, const valid_params_t *pb)
{
    const valid_upload_t *u;
    valid_vec_t up = { NULL, 0 };
    const char *mime;
    long long count;
    size_t i, j, size;
    int rc = 0;

    dbg_err_if(valid_vec(request_get_uploads(rq), &up));

    for(j = 0; rc == 0 && j < pb->nuploads; ++j)
    {
        u = &pb->uploads[j];
        for(count = 0, i = 0; rc == 0 && i < up.n; ++i)
        {
            if(up.v[i] == NULL || strcmp(var_get_name(up.v[i]), u->name))
                continue;
            dbg_err_if(request_get_upload_info(up.v[i], &size, &mime));
            if(u->max_size >= 0 && (unsigned long long) size >
                    (unsigned long long) u->max_size)
                rc = HTTP_STATUS_REQUEST_TOO_LARGE;
            else if(u->types && !valid_type_ok(u->types, mime))
                rc = HTTP_STATUS_BAD_REQUEST;
            if(rc && pb->drop)
            {
                dbg_err_if(request_del_upload(rq, up.v[i]));
                up.v[i] = NULL;
                rc = 0;
            } else if(rc == 0)
                count++;
        }
        /* too many files, or none for a required field: refused whatever
           on_invalid says */
        if(rc == 0 && (count > u->max_count || (count == 0 && u->required)))
            rc = HTTP_STATUS_BAD_REQUEST;
    }

    for(i = 0; rc == 0 && pb->unknown != UNKNOWN_ALLOW && i < up.n; ++i)
    {
        if(up.v[i] == NULL || valid_upload_named(pb, var_get_name(up.v[i])))
            continue;
        if(pb->unknown == UNKNOWN_REJECT)
            rc = HTTP_STATUS_BAD_REQUEST;
        else
        {
            dbg_err_if(request_del_upload(rq, up.v[i]));
            up.v[i] = NULL;
        }
    }

    U_FREE(up.v);
    return rc;
err:
    U_FREE(up.v);
    return HTTP_STATUS_INTERNAL_SERVER_ERROR;
}

/**
 * \brief   Validate a request's parameters and uploaded files
 *
 * If a params block of the vhost matches \p path (the path the client
 * asked for) and the method, parse the arguments and the form body and
 * check them against its rules.
 *
 * \return \c 0 to serve the page, or the status to answer with (400, 413
 *         for a file larger than its rule allows, or what parsing the body
 *         failed with)
 */
int validate_request(http_t *h, request_t *rq, const char *path)
{
    const valid_params_t *pb;
    valid_arg_t *args = NULL;
    vhost_t *vhost;
    size_t i, nargs = 0;
    int rc;

    dbg_return_if (h == NULL || rq == NULL || path == NULL,
            HTTP_STATUS_INTERNAL_SERVER_ERROR);

    if((vhost = http_get_vhost(h, rq)) == NULL || vhost->nparams == 0)
        return 0;
    if((pb = valid_find_params(vhost, path, request_get_method_name(rq)))
            == NULL)
        return 0;

    if((rc = request_parse_data(rq)) != 0)
        return rc;

    dbg_err_if(valid_args(rq, &args, &nargs));

    for(i = 0; i < pb->nrules; ++i)
        if((rc = valid_rule(rq, &pb->rules[i], args, nargs, pb->drop)) != 0)
            goto end;

    if((rc = valid_uploads(rq, pb)) != 0)
        goto end;

    /* the arguments no rule names: dropped (default), refused or kept */
    for(i = 0; pb->unknown != UNKNOWN_ALLOW && i < nargs; ++i)
    {
        if(args[i].var == NULL || valid_named(rq, pb, &args[i]))
            continue;
        if(pb->unknown == UNKNOWN_REJECT)
        {
            rc = HTTP_STATUS_BAD_REQUEST;
            goto end;
        }
        dbg_err_if(request_del_arg(rq, args[i].var));
        args[i].var = NULL;
    }

end:
    U_FREE(args);
    return rc;
err:
    U_FREE(args);
    return HTTP_STATUS_INTERNAL_SERVER_ERROR;
}

/* load a "param" block into 'r' */
static int valid_load_rule(vhost_t *vhost, u_config_t *pr, valid_rule_t *r)
{
    const char *s;

    r->name = u_config_get_subkey_value(pr, "name");
    warn_err_ifm(r->name == NULL, "params: a param has no name");
    r->src = valid_source(u_config_get_subkey_value(pr, "source"));
    warn_err_ifm(!r->src, "params: %s: source is query, form, route, header "
            "or any", r->name);
    s = u_config_get_subkey_value(pr, "validator");
    warn_err_ifm(s && (r->v = valid_find(vhost, s)) == NULL,
            "params: %s: no validator called %s", r->name, s);
    r->required = valid_yes(pr, "required", 0);

    return 0;
err:
    return ~0;
}

/* is each entry of a type list "type/subtype" or "type/ *", the type and
   the subtype each made of token characters (as valid_type_ok() wants)? */
static int valid_types_ok(const char *types)
{
    const char *p, *e;
    size_t k, t;

    for(p = types; *p; p = e)
    {
        p += strspn(p, " \t,");
        e = p + strcspn(p, " \t,");
        if((k = (size_t) (e - p)) == 0)
            continue;
        if((t = valid_token(p, k)) == 0 || t + 1 >= k || p[t] != '/')
            return 0;
        if(!(k == t + 2 && p[t + 1] == '*') &&
                valid_token(p + t + 1, k - t - 1) != k - t - 1)
            return 0;
    }
    return 1;
}

/* load an "upload" block into 'u' */
static int valid_load_upload(const valid_params_t *pb, u_config_t *uc,
        valid_upload_t *u)
{
    u_config_t *c;
    const char *key, *s;
    int i;

    u->name = u_config_get_subkey_value(uc, "name");
    warn_err_ifm(u->name == NULL, "params: an upload has no name");
    warn_err_ifm(valid_upload_named(pb, u->name),
            "params: upload %s is named twice", u->name);
    for(i = 0; (c = u_config_get_child_n(uc, NULL, i)); ++i)
    {
        key = u_config_get_key(c);
        warn_err_ifm(strcmp(key, "name") && strcmp(key, "max_size") &&
                strcmp(key, "type") && strcmp(key, "max_count") &&
                strcmp(key, "required"), "params: upload %s: unknown key "
                "'%s' (name, max_size, type, max_count, required)",
                u->name, key);
    }

    /* a key given as a block has no value: refuse it rather than fall back
       to the default (no size limit, an optional upload) */
    u->max_size = -1;
    if((s = u_config_get_subkey_value(uc, "max_size")) != NULL ||
            u_config_get_child(uc, "max_size"))
        warn_err_ifm(s == NULL || valid_int(s, &u->max_size) ||
                u->max_size < 0, "params: upload %s: max_size is a size in "
                "bytes", u->name);
    u->max_count = 1;
    if((s = u_config_get_subkey_value(uc, "max_count")) != NULL ||
            u_config_get_child(uc, "max_count"))
        warn_err_ifm(s == NULL || valid_int(s, &u->max_count) ||
                u->max_count < 1, "params: upload %s: max_count is 1 or "
                "more", u->name);
    if((s = u_config_get_subkey_value(uc, "type")) != NULL ||
            u_config_get_child(uc, "type"))
        warn_err_ifm(s == NULL || !valid_types_ok(s) ||
                s[strspn(s, " \t,")] == '\0', "params: upload %s: type is a "
                "list of type/subtype or type/*", u->name);
    u->types = s;
    u->required = 0;
    if(u_config_get_child(uc, "required"))
        warn_err_ifm(u_config_get_subkey_value(uc, "required") == NULL ||
                u_config_get_subkey_value_b(uc, "required", 0, &u->required),
                "params: upload %s: required is yes or no", u->name);

    return 0;
err:
    return ~0;
}

/* load a "params" block into 'pb' (a bad rule stops the server) */
static int valid_load_params(vhost_t *vhost, u_config_t *c,
        valid_params_t *pb)
{
    u_config_t *pc;
    const char *s;
    int i, n;

    for(n = 0; u_config_get_child_n(c, "path", n); ++n)
        ;
    warn_err_ifm(n == 0, "params: a block has no path");
    dbg_err_if((pb->paths = u_zalloc(n * sizeof *pb->paths)) == NULL);
    for(i = 0; (pc = u_config_get_child_n(c, "path", i)); ++i)
        if((s = u_config_get_value(pc)) != NULL)
            pb->paths[pb->npaths++] = s;    /* a path without a value never
                                               matches */
    pb->methods = u_config_get_subkey_value(c, "method");

    s = u_config_get_subkey_value(c, "unknown");
    warn_err_ifm(s && strcasecmp(s, "drop") && strcasecmp(s, "reject") &&
            strcasecmp(s, "allow"), "params: unknown is drop, reject or "
            "allow");
    pb->unknown = (s == NULL || !strcasecmp(s, "drop")) ? UNKNOWN_DROP :
        !strcasecmp(s, "reject") ? UNKNOWN_REJECT : UNKNOWN_ALLOW;
    s = u_config_get_subkey_value(c, "on_invalid");
    warn_err_ifm(s && strcasecmp(s, "drop") && strcasecmp(s, "reject"),
            "params: on_invalid is drop or reject");
    pb->drop = (s && !strcasecmp(s, "drop"));

    /* every rule names a parameter, a known source and a defined
       validator */
    for(n = 0; u_config_get_child_n(c, "param", n); ++n)
        ;
    if(n)
        dbg_err_if((pb->rules = u_zalloc(n * sizeof *pb->rules)) == NULL);
    for(i = 0; (pc = u_config_get_child_n(c, "param", i)); ++i)
    {
        dbg_err_if(valid_load_rule(vhost, pc, &pb->rules[pb->nrules]));
        pb->nrules++;
    }

    for(n = 0; u_config_get_child_n(c, "upload", n); ++n)
        ;
    if(n)
        dbg_err_if((pb->uploads = u_zalloc(n * sizeof *pb->uploads)) == NULL);
    for(i = 0; (pc = u_config_get_child_n(c, "upload", i)); ++i)
    {
        dbg_err_if(valid_load_upload(pb, pc, &pb->uploads[pb->nuploads]));
        pb->nuploads++;
    }

    return 0;
err:
    return ~0;
}

/**
 * \brief   Load a vhost's validators and params blocks (a bad one stops
 *          the server); validate_free() frees what it makes
 */
int validate_load(vhost_t *vhost)
{
    u_config_t *vs, *vc, *pb;
    validator_t *v;
    const char *s, *name;
    char *pat = NULL;
    long long a, b;
    char ka[32], kb[32];
    int i, n;
    size_t len;

    dbg_return_if (vhost == NULL, ~0);

    if((vs = u_config_get_child(vhost->config, "validators")) != NULL)
    {
        for(n = 0; u_config_get_child_n(vs, "validator", n); ++n)
            ;
        if(n)
            dbg_err_if((vhost->validators = u_zalloc(n * sizeof *v)) == NULL);

        for(i = 0; (vc = u_config_get_child_n(vs, "validator", i)); ++i)
        {
            v = &vhost->validators[vhost->nvalidators];
            name = u_config_get_subkey_value(vc, "name");
            warn_err_ifm(name == NULL, "validators: a validator has no name");
            warn_err_ifm(valid_find(vhost, name) != NULL,
                    "validators: %s is defined twice", name);
            v->name = name;

            if((s = u_config_get_subkey_value(vc, "regex")) != NULL)
            {
                len = strlen(s) + 5;
                dbg_err_if((pat = u_malloc(len)) == NULL);
                (void) u_snprintf(pat, len, "^(%s)$", s);
                warn_err_ifm(regcomp(&v->re, pat, REG_EXTENDED | REG_NOSUB),
                        "validators: %s: bad regular expression", name);
                v->type = VALID_REGEX;  /* compiled: validate_free frees it */
                U_FREE(pat);
            } else if((s = u_config_get_subkey_value(vc, "int")) != NULL ||
                    (s = u_config_get_subkey_value(vc, "length")) != NULL) {
                v->type = u_config_get_subkey_value(vc, "int") ? VALID_INT :
                    VALID_LENGTH;
                warn_err_ifm(sscanf(s, "%31s %31s", ka, kb) != 2 ||
                        valid_int(ka, &a) || valid_int(kb, &b) || a > b ||
                        (v->type == VALID_LENGTH && a < 0),
                        "validators: %s: give a minimum and a maximum", name);
                v->min = a;
                v->max = b;
            } else if((s = u_config_get_subkey_value(vc, "enum")) != NULL) {
                v->type = VALID_ENUM;
                v->values = s;
            } else
                warn_err("validators: %s needs regex, int, length or enum",
                        name);
            vhost->nvalidators++;
        }
    }

    for(n = 0; u_config_get_child_n(vhost->config, "params", n); ++n)
        ;
    if(n)
        dbg_err_if((vhost->params = u_zalloc(n * sizeof *vhost->params))
                == NULL);
    for(i = 0; (pb = u_config_get_child_n(vhost->config, "params", i)); ++i)
    {
        /* counted first, so validate_free frees what a bad block made */
        vhost->nparams++;
        dbg_err_if(valid_load_params(vhost, pb,
                    &vhost->params[vhost->nparams - 1]));
    }

    return 0;
err:
    U_FREE(pat);
    return ~0;
}

/* free what validate_load() made */
void validate_free(vhost_t *vhost)
{
    size_t i;

    if(vhost == NULL)
        return;
    for(i = 0; i < vhost->nvalidators; ++i)
        if(vhost->validators[i].type == VALID_REGEX)
            regfree(&vhost->validators[i].re);
    U_FREE(vhost->validators);
    vhost->nvalidators = 0;
    for(i = 0; i < vhost->nparams; ++i)
    {
        U_FREE(vhost->params[i].paths);
        U_FREE(vhost->params[i].rules);
        U_FREE(vhost->params[i].uploads);
    }
    U_FREE(vhost->params);
    vhost->nparams = 0;
}
