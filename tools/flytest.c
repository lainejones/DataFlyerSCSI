/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * flytest - test dataflyer.device through the normal device interface.
 *
 *   flytest UNIT=<n> [DEVICE=<name>] [PATTERN] [WRITE] [SPEED]
 *
 *   always:   NSCMD_DEVICEQUERY, TD_GETGEOMETRY, HD_SCSICMD INQUIRY
 *   PATTERN:  the unit holds the test pattern (every 512-byte block = its own
 *             number + "BLK!"): CMD_READ single blocks, a 200 KB read across the
 *             driver's 64 KB chunks, TD_READ64 + NSCMD_TD_READ64 near the end,
 *             all checked byte for byte; and the read speed
 *   SPEED:    read 2.5 MB from the start of the unit, check nothing: safe on any disk
 *   WRITE:    the unit is scratch: write 300 KB of a pattern, read it back,
 *             compare; and the write speed
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/newstyle.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

static struct IOStdReq *io;
static int fails, passes;

#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("  FAIL: "); printf(__VA_ARGS__); \
                           printf(" (io_Error %d)\n", io->io_Error); } } while (0)

static ULONG be32(const UBYTE *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }

static int doio(UWORD cmd, APTR data, ULONG len, ULONG off, ULONG hi)
{
    io->io_Command = cmd;
    io->io_Data = data;
    io->io_Length = len;
    io->io_Offset = off;
    io->io_Actual = hi;
    return DoIO((struct IORequest *)io);
}

/* blocks [first, first+n) hold the pattern? */
static int pattern_ok(const UBYTE *buf, ULONG first, ULONG n)
{
    ULONG b, k;
    for (b = 0; b < n; b++)
        for (k = 0; k < 512; k += 8)
            if (be32(buf + b * 512 + k) != first + b || memcmp(buf + b * 512 + k + 4, "BLK!", 4))
                return 0;
    return 1;
}

static ULONG ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return ds.ds_Minute * 3000 + ds.ds_Tick;
}

