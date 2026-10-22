/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * tick - for timing in scripts. "tick SET" remembers now in ENV:tick;
 * "tick <label> [BYTES=<n>]" prints the 1/50 s ticks since then (and KB/s).
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>

static ULONG now(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Days * 4320000UL + ds.ds_Minute * 3000 + ds.ds_Tick;
}

int main(void)
{
    LONG args[3] = { 0 };
    struct RDArgs *rda = ReadArgs("LABEL,SET/S,BYTES/K/N", args, NULL);
    char buf[16];
    if (!rda) return 20;
    if (args[1]) {
        sprintf(buf, "%lu", (unsigned long)now());
        SetVar("tick", buf, -1, GVF_GLOBAL_ONLY);
    } else if (GetVar("tick", buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0) {
        ULONG t = now() - strtoul(buf, NULL, 10);
        printf("%s: %lu ticks (%lu.%02lu s)", args[0] ? (char *)args[0] : "time",
               (unsigned long)t, (unsigned long)(t / 50), (unsigned long)(t % 50) * 2);
        if (args[2] && t)
            printf(" = %lu KB/s", (unsigned long)(*(LONG *)args[2] / 1024 * 50 / t));
        printf("\n");
    }
    FreeArgs(rda);
    return 0;
}
