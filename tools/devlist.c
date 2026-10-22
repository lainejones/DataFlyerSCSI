/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * devlist - every mounted DOS device with the exec device and unit behind it
 * (what dataflyer.device's built-in-IDE check looks at).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>

int main(void)
{
    struct DosList *dl = LockDosList(LDF_DEVICES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_DEVICES))) {
        const UBYTE *nm = BADDR(dl->dol_Name);
        BPTR st = dl->dol_misc.dol_handler.dol_Startup;
        struct FileSysStartupMsg *fssm = BADDR(st);
        printf("%-10.*s ", nm ? nm[0] : 0, nm ? (const char *)nm + 1 : "");
        if ((LONG)st > 64 && TypeOfMem(fssm) && fssm->fssm_Device && TypeOfMem(BADDR(fssm->fssm_Device))) {
            const UBYTE *d = BADDR(fssm->fssm_Device);
            ULONG *env = BADDR(fssm->fssm_Environ);
            printf("%.*s unit %lu", d[0], (const char *)d + 1, (unsigned long)fssm->fssm_Unit);
            if (env && TypeOfMem(env) && env[DE_TABLESIZE] >= DE_MASK)
                printf("  BufMemType %lu MaxTransfer $%lx Mask $%08lx", (unsigned long)env[DE_MEMBUFTYPE],
                       (unsigned long)(env[DE_TABLESIZE] >= DE_MAXTRANSFER ? env[DE_MAXTRANSFER] : 0),
                       (unsigned long)env[DE_MASK]);
            printf("\n");
        } else
            printf("(no device)\n");
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    return 0;
}
