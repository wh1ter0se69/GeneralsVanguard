/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Copyright (C) Electronic Arts Canada Inc. 1995-2002.  All rights reserved.

#ifndef __REFREAD
#define __REFREAD 1

#include <string.h>
#include "codex.h"
#include "refcodex.h"

/****************************************************************/
/*  Information Functions                                       */
/****************************************************************/

/* check for reasonable header: */
/* 10fb header */

bool GCALL REF_is(const void *compresseddata)
{
    bool ok=false;
    int packtype=ggetm(compresseddata,2);

    if (packtype==0x10fb
     || packtype==0x11fb
     || packtype==0x90fb
     || packtype==0x91fb)
        ok = true;

    return(ok);
}


/****************************************************************/
/*  Decode Functions                                            */
/****************************************************************/

int GCALL REF_size(const void *compresseddata)
{
    int len=0;
    int packtype=ggetm(compresseddata,2);
    int ssize=(packtype&0x8000)?4:3;

    if (packtype&0x100)     /* 11fb */
    {
        len = ggetm((char *)compresseddata+2+ssize,ssize);
    }
    else                    /* 10fb */
    {
        len = ggetm((char *)compresseddata+2,ssize);
    }

    return(len);
}


/* GeneralsX @bugfix Bounded RefPack decode.
 *
 * REF_decode writes through *d++ until it meets an end opcode and reads through
 * *s++ with no view of either buffer's extent. The stream's own ulen field is
 * parsed but never used to limit the writes. CompressionManager::decompressData
 * accepted a destLen and then discarded it, so a crafted .map could expand far
 * past its destination — and map enumeration decompresses every .map on disk at
 * startup, including ones a peer transferred that the user never opened.
 *
 * This variant takes both extents and refuses rather than overrunning. Each
 * opcode's run lengths are known before its writes, so the check is exact. The
 * back-reference is also validated: "ref = d-1-offset" can point before the start
 * of the destination buffer on a malformed stream, which would read out of bounds
 * and copy that data into the output.
 *
 * Returns 0 on any violation, matching the existing failure contract.
 */
int GCALL REF_decode_bounded(void *dest, int destsize, const void *compresseddata, int compressedsize, int *compressedsizeout)
{
    unsigned char *s;
    unsigned char *ref;
    unsigned char *d;
    unsigned char *dstart;
    unsigned char *dlimit;
    const unsigned char *slimit;
    unsigned char first;
    unsigned char second;
    unsigned char third;
    unsigned char forth;
    unsigned int  run;
    unsigned int  type;
    int          ulen;

    if (!dest || !compresseddata || destsize <= 0 || compressedsize <= 0)
        return 0;

    s      = (unsigned char *) compresseddata;
    slimit = (const unsigned char *) compresseddata + compressedsize;
    d      = (unsigned char *) dest;
    dstart = d;
    dlimit = d + destsize;
    ulen   = 0L;

    /* every read of n bytes from the source, and write of n to the dest, is
     * gated on these before it happens */
    #define REF_NEED_SRC(n)  do { if (s + (n) > slimit)  return 0; } while (0)
    #define REF_NEED_DST(n)  do { if (d + (n) > dlimit)  return 0; } while (0)
    #define REF_COPY_LITERAL(n) do { unsigned int _n=(n); REF_NEED_SRC(_n); REF_NEED_DST(_n); \
                                     while (_n--) *d++ = *s++; } while (0)
    #define REF_COPY_REF(n)  do { unsigned int _n=(n); REF_NEED_DST(_n); \
                                  if (ref < dstart || ref + _n > dlimit) return 0; \
                                  while (_n--) *d++ = *ref++; } while (0)

    REF_NEED_SRC(2);
    type = *s++;
    type = (type<<8) + *s++;

    if (type&0x8000)                              /* 4 byte size field */
    {
        if (type&0x100) { REF_NEED_SRC(4); s += 4; }
        REF_NEED_SRC(4);
        ulen = *s++;
        ulen = (ulen<<8) + *s++;
        ulen = (ulen<<8) + *s++;
        ulen = (ulen<<8) + *s++;
    }
    else
    {
        if (type&0x100) { REF_NEED_SRC(3); s += 3; }
        REF_NEED_SRC(3);
        ulen = *s++;
        ulen = (ulen<<8) + *s++;
        ulen = (ulen<<8) + *s++;
    }

    /* The declared length must itself fit; a stream claiming more than the
     * caller allocated is rejected up front rather than part-way through. */
    if (ulen < 0 || ulen > destsize)
        return 0;

    for (;;)
    {
        REF_NEED_SRC(1);
        first = *s++;
        if (!(first&0x80))                        /* short form */
        {
            REF_NEED_SRC(1);
            second = *s++;
            REF_COPY_LITERAL(first&3);
            ref = d-1 - (((first&0x60)<<3) + second);
            REF_COPY_REF((((first&0x1c)>>2)+3-1) + 1);
            continue;
        }
        if (!(first&0x40))                        /* int form */
        {
            REF_NEED_SRC(2);
            second = *s++;
            third  = *s++;
            REF_COPY_LITERAL(second>>6);
            ref = d-1 - (((second&0x3f)<<8) + third);
            REF_COPY_REF(((first&0x3f)+4-1) + 1);
            continue;
        }
        if (!(first&0x20))                        /* very int form */
        {
            REF_NEED_SRC(3);
            second = *s++;
            third  = *s++;
            forth  = *s++;
            REF_COPY_LITERAL(first&3);
            ref = d-1 - (((first&0x10)>>4<<16) + (second<<8) + third);
            REF_COPY_REF((((first&0x0c)>>2<<8) + forth + 5-1) + 1);
            continue;
        }
        run = ((first&0x1f)<<2)+4;                /* literal */
        if (run<=112)
        {
            REF_COPY_LITERAL(run);
            continue;
        }
        REF_COPY_LITERAL(first&3);                /* eof (+0..3 literal) */
        break;
    }

    #undef REF_NEED_SRC
    #undef REF_NEED_DST
    #undef REF_COPY_LITERAL
    #undef REF_COPY_REF

    if (compressedsizeout)
        *compressedsizeout = (int)((char *)s-(char *)compresseddata);
    return(ulen);
}

