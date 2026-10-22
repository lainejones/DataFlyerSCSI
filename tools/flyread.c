/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * flyread - one READ(10) through HD_SCSICMD, and one CMD_READ, with what came
 * back: for looking at a single device's answers.
 *   flyread UNIT=<n> [DEVICE=<name>] [LBA=<n>] [BLOCKS=<n>] [BSIZE=<n>]
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    LONG args[5] = { 0 };
    struct RDArgs *rda = ReadArgs("UNIT/A/K/N,DEVICE/K,LBA/K/N,BLOCKS/K/N,BSIZE/K/N", args, NULL);
    const char *dev = args[1] ? (const char *)args[1] : "dataflyer.device";
    ULONG lba = args[2] ? *(LONG *)args[2] : 0, n = args[3] ? *(LONG *)args[3] : 1;
    ULONG bs = args[4] ? *(LONG *)args[4] : 512, len;
    struct MsgPort *port;
    struct IOStdReq *io;
    struct SCSICmd sc;
    UBYTE cdb[10], sense[18], *buf;
    int i;

    if (!rda) { printf("flyread UNIT=<n> [DEVICE=<name>] [LBA=<n>] [BLOCKS=<n>] [BSIZE=<n>]\n"); return 20; }
    len = n * bs;
    port = CreateMsgPort();
    io = (struct IOStdReq *)CreateIORequest(port, sizeof(*io));
    buf = AllocMem(len + 16, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf || OpenDevice((STRPTR)dev, *(LONG *)args[0], (struct IORequest *)io, 0)) {
        printf("flyread: can't open %s\n", dev);
        return 10;
    }
    memset(&sc, 0, sizeof(sc));
    memset(cdb, 0, sizeof(cdb));
    memset(sense, 0, sizeof(sense));
    cdb[0] = 0x28;
    cdb[2] = (UBYTE)(lba >> 24); cdb[3] = (UBYTE)(lba >> 16); cdb[4] = (UBYTE)(lba >> 8); cdb[5] = (UBYTE)lba;
    cdb[7] = (UBYTE)(n >> 8); cdb[8] = (UBYTE)n;
    sc.scsi_Data = (UWORD *)buf;
    sc.scsi_Length = len;
    sc.scsi_Command = cdb;
    sc.scsi_CmdLength = 10;
    sc.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
    sc.scsi_SenseData = sense;
    sc.scsi_SenseLength = sizeof(sense);
    io->io_Command = HD_SCSICMD;
    io->io_Data = &sc;
    io->io_Length = sizeof(sc);
    DoIO((struct IORequest *)io);
    printf("READ(10) lba %lu x %lu: io_Error %d, status %d, actual %lu of %lu, sense %lu: key %x asc %02x/%02x\n",
           (unsigned long)lba, (unsigned long)n, io->io_Error, sc.scsi_Status, (unsigned long)sc.scsi_Actual,
           (unsigned long)len, (unsigned long)sc.scsi_SenseActual, sense[2] & 15, sense[12], sense[13]);
    printf("  data:");
    for (i = 0; i < 16; i++) printf(" %02x", buf[i]);
    printf("\n");
    memset(buf, 0, len);
    io->io_Command = CMD_READ;
    io->io_Data = buf;
    io->io_Length = len;
    io->io_Offset = lba * bs;
    DoIO((struct IORequest *)io);
    printf("CMD_READ: io_Error %d, actual %lu\n  data:", io->io_Error, (unsigned long)io->io_Actual);
    for (i = 0; i < 16; i++) printf(" %02x", buf[i]);
    printf("\n");
    CloseDevice((struct IORequest *)io);
    FreeMem(buf, len + 16);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    FreeArgs(rda);
    return 0;
}
