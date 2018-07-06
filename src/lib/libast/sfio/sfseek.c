/***********************************************************************
 *                                                                      *
 *               This software is part of the ast package               *
 *          Copyright (c) 1985-2012 AT&T Intellectual Property          *
 *                      and is licensed under the                       *
 *                 Eclipse Public License, Version 1.0                  *
 *                    by AT&T Intellectual Property                     *
 *                                                                      *
 *                A copy of the License is available at                 *
 *          http://www.eclipse.org/org/documents/epl-v10.html           *
 *         (with md5 checksum b35adb5213ca9657e911e9befb180842)         *
 *                                                                      *
 *              Information and Software Systems Research               *
 *                            AT&T Research                             *
 *                           Florham Park NJ                            *
 *                                                                      *
 *               Glenn Fowler <glenn.s.fowler@gmail.com>                *
 *                    David Korn <dgkorn@gmail.com>                     *
 *                     Phong Vo <phongvo@gmail.com>                     *
 *                                                                      *
 ***********************************************************************/
#include "config_ast.h"  // IWYU pragma: keep

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "sfhdr.h"

/*	Set the IO pointer to a specific location in the stream
**
**	Written by Kiem-Phong Vo.
*/

static void newpos(Sfio_t *f, Sfoff_t p) {
    if ((f->bits & SF_MMAP) && f->data) {
        SFMUNMAP(f, f->data, f->endb - f->data);
        f->data = NULL;
    }
    f->next = f->endr = f->endw = f->data;
    f->endb = (f->mode & SF_WRITE) ? f->data + f->size : f->data;
    if ((f->here = p) < 0) {
        f->extent = -1;
        f->here = 0;
    }
}

