/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * flyprobe - talk to the DataFlyer 1200 SCSI+'s NCR 53C80 directly (programmed
 * I/O, no driver): select the SCSI side of the Gayle port, INQUIRY every SCSI id,
 * READ CAPACITY the ones that answer, and with CHECK=<id> read blocks and check
 * the test pattern (every 512-byte block = its own number + "BLK!", repeated).
 * The hardware map is docs/hardware.md.
 *
 *   flyprobe [CHECK=<id>] [RESET]
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#define BASE        0xDA0000UL
#define REG(n)      (*(volatile UBYTE *)(BASE + (n) * 4 + 2))
#define SEL_SCSI    (*(volatile UWORD *)0xDA1012UL)     /* read: the port becomes the 53C80 */
#define SEL_IDE     (*(volatile UWORD *)0xDA1016UL)     /* read: back to the IDE task file  */

/* 53C80 registers */
#define R_CSD  0    /* read: current SCSI data     write: output data       */
#define R_ICR  1    /* initiator command                                     */
#define R_MR   2    /* mode                                                  */
#define R_TCR  3    /* target command                                        */
#define R_CSBS 4    /* read: current bus status    write: select enable     */
#define R_BSR  5    /* read: bus and status        write: start DMA send    */
#define R_RPI  7    /* read: reset parity/interrupt                          */

#define ICR_DBUS 0x01
#define ICR_ATN  0x02
#define ICR_SEL  0x04
#define ICR_BSY  0x08
#define ICR_ACK  0x10
#define ICR_RST  0x80

#define CSBS_REQ 0x20
#define CSBS_BSY 0x40
#define BSR_PHASEMATCH 0x08

/* information transfer phases: MSG<<2 | C/D<<1 | I/O */
#define PH_DATA_OUT 0
#define PH_DATA_IN  1
#define PH_COMMAND  2
#define PH_STATUS   3
#define PH_MSG_OUT  6
#define PH_MSG_IN   7

#define OUR_ID 7

static int wait_set(int reg, UBYTE mask, ULONG loops)
{
    while (loops--)
        if (REG(reg) & mask)
            return 1;
    return 0;
}

static int wait_clear(int reg, UBYTE mask, ULONG loops)
{
    while (loops--)
        if (!(REG(reg) & mask))
            return 1;
    return 0;
}

static void bus_reset(void)
{
    REG(R_ICR) = ICR_RST;
    Delay(1);                                /* 20 ms - the spec wants >= 25 us */
    REG(R_ICR) = 0;
    (void)REG(R_RPI);
    Delay(25);                               /* reset-to-selection time, 250 ms */
}

/* select a target (single initiator: no arbitration) */
static int select_target(int id)
{
    REG(R_MR) = 0;
    REG(R_TCR) = 0;
    REG(R_ICR) = 0;
    if (REG(R_CSBS) & CSBS_BSY)
        return 0;                            /* bus not free */
    REG(R_CSD) = (UBYTE)((1 << id) | (1 << OUR_ID));
    REG(R_ICR) = ICR_DBUS;
    REG(R_ICR) = ICR_DBUS | ICR_SEL;
    if (!wait_set(R_CSBS, CSBS_BSY, 200000)) {   /* no target there */
        REG(R_ICR) = 0;
        return 0;
    }
    REG(R_ICR) = 0;                          /* release SEL and the data bus */
    return 1;
}

/* one byte out in the current phase */
static int put_byte(UBYTE b)
{
    REG(R_CSD) = b;
    REG(R_ICR) = ICR_DBUS;
    REG(R_ICR) = ICR_DBUS | ICR_ACK;
    if (!wait_clear(R_CSBS, CSBS_REQ, 200000))
        return 0;
    REG(R_ICR) = 0;
    return 1;
}

/* one byte in */
static int get_byte(UBYTE *b)
{
    *b = REG(R_CSD);
    REG(R_ICR) = ICR_ACK;
    if (!wait_clear(R_CSBS, CSBS_REQ, 200000))
        return 0;
    REG(R_ICR) = 0;
    return 1;
}

/* A whole command: CDB out, data in (up to len) or out, status, message.
 * Returns the SCSI status byte, or -1 on a bus error / timeout. *got = data bytes. */
