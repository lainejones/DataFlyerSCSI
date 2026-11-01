/* volunit - the device driver and unit behind a drawer or volume.
 *
 *   volunit DIR/A,VAR/K,VOLVAR/K
 *
 * Prints "DEVICE=<driver> UNIT=<n>" for the volume DIR lives on, as the
 * two flysuite arguments; with VAR=<name> it is set as a local Shell
 * variable instead, so a script can write `flysuite $dt ...`, and
 * VOLVAR=<name> gets the volume ("Work:") for AmiDefrag.
 * Return code 0, or 20 if the drawer is not on a device-backed volume
 * (RAM:, a network share, a handler without a FileSysStartupMsg).
 *
 * Part of DriveTests, for RunTests. GPL-3.0, see LICENSE. */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>

static const char ver[] __attribute__((used)) = "$VER: volunit 1.0 (31.10.2026) DriveTests";

int main(void)
{
    LONG args[3] = { 0, 0, 0 };
    char vol[64];
    struct RDArgs *rda;
    struct MsgPort *task;
    struct DosList *dl;
    BPTR lock;
    char out[80];
    int rc = 20;

    if (!(rda = ReadArgs((STRPTR)"DIR/A,VAR/K,VOLVAR/K", args, NULL))) {
        PrintFault(IoErr(), (STRPTR)"volunit");
        return 20;
    }
    if (!(lock = Lock((STRPTR)args[0], ACCESS_READ))) {
        PrintFault(IoErr(), (STRPTR)args[0]);
        FreeArgs(rda);
        return 20;
    }
    task = ((struct FileLock *)BADDR(lock))->fl_Task;
    vol[0] = 0;
    if (NameFromLock(lock, (STRPTR)vol, sizeof(vol))) {   /* "Work:tests/x" -> "Work:" */
        char *c = strchr(vol, ':');
        if (c) c[1] = 0;
    }
    UnLock(lock);

    dl = LockDosList(LDF_DEVICES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_DEVICES))) {
        struct FileSysStartupMsg *fssm;
        const UBYTE *dev;
        if (dl->dol_Task != task || (LONG)dl->dol_misc.dol_handler.dol_Startup <= 64)
            continue;
        fssm = BADDR(dl->dol_misc.dol_handler.dol_Startup);
        if (!TypeOfMem(fssm) || !fssm->fssm_Device)
            continue;
        dev = BADDR(fssm->fssm_Device);
        if (!TypeOfMem((APTR)dev) || !dev[0] || dev[0] > 60)
            continue;
        sprintf(out, "DEVICE=%.*s UNIT=%ld", dev[0], (const char *)dev + 1, (long)fssm->fssm_Unit);
        rc = 0;
        break;
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);

    if (rc != 0)
        printf("volunit: %s is not on a device-backed volume\n", (char *)args[0]);
    else if (args[1])
        SetVar((STRPTR)args[1], (STRPTR)out, -1, GVF_LOCAL_ONLY);
    else
        printf("%s\n", out);
    FreeArgs(rda);
    return rc;
}