int main(void)
{
    LONG args[5] = { 0 };
    struct RDArgs *rda = ReadArgs("UNIT/A/K/N,DEVICE/K,PATTERN/S,WRITE/S,SPEED/S", args, NULL);
    const char *devname = args[1] ? (const char *)args[1] : "dataflyer.device";
    struct MsgPort *port;
    int unit;
    UBYTE *buf;
    ULONG blocks = 0, bsize = 512;
    int devtype = 0;

    if (!rda) { printf("flytest UNIT=<n> [DEVICE=<name>] [PATTERN] [WRITE] [SPEED]\n"); return 20; }
    unit = (int)*(LONG *)args[0];
    port = CreateMsgPort();
    io = port ? (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq)) : NULL;
    buf = AllocMem(400 * 1024, MEMF_PUBLIC | MEMF_CLEAR);
    if (!io || !buf) { printf("flytest: no memory\n"); return 20; }
    if (OpenDevice((STRPTR)devname, unit, (struct IORequest *)io, 0)) {
        printf("flytest: can't open %s unit %d (error %d)\n", devname, unit, io->io_Error);
        if (io->io_Error == IOERR_UNITBUSY)
            printf("  The driver could not take the built-in IDE port over from scsi.device.\n");
        else if (io->io_Error == TDERR_BadUnitNum)
            printf("  No drive answers at that unit (SCSI id + 10 * LUN, or 100/101 for IDE).\n");
        return 10;
    }
    printf("flytest: %s unit %d\n", devname, unit);

    {   /* NSD */
        struct NSDeviceQueryResult q;
        memset(&q, 0, sizeof(q));
        CHECK(doio(NSCMD_DEVICEQUERY, &q, sizeof(q), 0, 0) == 0 && q.nsdqr_DeviceType == NSDEVTYPE_TRACKDISK,
              "NSCMD_DEVICEQUERY");
        if (q.nsdqr_SupportedCommands) {
            UWORD *c = q.nsdqr_SupportedCommands;
            int n = 0, has64 = 0;
            while (*c) { if (*c == NSCMD_TD_READ64) has64 = 1; c++; n++; }
            printf("  NSD: %d commands%s\n", n, has64 ? ", 64-bit" : "");
        }
    }
    {   /* geometry */
        struct DriveGeometry dg;
        memset(&dg, 0, sizeof(dg));
        CHECK(doio(TD_GETGEOMETRY, &dg, sizeof(dg), 0, 0) == 0, "TD_GETGEOMETRY");
        blocks = dg.dg_TotalSectors;
        devtype = dg.dg_DeviceType;
        bsize = dg.dg_SectorSize;
        printf("  geometry: %lu blocks of %lu bytes, type %d%s\n", (unsigned long)blocks,
               (unsigned long)bsize, dg.dg_DeviceType, (dg.dg_Flags & DGF_REMOVABLE) ? ", removable" : "");
    }
    {   /* HD_SCSICMD INQUIRY */
        struct SCSICmd sc;
        UBYTE cdb[6] = { 0x12, 0, 0, 0, 36, 0 }, inq[36], sense[18];
        memset(&sc, 0, sizeof(sc));
        memset(inq, 0, sizeof(inq));
        sc.scsi_Data = (UWORD *)inq;
        sc.scsi_Length = 36;
        sc.scsi_Command = cdb;
        sc.scsi_CmdLength = 6;
        sc.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
        sc.scsi_SenseData = sense;
        sc.scsi_SenseLength = sizeof(sense);
        CHECK(doio(HD_SCSICMD, &sc, sizeof(sc), 0, 0) == 0 && sc.scsi_Status == 0 && sc.scsi_Actual == 36,
              "HD_SCSICMD INQUIRY");
        printf("  HD_SCSICMD INQUIRY: \"%.8s\" \"%.16s\"\n", (char *)inq + 8, (char *)inq + 16);
    }
    {   /* HD_SCSICMD with a bad command: CHECK CONDITION + sense */
        struct SCSICmd sc;
        UBYTE cdb[6] = { 0xff, 0, 0, 0, 0, 0 }, sense[18];
        memset(&sc, 0, sizeof(sc));
        memset(sense, 0, sizeof(sense));
        sc.scsi_Command = cdb;
        sc.scsi_CmdLength = 6;
        sc.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
        sc.scsi_SenseData = sense;
        sc.scsi_SenseLength = sizeof(sense);
        doio(HD_SCSICMD, &sc, sizeof(sc), 0, 0);
        CHECK(io->io_Error == HFERR_BadStatus && sc.scsi_Status == 2 && sc.scsi_SenseActual >= 8 &&
              (sense[2] & 0x0f) == 5, "HD_SCSICMD bad opcode -> CHECK CONDITION, sense ILLEGAL REQUEST");
    }
    {   /* MODE SENSE for all pages, as HDToolBox sends it: pages 3 and 4 (geometry) are in it, or
         * the target says it has no mode pages at all (a CD-ROM, some SCSI emulators) */
        struct SCSICmd sc;
        UBYTE cdb[6] = { 0x1a, 0, 0x3f, 0, 254, 0 }, md[256], sense[18];
        int i, got3 = 0, got4 = 0, end;
        memset(&sc, 0, sizeof(sc));
        memset(md, 0, sizeof(md));
        sc.scsi_Data = (UWORD *)md;
        sc.scsi_Length = 254;
        sc.scsi_Command = cdb;
        sc.scsi_CmdLength = 6;
        sc.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
        sc.scsi_SenseData = sense;
        sc.scsi_SenseLength = sizeof(sense);
        doio(HD_SCSICMD, &sc, sizeof(sc), 0, 0);
        if (sc.scsi_Status == 0 && sc.scsi_Actual >= 4) {
            end = md[0] + 1;
            if (end > (int)sc.scsi_Actual) end = sc.scsi_Actual;
            for (i = 4 + md[3]; i + 1 < end; i += md[i + 1] + 2) {
                if ((md[i] & 0x3f) == 3) got3 = 1;
                if ((md[i] & 0x3f) == 4) got4 = 1;
            }
            if (devtype == 0)                    /* a disk has the geometry pages; a CD-ROM has its own */
                CHECK(got3 && got4, "MODE SENSE all pages: %lu bytes, page 3 %s, page 4 %s",
                      (unsigned long)sc.scsi_Actual, got3 ? "yes" : "NO", got4 ? "yes" : "NO");
            else
                printf("  MODE SENSE all pages: %lu bytes (not a disk: no geometry pages expected)\n",
                       (unsigned long)sc.scsi_Actual);
        } else
            printf("  MODE SENSE all pages: not supported (status %d), fine for a CD-ROM\n", sc.scsi_Status);
    }

    if (args[2] && blocks) {   /* the pattern unit */
        ULONG n, t0, t1, kb = 0;
        int i;
        static const ULONG one[] = { 0, 1, 12345 };
        for (i = 0; i < 3; i++) {
            n = one[i];
            memset(buf, 0, 512);
            CHECK(doio(CMD_READ, buf, 512, n * 512, 0) == 0 && io->io_Actual == 512 && pattern_ok(buf, n, 1),
                  "CMD_READ block %lu", (unsigned long)n);
        }
        memset(buf, 0, 200 * 1024);
        CHECK(doio(CMD_READ, buf, 200 * 1024, 1000 * 512, 0) == 0 && io->io_Actual == 200 * 1024 &&
              pattern_ok(buf, 1000, 400), "CMD_READ 200 KB across 64 KB chunks");
        memset(buf, 0, 4096);
        n = blocks - 8;
        CHECK(doio(TD_READ64, buf, 4096, n * 512, 0) == 0 && pattern_ok(buf, n, 8), "TD_READ64 the last 8 blocks");
        memset(buf, 0, 4096);
        CHECK(doio(NSCMD_TD_READ64, buf, 4096, n * 512, 0) == 0 && pattern_ok(buf, n, 8),
              "NSCMD_TD_READ64 the last 8 blocks");
        doio(CMD_READ, buf, 512, blocks * 512, 0);
        CHECK(io->io_Error != 0, "CMD_READ past the end fails");
        t0 = ticks();
        for (i = 0; i < 10; i++) {
            if (doio(CMD_READ, buf, 256 * 1024, (ULONG)i * 256 * 1024, 0) == 0) kb += 256;
        }
        t1 = ticks();
        if (t1 > t0)
            printf("  read speed: %lu KB in %lu ticks = %lu KB/s\n", (unsigned long)kb,
                   (unsigned long)(t1 - t0), (unsigned long)(kb * 50 / (t1 - t0)));
    }

    if (args[4] && blocks) {   /* read speed only - safe on any disk, nothing is checked or written */
        ULONG t0, t1, kb = 0, want = blocks > 8192 ? 10 : 1;
        int i;
        t0 = ticks();
        for (i = 0; i < (int)want; i++)
            if (doio(CMD_READ, buf, 256 * 1024, (ULONG)i * 256 * 1024, 0) == 0) kb += 256;
        t1 = ticks();
        CHECK(kb == want * 256, "read %lu x 256 KB", (unsigned long)want);
        if (t1 > t0)
            printf("  read speed: %lu KB in %lu ticks = %lu KB/s\n", (unsigned long)kb,
                   (unsigned long)(t1 - t0), (unsigned long)(kb * 50 / (t1 - t0)));
    }

    if (args[3] && blocks) {   /* the scratch unit */
        ULONG first = 2048, nb = 600, b, k, t0, t1;
        UBYTE *rb = buf + 300 * 1024 - 0;
        for (b = 0; b < nb; b++)
            for (k = 0; k < 512; k += 4) {
                ULONG v = (first + b) * 0x9e3779b1UL + k;
                buf[b * 512 + k] = (UBYTE)(v >> 24); buf[b * 512 + k + 1] = (UBYTE)(v >> 16);
                buf[b * 512 + k + 2] = (UBYTE)(v >> 8); buf[b * 512 + k + 3] = (UBYTE)v;
            }
        t0 = ticks();
        CHECK(doio(CMD_WRITE, buf, nb * 512, first * 512, 0) == 0 && io->io_Actual == nb * 512,
              "CMD_WRITE 300 KB");
        t1 = ticks();
        if (t1 > t0)
            printf("  write speed: 300 KB in %lu ticks = %lu KB/s\n", (unsigned long)(t1 - t0),
                   (unsigned long)(300 * 50 / (t1 - t0)));
        rb = AllocMem(nb * 512, MEMF_PUBLIC | MEMF_CLEAR);
        if (rb) {
            CHECK(doio(CMD_READ, rb, nb * 512, first * 512, 0) == 0 && !memcmp(rb, buf, nb * 512),
                  "read back 300 KB, compare");
            FreeMem(rb, nb * 512);
        }
        CHECK(doio(CMD_UPDATE, NULL, 0, 0, 0) == 0, "CMD_UPDATE");
    }

    CloseDevice((struct IORequest *)io);
    printf("flytest: %d passed, %d failed -> %s\n", passes, fails, fails ? "FAIL" : "PASS");
    FreeMem(buf, 400 * 1024);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    FreeArgs(rda);
    return fails ? 5 : 0;
}
