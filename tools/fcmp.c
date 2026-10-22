/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * fcmp - compare two files, or two drawers with everything in them, byte for
 * byte (for the copy tests).
 *   fcmp <path1> <path2>   -> "same, <n> files, <n> bytes" (rc 0) or what differs (rc 5)
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#define BUF 65536

static UBYTE *ba, *bb;
static ULONG nfiles, nbytes, ndiff;

static void cmp_file(const char *a, const char *b)
{
    BPTR fa = Open((STRPTR)a, MODE_OLDFILE), fb = Open((STRPTR)b, MODE_OLDFILE);
    ULONG pos = 0;
    if (!fa || !fb) { printf("fcmp: can't open %s\n", fa ? b : a); ndiff++; goto out; }
    for (;;) {
        LONG na = Read(fa, ba, BUF), nb = Read(fb, bb, BUF), i;
        if (na < 0 || nb < 0) { printf("fcmp: read error in %s at %lu\n", na < 0 ? a : b, (unsigned long)pos); ndiff++; break; }
        for (i = 0; i < na && i < nb; i++)
            if (ba[i] != bb[i]) {
                printf("fcmp: %s differs at byte %lu\n", a, (unsigned long)(pos + i));
                ndiff++;
                goto out;
            }
        if (na != nb) { printf("fcmp: %s: lengths differ\n", a); ndiff++; break; }
        if (!na) break;
        pos += na;
    }
    nfiles++;
    nbytes += pos;
out:
    if (fa) Close(fa);
    if (fb) Close(fb);
}

static void cmp_path(const char *a, const char *b)
{
    BPTR l = Lock((STRPTR)a, ACCESS_READ);
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    if (!l || !fib || !Examine(l, fib)) { printf("fcmp: can't read %s\n", a); ndiff++; goto out; }
    if (fib->fib_DirEntryType < 0) {
        UnLock(l); l = 0;
        cmp_file(a, b);
        goto out;
    }
    while (ExNext(l, fib)) {
        char pa[512], pb[512];
        strcpy(pa, a); AddPart((STRPTR)pa, (STRPTR)fib->fib_FileName, sizeof(pa));
        strcpy(pb, b); AddPart((STRPTR)pb, (STRPTR)fib->fib_FileName, sizeof(pb));
        if (CheckSignal(SIGBREAKF_CTRL_C)) { printf("fcmp: break\n"); ndiff++; break; }
        cmp_path(pa, pb);
    }
out:
    if (l) UnLock(l);
    if (fib) FreeDosObject(DOS_FIB, fib);
}

int main(void)
{
    LONG args[2] = { 0 };
    struct RDArgs *rda = ReadArgs("PATH1/A,PATH2/A", args, NULL);
    if (!rda) { printf("fcmp <path1> <path2>\n"); return 20; }
    ba = AllocMem(BUF, MEMF_ANY);
    bb = AllocMem(BUF, MEMF_ANY);
    if (ba && bb) {
        cmp_path((const char *)args[0], (const char *)args[1]);
        if (ndiff)
            printf("fcmp: %lu differences (%lu files the same)\n", (unsigned long)ndiff, (unsigned long)nfiles);
        else
            printf("fcmp: same, %lu files, %lu bytes\n", (unsigned long)nfiles, (unsigned long)nbytes);
    }
    if (ba) FreeMem(ba, BUF);
    if (bb) FreeMem(bb, BUF);
    FreeArgs(rda);
    return ndiff ? 5 : 0;
}
