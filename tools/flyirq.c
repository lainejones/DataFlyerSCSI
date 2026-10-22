/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * flyirq - does the DataFlyer's SCSI interrupt reach the CPU, and on which
 * line? Runs the driver's DF_IRQPROBE on a unit (a TEST UNIT READY with the
 * chip armed for a phase-mismatch interrupt, servers counting on INT2 and
 * INT6) and prints what happened.
 *   flyirq UNIT=<n> [TIMES=<n>]
 */
#include <exec/types.h>
#include <exec/io.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

struct IrqProbe { ULONG int2, int6, other, sel_polls, arm_polls, fired, bsr_at_fire, gayle_at_fire,
                  status, csbs_after, cmd_ok, bsr_task, g9000, g9800, late_polls, late_int,
                  monbsy_bsr, monbsy_int2, monbsy_g9000; };

int main(void)
{
    LONG args[2] = { 0 };
    struct RDArgs *rda = ReadArgs("UNIT/A/K/N,TIMES/K/N", args, NULL);
    struct MsgPort *port = CreateMsgPort();
    struct IOStdReq *io = port ? (struct IOStdReq *)CreateIORequest(port, sizeof(*io)) : NULL;
    struct IrqProbe r;
    int times = args[1] ? (int)*(LONG *)args[1] : 3, i;

    if (!rda || !io) { printf("flyirq UNIT=<n> [TIMES=<n>]\n"); return 20; }
    if (OpenDevice((STRPTR)"dataflyer.device", *(LONG *)args[0], (struct IORequest *)io, 0)) {
        printf("flyirq: can't open dataflyer.device unit %ld\n", (long)*(LONG *)args[0]);
        return 10;
    }
    for (i = 0; i < times; i++) {
        memset(&r, 0, sizeof(r));
        io->io_Command = 0x7ff3;
        io->io_Data = &r;
        io->io_Length = sizeof(r);
        if (DoIO((struct IORequest *)io) != 0) { printf("flyirq: error %d (an older driver?)\n", io->io_Error); break; }
        printf("probe %d: INT2 %lu  INT6 %lu  (handler ran %lu times without the chip's bit)\n", i + 1,
               (unsigned long)r.int2, (unsigned long)r.int6, (unsigned long)r.other);
        printf("  select: %lu polls; armed wait: %s after %lu polls; status seen in the handler: "
               "BSR $%02lx Gayle $%02lx; CSBS after $%02lx; command %s, status %ld\n",
               (unsigned long)r.sel_polls,
               r.fired == 1 ? "INTERRUPT" : r.fired == 2 ? "phase changed, NO interrupt" : "TIMEOUT",
               (unsigned long)r.arm_polls, (unsigned long)r.bsr_at_fire, (unsigned long)r.gayle_at_fire,
               (unsigned long)r.csbs_after, r.cmd_ok ? "completed" : "NOT completed", (long)r.status);
        printf("  monitor-busy test (bus free): BSR $%02lx (chip IRQ %s), Gayle status $%02lx, interrupts counted %lu\n",
               (unsigned long)r.monbsy_bsr, (r.monbsy_bsr & 0x10) ? "SET" : "clear", (unsigned long)r.monbsy_g9000,
               (unsigned long)r.monbsy_int2);
        printf("  at the phase change, from the task: BSR $%02lx (bit 4 = chip IRQ %s), Gayle status $%02lx enable $%02lx;"
               " late interrupt: %s (%lu polls)\n", (unsigned long)r.bsr_task, (r.bsr_task & 0x10) ? "SET" : "clear",
               (unsigned long)r.g9000, (unsigned long)r.g9800, r.late_int ? "YES" : "no", (unsigned long)r.late_polls);
    }
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    FreeArgs(rda);
    return 0;
}