static int scsi_cmd(int id, const UBYTE *cdb, int cdblen, UBYTE *data, ULONG len, int out, ULONG *got)
{
    int status = -1, ci = 0;
    ULONG di = 0;
    if (got) *got = 0;
    if (!select_target(id))
        return -1;
    for (;;) {
        UBYTE csbs, b;
        int phase;
        if (!wait_set(R_CSBS, CSBS_REQ | 0, 2000000)) {
            if (!(REG(R_CSBS) & CSBS_BSY))
                break;                       /* bus free: done */
            return -1;
        }
        csbs = REG(R_CSBS);
        if (!(csbs & CSBS_BSY))
            break;
        phase = (csbs >> 2) & 7;
        REG(R_TCR) = (UBYTE)phase;           /* so the chip matches the phase */
        switch (phase) {
        case PH_COMMAND:
            if (!put_byte(ci < cdblen ? cdb[ci] : 0)) return -1;
            ci++;
            break;
        case PH_DATA_IN:
            if (!get_byte(&b)) return -1;
            if (data && !out && di < len) data[di] = b;
            di++;
            break;
        case PH_DATA_OUT:
            if (!put_byte(data && out && di < len ? data[di] : 0)) return -1;
            di++;
            break;
        case PH_STATUS:
            if (!get_byte(&b)) return -1;
            status = b;
            break;
        case PH_MSG_IN:
            if (!get_byte(&b)) return -1;
            if (b == 0x00) {                 /* COMMAND COMPLETE: the target frees the bus */
                REG(R_TCR) = 0;
                wait_clear(R_CSBS, CSBS_BSY, 200000);
                if (got) *got = di;
                return status;
            }
            break;
        case PH_MSG_OUT:
            if (!put_byte(0x08)) return -1;  /* NOP */
            break;
        default:
            return -1;
        }
    }
    if (got) *got = di;
    return status;
}

static ULONG be32(const UBYTE *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | (p[2] << 8) | p[3]; }

static int read_block(int id, ULONG lba, UBYTE *buf)
{
    UBYTE cdb[10] = { 0x28, 0, (UBYTE)(lba >> 24), (UBYTE)(lba >> 16), (UBYTE)(lba >> 8), (UBYTE)lba, 0, 0, 1, 0 };
    ULONG got;
    int st = scsi_cmd(id, cdb, 10, buf, 512, 0, &got);
    return st == 0 && got == 512;
}

int main(void)
{
    LONG args[2] = { 0 };
    struct RDArgs *rda = ReadArgs("CHECK/K/N,RESET/S", args, NULL);
    static UBYTE buf[512];
    int id, check = args[0] ? (int)*(LONG *)args[0] : -1;
    ULONG blocks[8] = { 0 };

    printf("flyprobe: DataFlyer 1200 SCSI+ 53C80 at $%06lx\n", BASE);
    (void)SEL_SCSI;                          /* the port becomes the 53C80 */
    REG(R_MR) = 0;
    REG(R_ICR) = 0;
    REG(R_TCR) = 0;
    (void)REG(R_RPI);
    printf("  bus status %02x, bus and status %02x\n", REG(R_CSBS), REG(R_BSR));
    if (args[1]) {
        printf("  SCSI bus reset\n");
        bus_reset();
    }
    for (id = 0; id < 7; id++) {
        static const UBYTE inq[6] = { 0x12, 0, 0, 0, 36, 0 };
        UBYTE cap[8], rc[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        ULONG got;
        int st;
        memset(buf, 0, 64);
        st = scsi_cmd(id, inq, 6, buf, 36, 0, &got);
        if (st < 0) {
            printf("  id %d: -\n", id);
            continue;
        }
        printf("  id %d: status %d, %lu bytes: type %d, \"%.8s\" \"%.16s\" \"%.4s\"\n", id, st,
               (unsigned long)got, buf[0] & 31, (char *)buf + 8, (char *)buf + 16, (char *)buf + 32);
        if (scsi_cmd(id, rc, 10, cap, 8, 0, &got) == 0 && got == 8) {
            blocks[id] = be32(cap) + 1;
            printf("        %lu blocks of %lu bytes (%lu MB)\n", (unsigned long)blocks[id],
                   (unsigned long)be32(cap + 4), (unsigned long)(blocks[id] / 2048));
        }
    }
    if (check >= 0 && check < 7 && blocks[check]) {
        ULONG tests[6], n, bad = 0, tried = 0;
        int i;
        tests[0] = 0; tests[1] = 1; tests[2] = 12345; tests[3] = blocks[check] / 2;
        tests[4] = blocks[check] - 2; tests[5] = blocks[check] - 1;
        for (i = 0; i < 6; i++) {
            ULONG k, ok = 1;
            n = tests[i];
            tried++;
            if (!read_block(check, n, buf)) {
                printf("  id %d block %lu: read FAILED\n", check, (unsigned long)n);
                bad++;
                continue;
            }
            for (k = 0; k < 512; k += 8)
                if (be32(buf + k) != n || memcmp(buf + k + 4, "BLK!", 4))
                    ok = 0;
            printf("  id %d block %lu: %s\n", check, (unsigned long)n, ok ? "right" : "WRONG");
            if (!ok) bad++;
        }
        printf("  pattern check: %lu of %lu blocks right\n", (unsigned long)(tried - bad), (unsigned long)tried);
    }
    REG(R_MR) = 0;
    REG(R_ICR) = 0;
    (void)SEL_IDE;                           /* the port is IDE again */
    if (rda) FreeArgs(rda);
    return 0;
}