Sfoff_t sfseek(Sfio_t *f, Sfoff_t p, int type) {
    Sfoff_t r, s;
    int mode, local, hardseek, mustsync;
    SFMTXDECL(f);

    SFMTXENTER(f, (Sfoff_t)(-1));

    GETLOCAL(f, local);

    hardseek = (type | f->flags) & (SF_SHARE | SF_PUBLIC);

    if (hardseek && f->mode == (SF_READ | SF_SYNCED)) {
        newpos(f, f->here);
        f->mode = SF_READ;
    }

    /* set and initialize the stream to a definite mode */
    if ((int)SFMODE(f, local) != (mode = f->mode & SF_RDWR)) {
        int flags = f->flags;

        if (hardseek & SF_PUBLIC) /* seek ptr must follow file descriptor */
            f->flags |= SF_SHARE | SF_PUBLIC;
        mode = _sfmode(f, mode, local);
        if (hardseek & SF_PUBLIC) f->flags = flags;

        if (mode < 0) SFMTXRETURN(f, (Sfoff_t)(-1));
    }

    mustsync =
        (type & SF_SHARE) && !(type & SF_PUBLIC) && (f->mode & SF_READ) && !(f->flags & SF_STRING);

    /* Xopen-compliant */
    if ((type &= (SEEK_SET | SEEK_CUR | SEEK_END)) != SEEK_SET && type != SEEK_CUR &&
        type != SEEK_END) {
        errno = EINVAL;
        SFMTXRETURN(f, (Sfoff_t)(-1));
    }

    if (f->extent < 0) { /* let system call set errno */
        (void)SFSK(f, (Sfoff_t)0, SEEK_CUR, f->disc);
        SFMTXRETURN(f, (Sfoff_t)(-1));
    }

    /* throw away ungetc data */
    if (f->disc == _Sfudisc) (void)sfclose((*_Sfstack)(f, NULL));

    /* lock the stream for internal manipulations */
    SFLOCK(f, local);

    /* clear error and eof bits */
    f->flags &= ~(SF_EOF | SF_ERROR);

    while (f->flags & SF_STRING) {
        SFSTRSIZE(f);

        if (type == SEEK_CUR)
            r = p + (f->next - f->data);
        else if (type == SEEK_END)
            r = p + f->extent;
        else
            r = p;

        if (r >= 0 && r <= f->size) {
            p = r;
            f->next = f->data + p;
            f->here = p;
            if (p > f->extent) memset(f->data + f->extent, 0, p - f->extent);
            goto done;
        }

        /* check exception handler, note that this may pop stream */
        if (SFSK(f, r, SEEK_SET, f->disc) != r) {
            p = -1;
            goto done;
        } else if (!(f->flags & SF_STRING)) {
            p = r;
            goto done;
        }
    }

    if (f->mode & SF_WRITE) { /* see if we can avoid flushing buffer */
        if (!hardseek && type < SEEK_END && !(f->flags & SF_APPENDWR)) {
            s = f->here + (f->next - f->data);
            r = p + (type == SEEK_SET ? 0 : s);
            if (r == s) {
                p = r;
                goto done;
            }
        }

        if (f->next > f->data && SFSYNC(f) < 0) {
            p = -1;
            goto done;
        }
    }

    if (type == SEEK_END || (f->mode & SF_WRITE)) {
        if ((hardseek & SF_PUBLIC) || type == SEEK_END)
            p = SFSK(f, p, type, f->disc);
        else {
            r = p + (type == SEEK_CUR ? f->here : 0);
            p = (hardseek || r != f->here) ? SFSK(f, r, SEEK_SET, f->disc) : r;
        }
        if (p >= 0) newpos(f, p);

        goto done;
    }

    /* if get here, must be a read stream */
    s = f->here - (f->endb - f->next);
    r = p + (type == SEEK_CUR ? s : 0);
    if (r <= f->here && r >= (f->here - (f->endb - f->data))) {
        if ((hardseek || (type == SEEK_CUR && p == 0))) {
            if ((s = SFSK(f, (Sfoff_t)0, SEEK_CUR, f->disc)) == f->here ||
                (s >= 0 && !(hardseek & SF_PUBLIC) &&
                 (s = SFSK(f, f->here, SEEK_SET, f->disc)) == f->here))
                goto near_done;
            else if (s < 0) {
                p = -1;
                goto done;
            } else {
                newpos(f, s);
                hardseek = 0;
            }
        } else {
        near_done:
            f->next = f->endb - (f->here - r);
            p = r;
            goto done;
        }
    }

    /* desired position */
    if ((p += type == SEEK_CUR ? s : 0) < 0) goto done;

    if (f->bits & SF_MMAP) { /* if mmap is not great, stop mmaping if moving around too much */
        /* for mmap, f->here can be virtual except for hardseek */
        newpos(f, p);
        if (!hardseek) goto done;
    }

    if (f->endb > f->next) { /* reduce wastage in future buffer fillings */
        f->iosz = (f->next - f->data) + (f->endb - f->next) / 2;
        f->iosz = ((f->iosz + f->blksz - 1) / f->blksz) * f->blksz;
    }
    if (f->iosz >= f->size) f->iosz = 0;

    /* buffer is now considered empty */
    f->next = f->endr = f->endb = f->data;

    /* small backseeks often come in bunches, so seek back as far as possible */
    if (p < f->lpos && f->size > f->blksz && (p + f->blksz) > s) {
        if ((r = s - f->size) < 0) r = 0;
    }
    /* try to align buffer to block boundary to enhance I/O speed */
    else if (f->blksz > 0 && f->size >= 2 * f->blksz)
        r = p - (p % f->blksz);
    else {
        r = p;

        /* seeking around and wasting data, be conservative */
        if (f->iosz > 0 && (p > f->lpos || p < f->lpos - f->size)) f->bits |= SF_JUSTSEEK;
    }

    if ((hardseek || r != f->here) && (f->here = SFSK(f, r, SEEK_SET, f->disc)) != r) {
        if (r < p) /* now try to just get to p */
            f->here = SFSK(f, p, SEEK_SET, f->disc);
        if (f->here != p) p = -1;
        goto done;
    }

    if (r < p) /* read to cover p */
    {
        (void)SFRD(f, f->data, f->size, f->disc);
        if (p <= f->here && p >= (f->here - (f->endb - f->data)))
            f->next = f->endb - (size_t)(f->here - p);
        else /* recover from read failure by just seeking to p */
        {
            f->next = f->endb = f->data;
            if ((f->here = SFSK(f, p, SEEK_SET, f->disc)) != p) p = -1;
        }
    }

done:
    if (f->here < 0) /* hasn't been the best of time */
    {
        f->extent = -1;
        f->here = 0;
    }

    f->lpos = p;

    SFOPEN(f, local);

    if (mustsync) sfsync(f);
    SFMTXRETURN(f, p);
}