int GCALL REF_decode(void *dest, const void *compresseddata, int *compressedsize)
{
    unsigned char *s;
    unsigned char *ref;
    unsigned char *d;
    unsigned char first;
    unsigned char second;
    unsigned char third;
    unsigned char forth;
    unsigned int  run;
    unsigned int  type;
    int          ulen;

    s = (unsigned char *) compresseddata;
    d = (unsigned char *) dest;
    ulen = 0L;

    if (s)
    {
        type = *s++;
        type = (type<<8) + *s++;

        if (type&0x8000) /* 4 byte size field */
        {
            if (type&0x100)                       /* skip ulen */
                s += 4;

            ulen = *s++;
            ulen = (ulen<<8) + *s++;
            ulen = (ulen<<8) + *s++;
            ulen = (ulen<<8) + *s++;
        }
        else
        {
            if (type&0x100)                       /* skip ulen */
                s += 3;

            ulen = *s++;
            ulen = (ulen<<8) + *s++;
            ulen = (ulen<<8) + *s++;
        }

        for (;;)
        {
            first = *s++;
            if (!(first&0x80))          /* short form */
            {
                second = *s++;
                run = first&3;
                while (run--)
                    *d++ = *s++;
                ref = d-1 - (((first&0x60)<<3) + second);
                run = ((first&0x1c)>>2)+3-1;
                do
                {
                    *d++ = *ref++;
                } while (run--);
                continue;
            }
            if (!(first&0x40))          /* int form */
            {
                second = *s++;
                third = *s++;
                run = second>>6;
                while (run--)
                    *d++ = *s++;

                ref = d-1 - (((second&0x3f)<<8) + third);

                run = (first&0x3f)+4-1;
                do
                {
                    *d++ = *ref++;
                } while (run--);
                continue;
            }
            if (!(first&0x20))          /* very int form */
            {
                second = *s++;
                third = *s++;
                forth = *s++;
                run = first&3;
                while (run--)
                    *d++ = *s++;

                ref = d-1 - (((first&0x10)>>4<<16) +  (second<<8) + third);

                run = ((first&0x0c)>>2<<8) + forth + 5-1;
                do
                {
                    *d++ = *ref++;
                } while (run--);
                continue;
            }
            run = ((first&0x1f)<<2)+4;  /* literal */
            if (run<=112)
            {
                while (run--)
                    *d++ = *s++;
                continue;
            }
            run = first&3;              /* eof (+0..3 literal) */
            while (run--)
                *d++ = *s++;
            break;
        }
    }
    if (compressedsize)
        *compressedsize = (int)((char *)s-(char *)compresseddata);
    return(ulen);
}

#endif

