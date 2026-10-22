/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * rdbfix - show, back up and repair the partition blocks of a drive's RDB.
 *
 *   rdbfix DEVICE=<name> UNIT=<n> [SAVE=<file>] [FIX] [RESTORE=<file>]
 *
 * Without FIX it only lists each partition's MaxTransfer and Mask (return
 * code 5 when one looks swapped). SAVE writes
 * the RDB blocks (block 0 up to rdb_RDBBlocksHi) to a file first. FIX repairs a
 * partition whose MaxTransfer and Mask were written the wrong way round (a Mask
 * below $00FFFFFE cannot be a memory mask): it swaps them back, fixes the
 * block's checksum, writes it and reads it back to check. RESTORE writes a
 * SAVE file back to the drive as it was.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#define RDSK 0x5244534BUL
#define PART 0x50415254UL
#define ENV  32                          /* pb_Environment, in longs */

static struct IOStdReq *io;

static int blk(UWORD cmd, ULONG *buf, ULONG b, ULONG n)
{
    io->io_Command = cmd;
    io->io_Data = buf;
    io->io_Length = n * 512;
    io->io_Offset = b * 512;
    return DoIO((struct IORequest *)io);
}

static void sum_fix(ULONG *p)
{
    ULONG n = p[1], i, s = 0;
    if (n < 3 || n > 128) n = 128;
    p[2] = 0;
    for (i = 0; i < n; i++) s += p[i];
    p[2] = (ULONG)-(LONG)s;
}

static int sum_ok(const ULONG *p)
{
    ULONG n = p[1], i, s = 0;
    if (n < 3 || n > 128) return 0;
    for (i = 0; i < n; i++) s += p[i];
    return s == 0;
}

int main(void)
{
    LONG args[5] = { 0 };
    struct RDArgs *rda = ReadArgs("DEVICE/A/K,UNIT/A/K/N,SAVE/K,FIX/S,RESTORE/K", args, NULL);
    struct MsgPort *port;
    ULONG *b = NULL, rdsk = 0xffffffff, part, hi = 0, i;
    int rc = 0, fixed = 0;

    if (!rda) { printf("rdbfix DEVICE=<name> UNIT=<n> [SAVE=<file>] [FIX] [RESTORE=<file>]\n"); return 20; }
    port = CreateMsgPort();
    io = (struct IOStdReq *)CreateIORequest(port, sizeof(*io));
    b = AllocMem(512, MEMF_PUBLIC);
    if (!io || !b || OpenDevice((STRPTR)args[0], *(LONG *)args[1], (struct IORequest *)io, 0)) {
        printf("rdbfix: can't open %s unit %ld\n", (char *)args[0], (long)*(LONG *)args[1]);
        return 20;
    }

    if (args[4]) {                               /* RESTORE */
        BPTR f = Open((STRPTR)args[4], MODE_OLDFILE);
        ULONG n = 0;
        if (!f) { printf("rdbfix: can't open %s\n", (char *)args[4]); rc = 20; goto out; }
        while (Read(f, b, 512) == 512) {
            if (blk(CMD_WRITE, b, n, 1)) { printf("rdbfix: write error at block %lu\n", (unsigned long)n); rc = 20; break; }
            n++;
        }
        Close(f);
        blk(CMD_UPDATE, NULL, 0, 0);
        printf("rdbfix: %lu blocks written back from %s\n", (unsigned long)n, (char *)args[4]);
        goto out;
    }

    for (i = 0; i < 16; i++)
        if (!blk(CMD_READ, b, i, 1) && b[0] == RDSK) { rdsk = i; break; }
    if (rdsk == 0xffffffff) { printf("rdbfix: no RDB on this unit\n"); rc = 10; goto out; }
    part = b[7];                                 /* rdb_PartitionList */
    hi = b[33];                                  /* rdb_RDBBlocksHi */
    if (hi < rdsk || hi > 4095) hi = 63;
    printf("RDB at block %lu, RDB area 0-%lu\n", (unsigned long)rdsk, (unsigned long)hi);

    if (args[2]) {                               /* SAVE */
        BPTR f = Open((STRPTR)args[2], MODE_NEWFILE);
        if (!f) { printf("rdbfix: can't create %s\n", (char *)args[2]); rc = 20; goto out; }
        for (i = 0; i <= hi; i++) {
            if (blk(CMD_READ, b, i, 1) || Write(f, b, 512) != 512) {
                printf("rdbfix: backup failed at block %lu\n", (unsigned long)i);
                Close(f);
                rc = 20;
                goto out;
            }
        }
        Close(f);
        printf("saved blocks 0-%lu to %s\n", (unsigned long)hi, (char *)args[2]);
    }

    for (i = 0; part != 0xffffffff && i < 32; i++) {
        ULONG next, *env = b + ENV, mt, mask;
        UBYTE name[32];
        int n;
        if (blk(CMD_READ, b, part, 1) || b[0] != PART) { printf("rdbfix: bad PART block %lu\n", (unsigned long)part); rc = 10; break; }
        next = b[4];
        n = ((UBYTE *)b)[36];
        if (n > 30) n = 30;
        memcpy(name, (UBYTE *)b + 37, n);
        name[n] = 0;
        mt = env[13];
        mask = env[14];
        printf("%-10s block %-4lu MaxTransfer $%08lx  Mask $%08lx  checksum %s", (char *)name, (unsigned long)part,
               (unsigned long)mt, (unsigned long)mask, sum_ok(b) ? "ok" : "BAD");
        if (env[0] < 14) {
            printf("  (old environment, no Mask)\n");
        } else if (mask >= 0x00fffffe) {
            printf("  fine\n");
        } else if (!args[3]) {
            printf("  SWAPPED\n");
            rc = 5;                              /* WARN: the installer asks about it */
        } else if (mt < 0x00fffffe) {
            /* the other value is not a memory mask either: not a swap, something else wrote it */
            printf("  not a swap (MaxTransfer is no mask either) - left alone; set it in HDToolBox\n");
        } else {
            static ULONG chk[128];
            env[13] = mask;                      /* the values belong the other way round */
            env[14] = mt;
            sum_fix(b);
            if (blk(CMD_WRITE, b, part, 1)) {
                printf("  write FAILED (error %d)\n", io->io_Error);
                rc = 20;
                break;
            }
            blk(CMD_UPDATE, NULL, 0, 0);
            if (blk(CMD_READ, chk, part, 1) || memcmp(chk, b, 512)) {
                printf("  read-back DIFFERS\n");
                rc = 20;
                break;
            }
            printf("\n  -> fixed: MaxTransfer $%08lx  Mask $%08lx  checksum ok\n", (unsigned long)env[13], (unsigned long)env[14]);
            fixed++;
        }
        part = next;
    }
    if (args[3]) printf("rdbfix: %d partition(s) fixed\n", fixed);
out:
    CloseDevice((struct IORequest *)io);
    FreeMem(b, 512);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    FreeArgs(rda);
    return rc;
}
