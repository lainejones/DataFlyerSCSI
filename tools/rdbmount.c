/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * RDBMount - mounts the RDB partitions of one or more units of a device
 * (the DataFlyer's drives, in S:User-Startup).
 *
 *   RDBMount [DEVICE=<name>] UNITS=<n>[,<n>...] | ALL [FORCE] [QUIET]
 *
 *   DEVICE defaults to dataflyer.device. ALL tries SCSI ids 0-6 (and the
 *   built-in IDE drives, units 100 and 101) and mounts every RDB it finds;
 *   a unit that is already mounted by the system is skipped. FORCE mounts
 *   partitions flagged "no automount" too. The engine is rdbmountcore.h.
 */
#include <exec/execbase.h>
#include <proto/exec.h>
#include <dos/var.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
struct ExpansionBase *ExpansionBase;

static const char ver[] __attribute__((used)) = "$VER: RDBMount 1.0.3 (22.10.2026) DataFlyerSCSI";
static int quiet;
#define RM_PRINTF if (!quiet) printf
#include "rdbmountcore.h"

int main(void)
{
    LONG args[5] = { 0 };
    struct RDArgs *rda = ReadArgs("DEVICE/K,UNITS/K,ALL/S,FORCE/S,QUIET/S", args, NULL);
    const char *dev = args[0] ? (const char *)args[0] : "dataflyer.device";
    static const int all_units[] = { 0, 1, 2, 3, 4, 5, 6, 100, 101, -1 };
    int units[16], found[16], n = 0, i, rc = 0, total = 0;

    if (!rda || (!args[1] && !args[2])) {
        printf("RDBMount [DEVICE=<name>] UNITS=<n>[,<n>...] | ALL [FORCE] [QUIET]\n");
        if (rda) FreeArgs(rda);
        return 5;
    }
    (void)ver;
    quiet = args[4] != 0;
    if (args[2]) {
        for (i = 0; all_units[i] >= 0 && n < 16; i++) units[n++] = all_units[i];
    } else {
        const char *p = (const char *)args[1];
        while (*p && n < 16) {
            while (*p == ',' || *p == ' ') p++;
            if (!*p) break;
            units[n++] = atoi(p);
            while (*p && *p != ',' && *p != ' ') p++;
        }
    }
    ExpansionBase = (struct ExpansionBase *)OpenLibrary((STRPTR)"expansion.library", 36);
    if (!ExpansionBase) { printf("RDBMount: no expansion.library\n"); FreeArgs(rda); return 10; }
    for (i = 0; i < n; i++) {
        LONG r;
        found[i] = 0;
        if (!quiet) printf("%s unit %d:\n", dev, units[i]);
        r = rm_mount_unit(SysBase, DOSBase, ExpansionBase, dev, units[i], args[3] != 0);
        if (r == -2) { if (!quiet) printf("  no drive\n"); if (!args[2]) rc = 5; }
        else if (r < 0) { if (!quiet) printf("  no RDB on it\n"); if (!args[2]) rc = 5; }
        else { total += r; found[i] = 1; if (!quiet) printf("  %ld partition(s) mounted\n", (long)r); }
    }
    if (!quiet && args[2]) printf("RDBMount: %d partition(s) mounted\n", total);
    /* for the installer: the units that had an RDB, as a UNITS= list */
    {
        char list[80];
        int k = 0;
        for (i = 0; i < n; i++)
            if (found[i] && k < 70) k += sprintf(list + k, "%s%d", k ? "," : "", units[i]);
        SetVar((STRPTR)"DataFlyerSCSI/units", (STRPTR)list, k, GVF_GLOBAL_ONLY);
    }
    CloseLibrary((struct Library *)ExpansionBase);
    FreeArgs(rda);
    return rc;
}
