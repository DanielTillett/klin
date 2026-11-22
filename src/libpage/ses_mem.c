/*
 * Copyright (c) 2005-2012 by KoanLogic s.r.l. <http://www.koanlogic.com>
 * Copyright (c) 2026 Daniel Tillett
 * All rights reserved.
 *
 * This file is part of Klin (derived from KLone), and as such it is subject to the license stated
 * in the LICENSE file which you have received as part of this distribution.
 */

#include "klin_conf.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <u/libu.h>
#include <klin/session.h>
#include <klin/request.h>
#include <klin/context.h>
#include <klin/response.h>
#include <klin/vars.h>
#include <klin/utils.h>
#include <klin/atom.h>
#include <klin/ses_prv.h>
#include <klin/ppc.h>
#include <klin/ppc_cmd.h>

enum { SESSION_FILENAME_MAX_LENGTH = 256 };

struct enc_ses_mem_s
{
    time_t mtime;                                   /* modification time    */
    char filename[SESSION_FILENAME_MAX_LENGTH];     /* session filename     */
};

typedef struct enc_ses_mem_s enc_ses_mem_t;

static int so_atom_delete_oldest(session_opt_t *so)
{
    atom_t *atom, *oldest = NULL;

    dbg_err_if (so == NULL);

    LIST_FOREACH(atom, &so->atoms->list, np)
    {
        if(oldest == NULL || (time_t)atom->arg <= (time_t)oldest->arg)
            oldest = atom;
    }

    nop_err_if(oldest == NULL);
    dbg_err_if(atoms_remove(so->atoms, oldest));
    atom_free(oldest);

    return 0;
err:
    return ~0;
}

static int so_atom_remove(session_opt_t *so, const char *id)
{
    atom_t *atom = NULL;

    dbg_err_if (so == NULL);
    dbg_err_if (id == NULL);

    /* find the atom bound to this session */
    if(atoms_get(so->atoms, id, &atom))
        return 0;

    /* remove it from the list */
    dbg_err_if(atoms_remove(so->atoms, atom));

    atom_free(atom);

    return 0;
err:
    return ~0;
}


/* Sweep on writes, at most once a minute. Accessed sessions still go
   through the regular idle and absolute timeout checks in session_create. */
static void so_atom_expire(session_opt_t *so, time_t now)
{
    enum { SWEEP_INTERVAL = 60 };
    atom_t *atom, *next;

    if(so->max_age <= 0 || now == (time_t)-1 ||
            (now >= so->last_mem_sweep &&
             difftime(now, so->last_mem_sweep) < SWEEP_INTERVAL))
        return;

    so->last_mem_sweep = now;
    for(atom = LIST_FIRST(&so->atoms->list); atom; atom = next)
    {
        next = LIST_NEXT(atom, np);
        if(difftime(now, (time_t)atom->arg) > so->max_age &&
                atoms_remove(so->atoms, atom) == 0)
            atom_free(atom);
    }
}

static int session_uses_parent_store(void)
{
    return ctx->pipc && ctx->backend &&
        ctx->backend->model == SERVER_MODEL_PREFORK;
}

static int so_atom_add(session_opt_t *so, const char *id, char *buf, 
    size_t size, void *arg)
{
    atom_t *atom = NULL;

    dbg_err_if (so == NULL);
    dbg_err_if (id == NULL);
    dbg_err_if (buf == NULL);

    warn_err_ifm(so->mem_limit && size > so->mem_limit,
        "session size exceeds memory.limit, save aborted");
    dbg_err_if(atom_create(id, buf, size, arg, &atom));

    so_atom_expire(so, time(NULL));

    /* A replacement does not count toward either limit twice. Remove it
       before choosing eviction victims, after allocating its replacement. */
    dbg_err_if(so_atom_remove(so, id));

    while((so->max_count && atoms_count(so->atoms) >= so->max_count) ||
            (so->mem_limit && atoms_size(so->atoms) > so->mem_limit - size))
        dbg_err_if(so_atom_delete_oldest(so));

    dbg_err_if(atoms_add(so->atoms, atom));

    return 0;
err:
    if(atom)
        atom_free(atom);
    return ~0;
}


