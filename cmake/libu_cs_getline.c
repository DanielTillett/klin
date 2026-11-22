static int cs_getline (u_config_gets_t cb, void *arg, u_string_t *ln)
{
    /* Klin's replacement (see cmake/LibU.cmake): LibU's lost a last line with
       no newline at the end of the input, and the last character of every
       1023-byte chunk of a longer line */
    enum { BUFSZ = 1024 };
    char buf[BUFSZ];
    size_t len;
    int got = 0;

    u_string_clear(ln);

    while(cb(arg, buf, BUFSZ) != NULL)
    {
        got = 1;
        if((len = strlen(buf)) == 0)
            break; /* empty line */
        if(u_isnl(buf[len - 1]))
        {
            dbg_err_if(u_string_append(ln, buf, len - 1));
            return 0;
        }
        /* a chunk of a longer line, or the last line without a newline */
        dbg_err_if(u_string_append(ln, buf, len));
    }

    nop_err_if(!got); /* eof */

    return 0;
err:
    return ~0;
}

