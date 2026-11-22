/*
 * Copyright (c) 2005-2012 by KoanLogic s.r.l. <http://www.koanlogic.com>
 * Copyright (c) 2026 Daniel Tillett
 * All rights reserved.
 *
 * This file is part of Klin (derived from KLone), and as such it is subject to the license stated
 * in the LICENSE file which you have received as part of this distribution.
 */

#include "klin_conf.h"
#ifdef OS_WIN
#include <winsock2.h>
#include <limits.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#include <u/libu.h>
#include <klin/io.h>
#include <klin/ioprv.h>
#include <klin/timer.h>

struct io_fd_s
{
    struct io_s io; /* must be the first item */
    int fd;
    int flags;
    int issock;     /* >0 if fd is a SOCKET (win32 only) */
};

typedef struct io_fd_s io_fd_t;

static ssize_t io_fd_read(io_t *io_, char *buf, size_t size);
static ssize_t io_fd_write(io_t *io_, const char *buf, size_t size);
static ssize_t io_fd_seek(io_t *io_, size_t off);
static ssize_t io_fd_tell(io_t *io_);
static int io_fd_close(io_t *io_);
static int io_fd_free(io_t *io_);

static ssize_t io_fd_read(io_t *io_, char *buf, size_t size)
{
    io_fd_t *ifd = (io_fd_t *) io_;
    ssize_t c;

    dbg_return_if (ifd == NULL, ~0);
    dbg_return_if (buf == NULL, ~0);
    
again:
    /* while an alarm is set (a timeout), wait for data with the alarms
       running: one may close the descriptor */
    if(timerm_armed())
        nop_err_if(timerm_wait(&ifd->fd, TIMERM_WAIT_READ));

    if(!ifd->issock)
        c = read(ifd->fd, buf, size);
    else
        c = recv(ifd->fd, buf, size, 0);
    /* if(c < 0 && (errno == EINTR || errno == EAGAIN)) */
    if(c < 0 && errno == EINTR)
        goto again; 

    dbg_err_sif(c == -1); 

    return c;
err:
    return -1;
}

