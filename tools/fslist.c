/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * fslist - the file systems in FileSystem.resource: DosType, version,
 * patch flags, seglist and who added it (the node name; "RDBMount" for one
 * RDBMount loaded from an RDB). A test aid for RDBMount.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <resources/filesysres.h>
#include <proto/exec.h>
#include <stdio.h>

int main(void)
{
    struct FileSysResource *fsr = OpenResource((STRPTR)FSRNAME);
    struct FileSysEntry *fse;
    char line[64][100];
    int n = 0, i;

    if (!fsr) { printf("fslist: no FileSystem.resource\n"); return 10; }
    Forbid();
    for (fse = (struct FileSysEntry *)fsr->fsr_FileSysEntries.lh_Head; fse->fse_Node.ln_Succ && n < 64;
         fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
        ULONG t = fse->fse_DosType;
        sprintf(line[n++], "%c%c%c\\%lx  $%08lx  version %lu.%lu  patch $%03lx  seglist $%08lx  %s",
                (char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (unsigned long)(t & 0xff), (unsigned long)t,
                (unsigned long)(fse->fse_Version >> 16), (unsigned long)(fse->fse_Version & 0xffff),
                (unsigned long)fse->fse_PatchFlags, (unsigned long)fse->fse_SegList,
                fse->fse_Node.ln_Name ? fse->fse_Node.ln_Name : "-");
    }
    Permit();
    for (i = 0; i < n; i++)
        printf("%s\n", line[i]);
    return 0;
}
