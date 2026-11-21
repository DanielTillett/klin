/*
 * Copyright (c) 2005-2012 by KoanLogic s.r.l. <http://www.koanlogic.com>
 * Copyright (c) 2026 Daniel Tillett
 * All rights reserved.
 *
 * This file is part of Klin (derived from KLone), and as such it is subject to the license stated
 * in the LICENSE file which you have received as part of this distribution.
 */

#include <klin/emb.h>
#include <klin/io.h>
#include <klin/codecs.h>
#include <u/libu.h>
#include <u/toolbox/hmap.h>

/* these are klin-site autogen functions */
void register_pages(void);
void unregister_pages(void);

static u_hmap_t *embmap = NULL;     /* hashmap of embedded resources */
static int init = 0;

struct list_cursor
{
    char **list;
    size_t idx;
};

static int listadd (const void *val, const void *arg);

int emb_init(void)
{
    u_hmap_opts_t *hopts = NULL;

    if(!init)
    {
        dbg_err_if (u_hmap_opts_new(&hopts));

        /* Start with LibU's smallest bucket array; it grows as needed. */
        dbg_err_if (u_hmap_opts_set_size(hopts, 13));

        /* no free function needed for static values */
        dbg_err_if (u_hmap_opts_set_val_freefunc(hopts, NULL));

        /* the hmap keeps its own copy of the options */
        dbg_err_if (u_hmap_easy_new(hopts, &embmap));
        u_hmap_opts_free(hopts);
        hopts = NULL;

        /* Mark initialized before the generated code calls emb_register. */
        init = 1;
        u_dbg("registering embedded resources");
        register_pages();
    }

    return 0;
 
err:
    if (hopts)
        u_hmap_opts_free(hopts);

    return ~0;
}

int emb_term(void)
{
    dbg_err_if(init == 0);

    unregister_pages();

    U_FREEF(embmap, u_hmap_easy_free);

    return 0;
err:
    return ~0;
}

int emb_register(embres_t *res)
{
    dbg_err_if(init == 0 || res == NULL);

    if(res->type == ET_FILE) 
        u_dbg("registering %s (%s)", res->filename, 
                ((embfile_t*)res)->comp ? "compressed" : "uncompressed");
    else 
        u_dbg("registering %s", res->filename);

    dbg_err_if (u_hmap_easy_put(embmap, res->filename, (const void *) res));
    
    return 0;
err:
    return ~0;
}

int emb_unregister(embres_t *res)
{
    dbg_err_if(init == 0 || res == NULL);

    if (embmap)
        dbg_err_if (u_hmap_easy_del(embmap, res->filename));

    return 0;
err:
    return ~0;
}

int emb_lookup(const char *filename, embres_t **pr)
{
    embres_t *res;

    dbg_err_if (init == 0);
    dbg_err_if (filename == NULL);
    dbg_err_if (filename[0] == '\0');
    dbg_err_if (pr == NULL);

    res = u_hmap_easy_get(embmap, filename);
    /* dbg_err_ifm (res == NULL, "%s not found", filename); */
    nop_err_if (res == NULL);

    *pr = res;

    return 0;
err:
    return ~0;
}

static int emb_open_sized(const char *file, io_t **pio, size_t *size)
{
    embres_t *res;
    const embfile_t *e;
    size_t read_size;
    codec_t *gzip = NULL;
    io_t *io = NULL;

    dbg_return_if (pio == NULL, ~0);
    dbg_return_if (file == NULL, ~0);
    
    dbg_err_if(emb_lookup(file, &res) || res->type != ET_FILE);
    e = (const embfile_t *)res;
    read_size = e->size;

    dbg_err_if(io_mem_create(KLIN_UNCONST(e->data), e->size, 0, &io));

#ifdef HAVE_LIBZ
    if(e->comp)
    {
        dbg_err_if(codec_gzip_create(e->comp == EMB_COMP_ZLIB ?
                    ZLIB_UNCOMPRESS : GZIP_UNCOMPRESS, &gzip));
        dbg_err_if(io_codec_add_tail(io, gzip));
        gzip = NULL;
        read_size = e->file_size;
    }
#endif

    *pio = io;
    if(size)
        *size = read_size;

    return 0;
err:
    if(gzip)
        codec_free(gzip);
    if(io)
        io_free(io);
    return ~0;
}

int emb_open(const char *file, io_t **pio)
{
    return emb_open_sized(file, pio, NULL);
}

int emb_list (char ***plist)
{
    ssize_t nelems;
    struct list_cursor cursor = { NULL, 0 };

    dbg_err_if (plist == NULL);
    /* See how many elements are registered. */
    dbg_err_if ((nelems = u_hmap_count(embmap)) <= 0);

    /* Zero the array for the terminator and cleanup of a partial list. */
    cursor.list = u_calloc((size_t) nelems + 1, sizeof(*cursor.list));
    dbg_err_sif (cursor.list == NULL);

    /* Walk through the embfs to collect res names. */
    dbg_err_if (u_hmap_foreach_arg(embmap, listadd, &cursor));

    /* Copy out result. */
    *plist = cursor.list;

    return 0;
err:
    emb_list_free(cursor.list);
    return ~0;
}

void emb_list_free (char **list)
{
    size_t i;

    if (list == NULL)
        return;

    for (i = 0; list[i] != NULL; i++)
        u_free(list[i]);

    u_free(list);

    return;
}

int emb_to_ubuf(const char *res_name, u_buf_t **pubuf)
{
    int c;
    size_t size;
    char buf[1024];
    io_t *tmp = NULL;
    u_buf_t *ubuf = NULL;

    dbg_err_if(res_name == NULL);
    dbg_err_if(pubuf == NULL);

    dbg_err_if(emb_open_sized(res_name, &tmp, &size));

    dbg_err_if(u_buf_create(&ubuf));
    dbg_err_if(size == SIZE_MAX || u_buf_reserve(ubuf, size));

    for (;;)
    {
        c = io_read(tmp, buf, sizeof(buf));

        if (c == 0)     /* EOF */
            break;
        else if (c < 0) /* read error */
            goto err;

        dbg_err_if (u_buf_append(ubuf, buf, c));
    }

    io_free(tmp); tmp = NULL;

    *pubuf = ubuf;

    return 0;
err:
    if (tmp) 
        io_free(tmp);
    if (ubuf)  
        u_buf_free(ubuf);

    return ~0;
}

static int listadd (const void *val, const void *arg)
{
    struct list_cursor *cursor = KLIN_UNCONST(arg);
    const embres_t *res = val;

    dbg_err_sif ((cursor->list[cursor->idx] = u_strdup(res->filename)) == NULL);
    cursor->idx++;

    return 0;
err:
    return ~0;
}