static ssize_t io_fd_write(io_t *io_, const char *buf, size_t size)
{
    io_fd_t *ifd = (io_fd_t *) io_;
    int64_t deadline;
    ssize_t c;

    dbg_return_if (ifd == NULL, ~0);
    dbg_return_if (buf == NULL, ~0);

    /* the deadline runs from the start of this write, not from each retry
       (a wait can report room that a write then finds gone) */
    deadline = timerm_deadline(io_->wr_timeout);

again:
    if((io_->wr_timeout > 0 || timerm_armed()) &&
            timerm_wait_until(&ifd->fd, TIMERM_WAIT_WRITE, deadline))
    {
        /* a stalled peer: later writes fail at once */
        if(errno == ETIMEDOUT && ifd->fd >= 0)
        {
            u_info("write timed out after %d s, closed", io_->wr_timeout);
            (void) io_fd_close(io_);
        }
        goto err;
    }

#ifdef OS_WIN
    if(ifd->issock)
    {
        u_long mode = 1;
        int timed = io_->wr_timeout > 0;
        int saved = 0;

        /* Plain connections use blocking reads; switch only for this send. */
        dbg_err_if(timed && ioctlsocket(ifd->fd, FIONBIO, &mode));
        c = send(ifd->fd, buf, (int) U_MIN(size, (size_t) INT_MAX), 0);
        if(c == SOCKET_ERROR)
            saved = WSAGetLastError();
        mode = 0;
        if(timed && ioctlsocket(ifd->fd, FIONBIO, &mode))
        {
            (void) io_fd_close(io_);
            goto err;
        }
        if(saved == WSAEINTR || (timed && saved == WSAEWOULDBLOCK))
            goto again;
        dbg_err_if(c == SOCKET_ERROR);
        return c;
    }
#else
    /* with a deadline the write itself must not block: the wait above
       says only that some room is free (on macOS as little as 2048 bytes,
       SO_SNDLOWAT), and a blocking write waits for room for all of 'buf'.
       O_NONBLOCK for this write only (macOS ignores MSG_DONTWAIT on send,
       and the reads on this descriptor expect it blocking) */
    if(io_->wr_timeout > 0)
    {
        int fl = fcntl(ifd->fd, F_GETFL), saved;

        dbg_err_sif(fl < 0);
        if(!(fl & O_NONBLOCK))
            dbg_err_sif(fcntl(ifd->fd, F_SETFL, fl | O_NONBLOCK) < 0);
        c = write(ifd->fd, buf, size);
        saved = errno;
        if(!(fl & O_NONBLOCK) && fcntl(ifd->fd, F_SETFL, fl) < 0)
        {
            (void) io_fd_close(io_);
            goto err;
        }
        errno = saved;
        if(c < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            goto again;
    } else
#endif
        c = write(ifd->fd, buf, size);
    if(c < 0 && errno == EINTR)
        goto again;

    dbg_err_sif(c == -1); 

    return c;
err:
    return -1;
}

static ssize_t io_fd_seek(io_t *io_, size_t off)
{
    io_fd_t *ifd = (io_fd_t *) io_;
    dbg_return_if (ifd == NULL, -1);

    return lseek(ifd->fd, off, SEEK_SET);
}

static ssize_t io_fd_tell(io_t *io_)
{
    io_fd_t *ifd = (io_fd_t *) io_;
    dbg_return_if (ifd == NULL, -1);

    return lseek(ifd->fd, 0, SEEK_CUR);
}

/* close the underlaying fd (may be called more then once) */
static int io_fd_close(io_t *io_)
{
    io_fd_t *ifd = (io_fd_t *) io_;
    dbg_return_if (ifd == NULL, ~0);
    
    if(ifd->flags & IO_FD_CLOSE && ifd->fd != -1)
    {
#ifdef OS_WIN
        if(ifd->issock)
        {
            closesocket(ifd->fd);
        } else
            close(ifd->fd);
#else
            close(ifd->fd);
#endif
        ifd->fd = -1;
    }

    return 0;
}

static int io_fd_free(io_t *io_)
{
    io_fd_t *ifd = (io_fd_t *) io_;
    dbg_if(io_fd_close(io_));

    return 0;
}

int io_fd_get_d(io_t *io)
{
    io_fd_t *ifd = (io_fd_t*)io;
    dbg_err_if(io == NULL);
    dbg_err_if(ifd->io.type != IO_TYPE_FD);

    return ifd->fd;
err:
    return -1;
}

int io_fd_create(int fd, int flags, io_t **pio)
{
    io_fd_t *ifd = NULL;
    io_t *io = NULL;

    dbg_err_if (pio == NULL);

    /* create through an io_t pointer: storing one in 'ifd' via a cast
       (io_t**)&ifd breaks strict aliasing (LTO then miscompiled it) */
    dbg_err_if(io_create(io_fd_t, &io));
    ifd = (io_fd_t *) io;

    ifd->io.type = IO_TYPE_FD;

    ifd->fd = fd;
    ifd->flags = flags;
    ifd->io.read = io_fd_read;
    ifd->io.write = io_fd_write;
    ifd->io.seek = io_fd_seek;
    ifd->io.tell = io_fd_tell;
    ifd->io.close = io_fd_close; 
    ifd->io.free = io_fd_free; 
    
#ifdef OS_WIN
    u_long ret;
    if(ioctlsocket(fd, FIONREAD, &ret) == 0)
        ifd->issock++; /* is a socket, use recv/send instead of read/write  */
    _setmode(fd, _O_BINARY); /* we never want Windows CR/LF translation */
#endif

    *pio = (io_t*)ifd;

    return 0;
err:
    if(ifd)
        io_free((io_t*)ifd);
    return ~0;
}
