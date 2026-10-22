/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * flychange - watch a removable unit's disk-change notification.
 *   flychange UNIT=<n> [DEVICE=<name>] [SECONDS=<n>] [EJECT] [LOAD]
 *
 * Adds a TD_ADDCHANGEINT interrupt, optionally ejects (or loads) the medium
 * with TD_EJECT, then waits up to SECONDS (default 30) and reports each time
 * the driver fires it, with TD_CHANGENUM and TD_CHANGESTATE; then removes it.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/interrupts.h>
#include <devices/trackdisk.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

static struct Task *me;
static ULONG sigmask;

/* the interrupt code: Cause()d by the driver; just signals the task */
static void chg_code(void)
{
    Signal(me, sigmask);
}

int main(void)
{
    LONG args[5] = { 0 };
    struct RDArgs *rda = ReadArgs("UNIT/A/K/N,DEVICE/K,SECONDS/K/N,EJECT/S,LOAD/S", args, NULL);
    const char *dev = args[1] ? (const char *)args[1] : "dataflyer.device";
    LONG secs = args[2] ? *(LONG *)args[2] : 30;
    struct MsgPort *port = CreateMsgPort(), *tport = CreateMsgPort();
    struct IOStdReq *io, *cio, *add;
    struct timerequest *tr;
    struct Interrupt is;
    BYTE sig;
    int fired = 0;
    ULONG t0;

    if (!rda) { printf("flychange UNIT=<n> [DEVICE=<name>] [SECONDS=<n>] [EJECT] [LOAD]\n"); return 20; }
    io = (struct IOStdReq *)CreateIORequest(port, sizeof(*io));
    cio = (struct IOStdReq *)CreateIORequest(port, sizeof(*cio));
    add = (struct IOStdReq *)CreateIORequest(port, sizeof(*add));
    tr = (struct timerequest *)CreateIORequest(tport, sizeof(*tr));
    if (!io || !cio || !add || !tr || OpenDevice((STRPTR)dev, *(LONG *)args[0], (struct IORequest *)io, 0)) {
        printf("flychange: can't open %s unit %ld\n", dev, (long)*(LONG *)args[0]);
        return 10;
    }
    if (OpenDevice((STRPTR)"timer.device", UNIT_VBLANK, (struct IORequest *)tr, 0)) { printf("no timer\n"); return 10; }
    *cio = *io; *add = *io;
    me = FindTask(NULL);
    sig = AllocSignal(-1);
    sigmask = 1UL << sig;
    memset(&is, 0, sizeof(is));
    is.is_Node.ln_Type = NT_INTERRUPT;
    is.is_Node.ln_Name = "flychange";
    is.is_Code = (void (*)())chg_code;

    cio->io_Command = TD_CHANGENUM; DoIO((struct IORequest *)cio);
    printf("%s unit %ld: change count %lu, ", dev, (long)*(LONG *)args[0], (unsigned long)cio->io_Actual);
    cio->io_Command = TD_CHANGESTATE; DoIO((struct IORequest *)cio);
    printf("%s\n", cio->io_Actual ? "no medium" : "medium in");

    add->io_Command = TD_ADDCHANGEINT;
    add->io_Data = &is;
    add->io_Length = sizeof(is);
    add->io_Flags = 0;
    SendIO((struct IORequest *)add);                 /* stays pending */

    if (args[3] || args[4]) {
        cio->io_Command = TD_EJECT;
        cio->io_Length = args[3] ? 1 : 0;            /* 1 = eject, 0 = load */
        DoIO((struct IORequest *)cio);
        printf("TD_EJECT %s: error %d\n", args[3] ? "eject" : "load", cio->io_Error);
    }

    t0 = 0;
    tr->tr_node.io_Command = TR_ADDREQUEST;
    tr->tr_time.tv_secs = secs; tr->tr_time.tv_micro = 0;
    SendIO((struct IORequest *)tr);
    printf("waiting %ld s for a change...\n", (long)secs);
    for (;;) {
        ULONG got = Wait(sigmask | (1UL << tport->mp_SigBit) | SIGBREAKF_CTRL_C);
        if (got & sigmask) {
            struct DateStamp ds;
            DateStamp(&ds);
            cio->io_Command = TD_CHANGENUM; DoIO((struct IORequest *)cio);
            printf("  CHANGE fired: count %lu, ", (unsigned long)cio->io_Actual);
            cio->io_Command = TD_CHANGESTATE; DoIO((struct IORequest *)cio);
            printf("%s (at %ld:%02ld.%02ld)\n", cio->io_Actual ? "no medium" : "medium in",
                   (long)(ds.ds_Minute / 60), (long)(ds.ds_Minute % 60), (long)(ds.ds_Tick / 50));
            fired++;
        }
        if (got & SIGBREAKF_CTRL_C) break;
        if (GetMsg(tport)) break;
        (void)t0;
    }
    if (!CheckIO((struct IORequest *)tr)) { AbortIO((struct IORequest *)tr); }
    WaitIO((struct IORequest *)tr);

    cio->io_Command = TD_REMCHANGEINT;
    cio->io_Data = &is;
    cio->io_Length = sizeof(is);
    DoIO((struct IORequest *)cio);
    WaitIO((struct IORequest *)add);                 /* answered by the remove */
    printf("flychange: %d change(s) seen -> %s\n", fired, fired ? "OK" : "none");

    CloseDevice((struct IORequest *)tr);
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)tr);
    DeleteIORequest((struct IORequest *)add);
    DeleteIORequest((struct IORequest *)cio);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(tport);
    DeleteMsgPort(port);
    FreeSignal(sig);
    FreeArgs(rda);
    return fired ? 0 : 5;
}