/* [parent] remove a session */
static int session_cmd_remove(ppc_t *ppc, int fd, unsigned char cmd,
    char *data, size_t size, void *vso)
{
    u_unused_args(ppc, fd, cmd);

    dbg_return_if(vso == NULL || data == NULL || size == 0 ||
        size > SESSION_FILENAME_MAX_LENGTH || data[size - 1] != 0, ~0);
    return so_atom_remove(vso, data);
}

/* [parent] save a session */
static int session_cmd_save(ppc_t *ppc, int fd, unsigned char cmd, char *data,
    size_t size, void *vso)
{
    enc_ses_mem_t esm;

    u_unused_args(ppc, fd, cmd);

    dbg_return_if(vso == NULL || data == NULL || size < sizeof esm, ~0);
    memcpy(&esm, data, sizeof esm);
    dbg_return_if(memchr(esm.filename, 0, sizeof esm.filename) == NULL, ~0);

    return so_atom_add(vso, esm.filename, data + sizeof esm, size - sizeof esm,
        (void*)esm.mtime);
}

/* Session messages contain a fixed header followed by the serialized data.
   PPC supplies the payload length and caps it at PPC_MAX_DATA_SIZE. */
static int session_ppc_write(ppc_t *ppc, int fd, unsigned char cmd,
    const char *filename, const char *data, size_t size, time_t mtime)
{
    enc_ses_mem_t esm;
    char msg[PPC_MAX_DATA_SIZE];

    warn_err_ifm(size > sizeof msg - sizeof esm,
        "serialized memory session exceeds PPC limit of %zu bytes",
        sizeof msg - sizeof esm);
    dbg_err_if(strlen(filename) >= sizeof esm.filename);

    memset(&esm, 0, sizeof esm);
    esm.mtime = mtime;
    u_strlcpy(esm.filename, filename, sizeof esm.filename);
    memcpy(msg, &esm, sizeof esm);
    memcpy(msg + sizeof esm, data, size);

    return ppc_write(ppc, fd, cmd, msg, sizeof esm + size) < 0 ? ~0 : 0;
err:
    return ~0;
}

/* [parent] get session data */
static int session_cmd_get(ppc_t *ppc, int fd, unsigned char cmd, char *data,
    size_t size, void *vso)
{
    session_opt_t *so = vso;
    atom_t *atom;

    u_unused_args(cmd);

    dbg_err_if(ppc == NULL || so == NULL || data == NULL || size == 0 ||
        size > SESSION_FILENAME_MAX_LENGTH || data[size - 1] != 0);
    nop_err_if(atoms_get(so->atoms, data, &atom));
    warn_err_ifm(atom->size > PPC_MAX_DATA_SIZE - sizeof(enc_ses_mem_t),
        "serialized memory session exceeds PPC limit of %zu bytes",
        PPC_MAX_DATA_SIZE - sizeof(enc_ses_mem_t));

    return session_ppc_write(ppc, fd, PPC_CMD_RESPONSE_OK, data, atom->data,
        atom->size, (time_t)atom->arg);
err:
    if(ppc)
        ppc_write(ppc, fd, PPC_CMD_RESPONSE_ERROR, "", 1);
    return ~0;
}

static int session_mem_add(session_opt_t *so, const char *filename, char *buf,
    size_t size, time_t mtime)
{
    ppc_t *ppc;

    dbg_err_if(so == NULL || filename == NULL || buf == NULL);

    if(ctx->pipc)
    {
        warn_err_ifm(so->mem_limit && size > so->mem_limit,
            "session size exceeds memory.limit, save aborted");
        ppc = server_get_ppc(ctx->server);
        dbg_err_if(ppc == NULL);
        dbg_err_if(session_ppc_write(ppc, ctx->pipc, PPC_CMD_MSES_SAVE,
            filename, buf, size, mtime));

        if(session_uses_parent_store())
            return 0;
    }

    /* Fork workers also read their own writes from the local store. */
    return so_atom_add(so, filename, buf, size, (void*)mtime);
err:
    return ~0;
}

static int session_mem_save(session_t *ss)
{
    char *buf = NULL;
    size_t sz;

    dbg_err_if (ss == NULL);
    
    /* save the session data to freshly alloc'd buf of size sz */
    dbg_err_if(session_prv_save_to_buf(ss, &buf, &sz));

    /* add the session to the in-memory session list */
    dbg_err_if(session_mem_add(ss->so, ss->filename, buf, sz, time(0)));

    U_FREE(buf);

    return 0;
err:
    U_FREE(buf);
    return ~0;
}

