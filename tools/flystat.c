/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * flystat - dataflyer.device's request counters since the last flystat
 * (they are cleared on every read).  flystat [UNIT=<n>] [OPTS=<n>]
 * OPTS sets the driver's options (bit 0: writes in 256-byte runs, on by default; OPTS=0 = a byte at a time).
 */
#include <exec/types.h>
#include <exec/io.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#define DF_STATS 0x7ff0
struct DFStats {
    ULONG reads, writes, rbytes, wbytes, cmds;
    ULONG size[8];
    ULONG polls[8];
    ULONG timeouts;
    ULONG sleeps;
};

int main(void)
{
    LONG args[3] = { 0 };
    struct RDArgs *rda = ReadArgs("UNIT/K/N,OPTS/K/N,DIAG/S", args, NULL);
    int unit = args[0] ? (int)*(LONG *)args[0] : 3, i;
    static const char *const names[8] = { "<=512", "<=2K", "<=8K", "<=32K", "<=64K", "<=128K", "<=512K", ">512K" };
    struct MsgPort *port = CreateMsgPort();
    struct IOStdReq *io = port ? (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq)) : NULL;
    struct DFStats st;

    if (!io || OpenDevice((STRPTR)"dataflyer.device", unit, (struct IORequest *)io, 0)) {
        printf("flystat: can't open dataflyer.device unit %d\n", unit);
        return 10;
    }
    if (args[1]) {
        io->io_Command = 0x7ff1;          /* DF_OPTS */
        io->io_Offset = *(LONG *)args[1];
        DoIO((struct IORequest *)io);
        printf("dataflyer.device options: %lu (were %lu)\n", (unsigned long)*(LONG *)args[1], (unsigned long)io->io_Actual);
    }
    if (args[2]) {                        /* DF_DIAG: the IDE takeover */
        ULONG d[8];
        memset(d, 0, sizeof(d));
        io->io_Command = 0x7ff2;
        io->io_Data = d;
        io->io_Length = sizeof(d);
        DoIO((struct IORequest *)io);
        printf("takeover: scsi.device units $%08lx $%08lx -> IDE drives $%08lx $%08lx\n", (unsigned long)d[0],
               (unsigned long)d[1], (unsigned long)d[2], (unsigned long)d[3]);
        printf("  unmatched scsi.device requests: %lu (last: unit $%08lx, command %lu, task $%08lx)\n",
               (unsigned long)d[4], (unsigned long)d[5], (unsigned long)d[6], (unsigned long)d[7]);
    }
    io->io_Command = DF_STATS;
    io->io_Data = &st;
    io->io_Length = sizeof(st);
    if (DoIO((struct IORequest *)io) == 0 && io->io_Actual == sizeof(st)) {
        printf("dataflyer.device: %lu reads (%lu KB), %lu writes (%lu KB), %lu SCSI commands\n",
               (unsigned long)st.reads, (unsigned long)(st.rbytes >> 10), (unsigned long)st.writes,
               (unsigned long)(st.wbytes >> 10), (unsigned long)st.cmds);
        printf("  request sizes:");
        for (i = 0; i < 8; i++)
            if (st.size[i]) printf("  %s %lu", names[i], (unsigned long)st.size[i]);
        printf("\n");
        {
            static const char *const w[8] = { "bus free", "select", "ACK", "REQ", "BSY end", "DMA in", "DMA out", "IDE" };
            printf("  wait polls (~1.4 us each on real hardware):");
            for (i = 0; i < 8; i++)
                if (st.polls[i]) printf("  %s %lu", w[i], (unsigned long)st.polls[i]);
            printf("\n  timeouts: %lu, timer sleeps (250 us) inside waits: %lu\n", (unsigned long)st.timeouts,
                   (unsigned long)st.sleeps);
        }
    } else
        printf("flystat: no counters (an older dataflyer.device?)\n");
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    if (rda) FreeArgs(rda);
    return 0;
}