static int session_mem_load(session_t *ss)
{
    atom_t *atom;
    char buf[PPC_MAX_DATA_SIZE];
    ssize_t size;
    enc_ses_mem_t esm;
    ppc_t *ppc;
    unsigned char cmd;

    dbg_err_if(ss == NULL);
    nop_err_if(ss->filename[0] == 0);

    if(session_uses_parent_store())
    {
        ppc = server_get_ppc(ctx->server);
        dbg_err_if(ppc == NULL);
        dbg_err_if(ppc_write(ppc, ctx->pipc, PPC_CMD_MSES_GET, ss->filename,
            strlen(ss->filename) + 1) < 0);

        size = ppc_read(ppc, ctx->pipc, &cmd, buf, sizeof buf);
        dbg_err_if(size <= 0);
        nop_err_if(cmd != PPC_CMD_RESPONSE_OK);
        dbg_err_if((size_t)size < sizeof esm);

        memcpy(&esm, buf, sizeof esm);
        ss->mtime = esm.mtime;
        return session_prv_load_from_buf(ss, buf + sizeof esm, size - sizeof esm);
    }

    nop_err_if(atoms_get(ss->so->atoms, ss->filename, &atom));
    ss->mtime = (time_t)atom->arg;
    return session_prv_load_from_buf(ss, atom->data, atom->size);
err:
    return ~0;
}

static int session_mem_remove(session_t *ss)
{
    ppc_t *ppc;

    dbg_err_if(ss == NULL);

    if(ctx->pipc)
    {
        ppc = server_get_ppc(ctx->server);
        dbg_err_if(ppc == NULL);
        dbg_err_if(ppc_write(ppc, ctx->pipc, PPC_CMD_MSES_REMOVE, ss->filename,
            strlen(ss->filename) + 1) < 0);

        if(session_uses_parent_store())
            return 0;
    }

    return so_atom_remove(ss->so, ss->filename);
err:
    return ~0;
}

static int session_mem_term(session_t *ss)
{
    /* nothing to do */
    u_unused_args(ss);
    return 0;
}

int session_mem_create(session_opt_t *so, request_t *rq, response_t *rs, 
        session_t **pss)
{
    session_t *ss = NULL;

    dbg_err_if (so == NULL);
    dbg_err_if (rq == NULL);
    dbg_err_if (rs == NULL);
    dbg_err_if (pss == NULL);

    ss = u_zalloc(sizeof(session_t));
    dbg_err_if(ss == NULL);

    ss->load = session_mem_load;
    ss->save = session_mem_save;
    ss->remove = session_mem_remove;
    ss->term = session_mem_term;
    ss->mtime = time(0);
    ss->so = so;

    dbg_err_if(session_prv_init(ss, rq, rs));

    *pss = ss;

    return 0;
err:
    if(ss)
        session_free(ss);
    return ~0;
}

/* this function will be called once by the server during startup */
int session_mem_module_init(u_config_t *config, session_opt_t *so)
{
    ppc_t *ppc;
    u_config_t *c;
    const char *v;

    /* config may be NULL */
    dbg_err_if (so == NULL);

    /* defaults */
    so->max_count = 0;  /* no limits */
    so->mem_limit = 0;  /* no limits */

    if(config && u_config_get_subkey(config, "memory", &c) == 0)
    {
        if((v = u_config_get_subkey_value(c, "max_count")) != NULL)
            so->max_count = atoi(v);

        if((v = u_config_get_subkey_value(c, "limit")) != NULL)
            so->mem_limit = atoi(v);
    }

    /* setup ppc parent <-> child channel */
    ppc = server_get_ppc(ctx->server);
    dbg_err_if(ppc == NULL);

    /* create an atom list to store in-memory session data */
    dbg_err_if(atoms_create(&so->atoms));

    /* register PPC commands callbacks */
    dbg_err_if(ppc_register(ppc, PPC_CMD_MSES_SAVE, session_cmd_save, so));
    dbg_err_if(ppc_register(ppc, PPC_CMD_MSES_GET, session_cmd_get, so));
    dbg_err_if(ppc_register(ppc, PPC_CMD_MSES_REMOVE, session_cmd_remove, so));

    return 0;
err:
    return ~0;
}

