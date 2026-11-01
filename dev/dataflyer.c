/*
 * dataflyer.device - a driver for the Expansion Systems DataFlyer 1200 SCSI+.
 * Part of DataFlyerSCSI. GPL-3.0-only, see LICENSE.
 *
 * The board is an NCR 53C80 that shares the A1200's internal IDE (Gayle) port:
 * reading $DA1012 turns the port into the 53C80, reading $DA1016 back into the
 * IDE task file (docs/hardware.md). Every SCSI command runs inside Forbid()
 * with the port switched to SCSI only for that command, and the port is IDE
 * again before Permit(). The IDE side is driven by this driver too, once it
 * has taken it over from scsi.device (ide.c).
 *
 * Unit number = SCSI id (0-6) + 10 * LUN; 100 and 101 = the IDE port's master
 * and slave. One driver task runs the commands.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <exec/semaphores.h>
#include <devices/trackdisk.h>
#include <devices/timer.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>
#include <devices/scsidisk.h>
#include <devices/newstyle.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include "dataflyer.h"

struct ExecBase *SysBase;
extern const char df_devname[], df_idstring[];

/* ---- hardware ----------------------------------------------------------- */

#define BASE        0xDA0000UL
#define CHIP(n)     (*(volatile UBYTE *)(BASE + (n) * 4 + 2))

#define R_CSD  0
#define R_ICR  1
#define R_MR   2
#define R_TCR  3
#define R_CSBS 4
#define R_BSR  5
#define R_RPI  7

#define ICR_DBUS 0x01
#define ICR_ATN  0x02
#define ICR_SEL  0x04
#define ICR_ACK  0x10
#define ICR_RST  0x80
#define CSBS_REQ 0x20
#define CSBS_BSY 0x40

#define PH_DATA_OUT 0
#define PH_DATA_IN  1
#define PH_COMMAND  2
#define PH_STATUS   3
#define PH_MSG_OUT  6
#define PH_MSG_IN   7

#define OUR_ID 7
#define MAX_UNITS 77                 /* ids 0-6, LUNs 0-7: unit = id + 10 * lun, up to 76 */

/* timeouts, counted in CIA reads (~1.4 us each, the same on every CPU) */
#define T_SELECT   180000            /* 250 ms selection timeout */
#define T_REQ     7000000            /* 10 s for a target to ask for the next byte */
#define T_REQ_2M  84000000           /* 2 minutes: commands that work before they answer (req_budget) */
#define T_REQ_10M 420000000          /* 10 minutes: FORMAT UNIT, SANITIZE, ERASE */
#define T_ACK      140000            /* 200 ms */

/* the most transferred in one Forbid(): 64 KB, then the system gets a turn */
#define CHUNK      65536

/* ---- state -------------------------------------------------------------- */

struct Device *dfdev;
static BPTR seglist;
static struct DFUnit *units[MAX_UNITS];
static struct Task *task;
static struct MsgPort *cmdport;      /* the task's; NULL until it runs */
struct SignalSemaphore buslock;
static struct SignalSemaphore openlock;  /* df_open: one at a time */
static UBYTE sense_buf[32];
static UBYTE pio_only;                /* 1 = programmed I/O only, no pseudo-DMA (not switchable yet) */
static ULONG opts = 1;                /* DF_OPTS: bit 0 = writes in 256-byte runs (on; flystat OPTS=0 turns it off) */
#define DF_OPTS 0x7ff1

/* ---- small libc --------------------------------------------------------- */

void *memcpy(void *d, const void *s, unsigned long n)
{
    UBYTE *a = d; const UBYTE *b = s;
    while (n--) *a++ = *b++;
    return d;
}

void *memset(void *d, int c, unsigned long n)
{
    UBYTE *a = d;
    while (n--) *a++ = (UBYTE)c;
    return d;
}

struct DFStats stats;            /* DF_STATS (tools/flystat) */

static ULONG be32(const UBYTE *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }

#ifdef DFDEBUG
/* a debug build (make DEBUG=1) logs every request and SCSI command to the serial port */
static void dbg_c(char c)
{
    register struct ExecBase *a6 __asm("a6") = SysBase;
    register ULONG d0 __asm("d0") = (UBYTE)c;
    __asm volatile ("jsr -516(%%a6)" : "+r"(d0) : "r"(a6) : "d1", "a0", "a1", "cc", "memory");
}
static void dbg_s(const char *s) { while (*s) dbg_c(*s++); }
static void dbg_x(ULONG v, int n) { while (n--) dbg_c("0123456789abcdef"[(v >> (4 * n)) & 15]); }
#define DBG(x) do { x; } while (0)
#else
#define DBG(x) do { } while (0)
#endif

/* ---- waiting for the hardware -----------------------------------------
 * The card has no interrupt line to the CPU (docs/hardware.md), so the driver
 * polls. A wait spins for about a millisecond (CIA reads, ~1.4 us each), then
 * sleeps 250 us at a time on timer.device between looks, so the CPU is free
 * while a drive thinks or a ZuluSCSI commits a write, and a command is at
 * most a quarter of a millisecond late. Sleeping needs the
 * driver task's own timer request; in any other context (a unit probe from
 * OpenDevice) the wait keeps spinning. t is the whole budget in CIA reads. */
#define SPIN_POLLS 700                   /* ~1 ms before the first sleep */
#define SLEEP_US   250                   /* one sleep */
#define SLEEP_POLLS (SLEEP_US * 10 / 14) /* the same in CIA reads, for the budget */

static struct timerequest *sleep_tr;     /* the driver task's; NULL = no sleeping */
static struct Task *sleep_task;

/* one step of a wait: a CIA read while spinning, else a short sleep. Returns
 * how much of the budget it used, in CIA reads. */
static ULONG wait_step(ULONG spins)
{
    if (spins < SPIN_POLLS || !sleep_tr || FindTask(NULL) != sleep_task) {
        (void)CIA_PRA;
        return 1;
    }
    sleep_tr->tr_node.io_Command = TR_ADDREQUEST;
    sleep_tr->tr_time.tv_secs = 0;
    sleep_tr->tr_time.tv_micro = SLEEP_US;
    DoIO((struct IORequest *)sleep_tr);
    stats.sleeps++;
    return SLEEP_POLLS;
}

int df_pause(ULONG spins) { return (int)wait_step(spins); }

/* ---- the 53C80 ---------------------------------------------------------- */

static int wait_set(int reg, UBYTE mask, ULONG t, int site)
{
    ULONG n = 0, used = 0;
    while (used < t) {
        if (CHIP(reg) & mask) { stats.polls[site] += n; return 1; }
        used += wait_step(n);
        n++;
    }
    stats.polls[site] += n;
    stats.timeouts++;
    return 0;
}

static int wait_clear(int reg, UBYTE mask, ULONG t, int site)
{
    ULONG n = 0, used = 0;
    while (used < t) {
        if (!(CHIP(reg) & mask)) { stats.polls[site] += n; return 1; }
        used += wait_step(n);
        n++;
    }
    stats.polls[site] += n;
    stats.timeouts++;
    return 0;
}

/* ICR_ATN while a command is being aborted: kept up through every handshake */
static UBYTE atn;

static void chip_idle(void)
{
    atn = 0;
    CHIP(R_MR) = 0;
    CHIP(R_ICR) = 0;
    CHIP(R_TCR) = 0;
    (void)CHIP(R_RPI);
}

static int select_target(int id)
{
    chip_idle();
    if (CHIP(R_CSBS) & CSBS_BSY) {           /* a hung target: let it go */
        if (!wait_clear(R_CSBS, CSBS_BSY, T_ACK, W_BUSFREE))
            return 0;
    }
    CHIP(R_CSD) = (UBYTE)((1 << id) | (1 << OUR_ID));
    CHIP(R_ICR) = ICR_DBUS;
    CHIP(R_ICR) = ICR_DBUS | ICR_SEL;
    if (!wait_set(R_CSBS, CSBS_BSY, T_SELECT, W_SELECT)) {
        CHIP(R_ICR) = 0;
        return 0;
    }
    CHIP(R_ICR) = 0;
    return 1;
}

static int put_byte(UBYTE b)
{
    CHIP(R_CSD) = b;
    CHIP(R_ICR) = atn | ICR_DBUS;
    CHIP(R_ICR) = atn | ICR_DBUS | ICR_ACK;
    if (!wait_clear(R_CSBS, CSBS_REQ, T_ACK, W_ACK))
        return 0;
    CHIP(R_ICR) = atn;
    return 1;
}

static int get_byte(UBYTE *b)
{
    *b = CHIP(R_CSD);
    CHIP(R_ICR) = atn | ICR_ACK;
    if (!wait_clear(R_CSBS, CSBS_REQ, T_ACK, W_ACK))
        return 0;
    CHIP(R_ICR) = atn;
    return 1;
}

/* RST for 25 us or more, then the reset-to-selection time (250 ms). The
 * only way to get a target out of a DATA OUT phase without handing it a
 * byte: it waits for every byte it asked for. Every target reports a UNIT
 * ATTENTION afterwards (rw sends a chunk once more for that). */
static void bus_reset(void)
{
    ULONG n = 0, used = 0;
    int k;
    CHIP(R_MR) = 0;
    CHIP(R_TCR) = 0;
    CHIP(R_ICR) = ICR_RST;
    for (k = 0; k < 40; k++) (void)CIA_PRA;     /* ~56 us */
    chip_idle();
    while (used < T_SELECT) { used += wait_step(n); n++; }
}

/* ---- pseudo-DMA: the data phases through the port at $DA1002 -------------
 * As ExpXDS does it: mode = DMA, start the transfer, then per byte wait for
 * DMA REQUEST (bus and status bit 6) and move one byte through the port; the
 * chip does the REQ/ACK handshake. When the target leaves the phase early,
 * DMA REQUEST stops and phase match drops: the loop ends there and the
 * command goes on in the new phase. */
#define DMAPORT     (*(volatile UBYTE *)0xDA1002UL)
#define MR_DMA      0x02
#define BSR_DRQ     0x40
#define BSR_PHASE   0x08
#define R_SDS  5    /* write: start DMA send             */
#define R_SDIR 7    /* write: start DMA initiator receive */

/* the REQ-wait budget of the command running (req_budget), and whether a
 * data phase ran out of it: the command is over then (the bus gets a reset) */
static ULONG t_req = T_REQ;
static UBYTE dma_timeout;

/* DMA REQUEST not up yet: a short spin first (the next byte is usually a
 * few register reads away), then the timed wait. 1 = DRQ, -1 = the phase
 * changed (the target moved on), 0 = timeout. */
static int drq_wait(int site)
{
    ULONG t, n = 0;
    int k;
    for (k = 0; k < 64; k++) {
        UBYTE b = CHIP(R_BSR);
        if (!(b & BSR_PHASE)) return -1;     /* first: DMA REQUEST can stay up after the phase changed */
        if (b & BSR_DRQ) return 1;
    }
    for (t = 0; t < t_req; ) {
        UBYTE b = CHIP(R_BSR);
        if (!(b & BSR_PHASE)) { stats.polls[site] += n; return -1; }
        if (b & BSR_DRQ) { stats.polls[site] += n; return 1; }
        t += wait_step(n);
        n++;
    }
    stats.polls[site] += n;
    stats.timeouts++;
    return 0;
}

/* one burst, with interrupts off a block at a time (512 bytes, ~1 ms on a
 * 68030): an interrupt in the middle of the REQ/ACK handshake stalls the
 * drive; ExpXDS turns interrupts off around its transfers too. Returns what
 * moved; it stops early when the chip is not ready (the caller waits). */
#define IRQ_BLOCK 512
/* runs of 256 bytes per DMA REQUEST test (df_dmain, and df_dmaout_run with
 * DF_OPTS bit 0) only when the target is certain to deliver every byte of
 * the run (block_run): a blind read of a byte that never comes holds the
 * CPU on the bus cycle with interrupts off - the whole machine freezes,
 * nothing can time out. Everything else goes a byte at a time, DMA
 * REQUEST tested before each (bounded: the loop ends at the first byte
 * the chip is not ready for). */
static UBYTE blockxfer;

/* whole blocks of a plain READ/WRITE (6/10/12/16) to a disk-like unit
 * (direct access, CD-ROM, optical, simplified direct access) whose block
 * size is known and a multiple of 256, with the buffer exactly the CDB's
 * blocks x the block size. A tape's READ(6) counts bytes (variable
 * records), a scanner's READ(10) ends short, a 520- or 2352-byte block
 * ends inside a run, a scsi_Length past the command's data waits for
 * bytes that never come: all of those take the byte path. */
static int block_run(const struct DFUnit *un, const UBYTE *cdb, int cdblen, ULONG len)
{
    ULONG nblk;
    if (!un->bsize || (un->bsize & 255) || !len)
        return 0;
    if (un->devtype != 0 && un->devtype != 5 && un->devtype != 7 && un->devtype != 0x0e)
        return 0;
    switch (cdb[0]) {
    case 0x08: case 0x0a:                                /* READ/WRITE(6): 0 = 256 blocks */
        if (cdblen < 6) return 0;
        nblk = cdb[4] ? cdb[4] : 256;
        break;
    case 0x28: case 0x2a:
        if (cdblen < 10) return 0;
        nblk = ((ULONG)cdb[7] << 8) | cdb[8];
        break;
    case 0xa8: case 0xaa:
        if (cdblen < 12) return 0;
        nblk = be32(cdb + 6);
        break;
    case 0x88: case 0x8a:
        if (cdblen < 16) return 0;
        nblk = be32(cdb + 10);
        break;
    default:
        return 0;
    }
    return nblk <= 0xffffffffUL / un->bsize && nblk * un->bsize == len;
}

static ULONG burst(UBYTE *p, ULONG n, int out)
{
    ULONG done = 0;
    int runs = blockxfer && !(n & 255);              /* what is left ends on a run boundary */
    while (done < n) {
        ULONG want = n - done, got;
        if (want > IRQ_BLOCK) want = IRQ_BLOCK;
        Disable();
        if (out)
            got = (runs && (opts & 1)) ? df_dmaout_run(p + done, want) : df_dmaout(p + done, want);
        else
            got = runs ? df_dmain(p + done, want) : df_dmain_byte(p + done, want);
        Enable();
        done += got;
        if (got < want) break;
    }
    return done;
}

static ULONG dma_in(UBYTE *p, ULONG n)
{
    ULONG i = 0;
    CHIP(R_MR) = MR_DMA;
    CHIP(R_SDIR) = 0;
    while (i < n) {
        int r;
        i += burst(p + i, n - i, 0);             /* the burst, in start.S */
        if (i >= n) break;
        r = drq_wait(W_DMAIN);
        if (r < 0) goto done;                    /* the target moved on */
        if (!r) { dma_timeout = 1; break; }
    }
done:
    CHIP(R_MR) = 0;
    (void)CHIP(R_RPI);
    return i;
}

static ULONG dma_out(const UBYTE *p, ULONG n)
{
    ULONG i = 0;
    CHIP(R_ICR) = ICR_DBUS;
    CHIP(R_MR) = MR_DMA;
    CHIP(R_SDS) = 0;
    while (i < n) {
        int r;
        i += burst((UBYTE *)p + i, n - i, 1);
        if (i >= n) break;
        r = drq_wait(W_DMAOUT);
        if (r < 0) goto done;
        if (!r) { dma_timeout = 1; goto done; }  /* the chip is not ready: no extra byte into it */
    }
    /* ExpXDS writes one more byte here: it completes the last handshake on
     * the card; once the phase has changed the chip ignores it */
    DMAPORT = 0;
done:
    CHIP(R_ICR) = 0;
    CHIP(R_MR) = 0;
    (void)CHIP(R_RPI);
    return i;
}

/* an abort drains at most this much DATA IN while it waits for MESSAGE OUT,
 * and feeds a DATA OUT at most this many bytes */
#define T_DRAIN 65536
#define T_FEED  16

/* How long a target may take to ask for the next byte: 10 s, except for
 * the commands that do their work before they answer - a FORMAT UNIT or
 * SANITIZE runs for minutes, a START (IMMED=0) on an old full-height drive
 * half a minute, a VERIFY or SYNCHRONIZE CACHE as long as the disk takes.
 * The 10 s limit failed those and left the target holding the bus. */
static ULONG req_budget(UBYTE op)
{
    switch (op) {
    case 0x04: case 0x48: case 0x19:                    /* FORMAT UNIT, SANITIZE, ERASE (tape) */
        return T_REQ_10M;
    case 0x41: case 0x93:                               /* WRITE SAME (10/16) */
    case 0x1d: case 0x1b:                               /* SEND DIAGNOSTIC, START STOP UNIT / LOAD UNLOAD */
    case 0x2f: case 0x8f:                               /* VERIFY (10/16) */
    case 0x35: case 0x91:                               /* SYNCHRONIZE CACHE (10/16) */
    case 0x01: case 0x11:                               /* REWIND, SPACE (tape) */
        return T_REQ_2M;
    }
    return T_REQ;
}

/* one SCSI command; the caller holds the bus (Forbid + SCSI selected).
 * Returns the status byte, -1 (no target answers the selection), DF_PHASE
 * (the target's data phase went against the caller's direction or past its
 * buffer and the command was aborted; or the target stopped asking within
 * its budget / let the bus go without COMMAND COMPLETE - the bus was reset
 * if it still held it) or DF_BUS (a REQ/ACK handshake failed). *actual =
 * the bytes that really moved to or from the caller's buffer. */
static int run_cmd(int id, int lun, const UBYTE *cdb, int cdblen,
                   UBYTE *data, ULONG len, int out, ULONG *actual)
{
    int status = -1, ci = 0, abort = 0;
    ULONG di = 0, junk = 0, fed = 0;
    UBYTE c0 = cdb[0], c1 = cdb[1];

    *actual = 0;
    t_req = req_budget(c0);
    dma_timeout = 0;
    if (!select_target(id))
        return -1;
    for (;;) {
        UBYTE csbs, b;
        int phase;
        if (!wait_set(R_CSBS, CSBS_REQ, t_req, W_REQ)) {
            if (CHIP(R_CSBS) & CSBS_BSY) goto reset;    /* out of time with the target on the bus: free it */
            break;
        }
        csbs = CHIP(R_CSBS);
        if (!(csbs & CSBS_BSY))
            break;
        phase = (csbs >> 2) & 7;
        CHIP(R_TCR) = (UBYTE)phase;
        switch (phase) {
        case PH_COMMAND:
            /* SCSI-1 style LUN in byte 1 (the target is selected without IDENTIFY) -
             * for LUN > 0 and standard commands only: in a vendor command (group
             * 6/7, $C0-$FF) byte 1 is the vendor's own (the BlueSCSI/ZuluSCSI
             * Toolbox's file index: files from the 33rd on read as index & 31),
             * and on LUN 0 the caller's byte 1 goes as it is (bits 5-7 are
             * RDPROTECT, the SEND DIAGNOSTIC self-test code, ... in newer commands) */
            b = ci == 0 ? c0 : ci == 1 && lun && c0 < 0xc0 ? (UBYTE)((c1 & 0x1f) | (lun << 5))
              : ci < cdblen ? cdb[ci] : 0;
            if (!put_byte(b)) goto busfail;
            ci++;
            break;
        case PH_DATA_IN:
            /* the caller said write (and gave a buffer): the target's data is
             * not for it. ATN up, the bytes go to a bit bucket until the target
             * takes the ABORT message (or finishes); the request fails. */
            if (!abort && out && len) {
                abort = 1;
                atn = ICR_ATN;
                CHIP(R_ICR) = atn;
            }
            if (!abort && !out && di < len && !pio_only) {
                di += dma_in(data + di, len - di);
                if (dma_timeout) goto reset;
                break;
            }
            if (!get_byte(&b)) goto busfail;
            if (abort) {
                if (++junk > T_DRAIN) goto reset;
                break;
            }
            /* more than the buffer holds (an allocation length longer than
             * scsi_Length, as HDToolBox sends): dropped, as every Amiga SCSI
             * driver does - a read has no side effect, scsi_Actual = the buffer */
            if (di < len) data[di] = b;
            di++;
            break;
        case PH_DATA_OUT:
            /* only the caller's own bytes go out. The target wants data the
             * caller did not give (SCSIF_READ, or past scsi_Length; 1.0.4 sent
             * zeros, which a WRITE put on the disk). It can't leave the phase
             * without a byte, and ATN only counts after one: so first the SCSI
             * way - ATN up and one byte (0) under it, so the target can go to
             * MESSAGE OUT and take the ABORT (PH_MSG_OUT below): the whole
             * command is discarded, so that byte never reaches a disk. A
             * target that keeps asking for data with ATN up gets at most
             * T_FEED such bytes, then the bus is reset, as 1.0.5 did at once.
             * WinUAE's emulation ignores ATN in a data phase, so the reset is
             * the path it exercises; a real target should take the ABORT. */
            if (abort || !out || di >= len) {
                if (!abort) { abort = 1; atn = ICR_ATN; CHIP(R_ICR) = atn; }
                if (++fed > T_FEED) goto reset;
                if (!put_byte(0)) goto busfail;
                break;
            }
            if (!pio_only) {
                di += dma_out(data + di, len - di);
                if (dma_timeout) goto reset;
                break;
            }
            if (!put_byte(data[di])) goto busfail;
            di++;
            break;
        case PH_STATUS:
            if (atn) { atn = 0; CHIP(R_ICR) = 0; }   /* the data is over: nothing left to abort */
            if (!get_byte(&b)) goto busfail;
            status = b;
            break;
        case PH_MSG_IN:
            if (!get_byte(&b)) goto busfail;
            if (b == 0x00) {                 /* COMMAND COMPLETE */
                CHIP(R_TCR) = 0;
                wait_clear(R_CSBS, CSBS_BSY, T_ACK, W_BSYEND);
                *actual = di < len ? di : len;
                return abort ? DF_PHASE : status;
            }
            break;
        case PH_MSG_OUT:
            if (abort) {
                atn = 0;                         /* ATN off before the message byte's ACK */
                if (!put_byte(0x06)) goto busfail;   /* ABORT: the target goes bus free */
                CHIP(R_TCR) = 0;
                if (!wait_clear(R_CSBS, CSBS_BSY, T_ACK, W_BSYEND)) goto reset;
                chip_idle();
                *actual = di < len ? di : len;
                return DF_PHASE;
            }
            if (!put_byte(0x08)) goto busfail;   /* NOP */
            break;
        default:
            goto busfail;
        }
    }
    /* the target let the bus go without COMMAND COMPLETE: the status if one
     * came, else the command is lost (it was selected, so not "no target") */
    *actual = di < len ? di : len;
    chip_idle();
    return abort || status < 0 ? DF_PHASE : status;
busfail:
    /* the target never dropped REQ for our ACK: if it still holds the bus, free it */
    if (CHIP(R_CSBS) & CSBS_BSY) bus_reset(); else chip_idle();
    *actual = di < len ? di : len;
    return DF_BUS;
reset:
    bus_reset();
    *actual = di < len ? di : len;
    return DF_PHASE;
}


/* the bus for one command: the port is SCSI for its length, then IDE again.
 * No Forbid(): nothing but this driver touches the port (it owns scsi.device
 * too), and the waits may sleep. The byte bursts run with interrupts off. */
static int scsi_do(struct DFUnit *un, const UBYTE *cdb, int cdblen,
                   UBYTE *data, ULONG len, int out, ULONG *actual)
{
    int st, id = un->id, lun = un->lun;
    ObtainSemaphore(&buslock);
    stats.cmds++;
    SEL_SCSI();
    DBG({ int k; dbg_s("scsi "); dbg_x(id, 1); dbg_c(' ');
          for (k = 0; k < cdblen; k++) dbg_x(cdb[k], 2);
          dbg_s(" len "); dbg_x(len, 8); dbg_s(out ? " out\r\n" : " in\r\n"); });
    blockxfer = (UBYTE)block_run(un, cdb, cdblen, len);
    st = run_cmd(id, lun, cdb, cdblen, data, len, out, actual);
    DBG({ dbg_s("  -> "); dbg_x((ULONG)st, 8); dbg_s(" got "); dbg_x(*actual, 8); dbg_s("\r\n"); });
    chip_idle();
    SEL_IDE();
    ReleaseSemaphore(&buslock);
    return st;
}

/* ---- DF_IRQPROBE (0x7ff3): does the card's interrupt reach the CPU? -----
 * Servers on INT2 and INT6 count the 53C80's interrupt bit. With the port
 * on SCSI and interrupts ENABLED (no Forbid), a TEST UNIT READY is sent;
 * after the command bytes the chip is armed for a phase-mismatch interrupt
 * (DMA mode with the target-command register = the phase just left), and
 * the task waits for the counters instead of polling the bus. The counts,
 * the status seen in the handler and how long each wait took come back. */
static struct DFUnit *unit_of(struct IOStdReq *io);
extern void df_int2srv(void), df_int6srv(void);
extern UBYTE irq_on, irq_bsr, irq_gayle;
extern ULONG irq_cnt[2], irq_other;
struct IrqProbe { ULONG int2, int6, other, sel_polls, arm_polls, fired, bsr_at_fire, gayle_at_fire,
                  status, csbs_after, cmd_ok, bsr_task, g9000, g9800, late_polls, late_int,
                  monbsy_bsr, monbsy_int2, monbsy_g9000; };

static void irqprobe(struct IOStdReq *io)
{
    struct DFUnit *un = unit_of(io);
    struct IrqProbe *r = (struct IrqProbe *)io->io_Data;
    static struct Interrupt is2, is6;
    UBYTE cdb[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0 };   /* READ(10) block 0: the drive takes a while */
    int ci = 0, ok = 0, st = -1;
    ULONG n;

    if (!r || io->io_Length < sizeof(*r) || !un || un->ide) { io->io_Error = IOERR_BADLENGTH; return; }
    memset(r, 0, sizeof(*r));
    memset(&is2, 0, sizeof(is2)); memset(&is6, 0, sizeof(is6));
    is2.is_Node.ln_Type = is6.is_Node.ln_Type = NT_INTERRUPT;
    is2.is_Node.ln_Name = is6.is_Node.ln_Name = (char *)df_devname;
    is2.is_Node.ln_Pri = is6.is_Node.ln_Pri = 120;      /* before the system's own servers */
    is2.is_Code = df_int2srv; is6.is_Code = df_int6srv;
    irq_cnt[0] = irq_cnt[1] = irq_other = 0;
    ObtainSemaphore(&buslock);
    AddIntServer(INTB_PORTS, &is2);
    AddIntServer(INTB_EXTER, &is6);
    SEL_SCSI();                                      /* interrupts stay on: nothing else uses the port */
    irq_on = 1;
    if (select_target(un->id)) {
        r->sel_polls = stats.polls[W_SELECT];
        for (;;) {                                   /* the command phase, by hand */
            UBYTE csbs;
            int phase;
            if (!wait_set(R_CSBS, CSBS_REQ, T_REQ, W_REQ)) break;
            csbs = CHIP(R_CSBS);
            if (!(csbs & CSBS_BSY)) break;
            phase = (csbs >> 2) & 7;
            if (phase != PH_COMMAND) break;
            CHIP(R_TCR) = PH_COMMAND;
            if (!put_byte(ci == 1 ? (UBYTE)(un->lun << 5) : cdb[ci])) break;
            if (++ci == 10) { ok = 1; break; }
        }
        if (ok) {
            /* armed: DMA mode, TCR still COMMAND. When the target moves to
             * STATUS the chip sees a phase mismatch and raises its interrupt. */
            ULONG before = irq_cnt[0] + irq_cnt[1];
            UBYTE g9800_old = *(volatile UBYTE *)0xDA9800UL;
            /* the card's interrupt may ride the IDE interrupt line: let Gayle pass it */
            *(volatile UBYTE *)0xDA9800UL = g9800_old | 0x80;
            CHIP(R_MR) = MR_DMA;
            CHIP(R_SDS) = 0;                         /* a DMA send in the COMMAND phase: nothing moves, but a
                                                        transfer is "in progress", which the mismatch interrupt needs */
            for (n = 0; n < 2000000; n++) {          /* ~3 s */
                if (irq_cnt[0] + irq_cnt[1] != before) { r->fired = 1; break; }
                if (!(CHIP(R_BSR) & BSR_PHASE)) { r->fired = 2; break; }   /* mismatch seen, no interrupt */
                (void)CIA_PRA;
            }
            r->arm_polls = n;
            r->bsr_task = CHIP(R_BSR);                       /* the chip's own view: bit 4 = its interrupt line */
            r->g9000 = *(volatile UBYTE *)0xDA9000UL;        /* Gayle interrupt status */
            r->g9800 = *(volatile UBYTE *)0xDA9800UL;        /* Gayle interrupt enable */
            if (r->fired == 2) {                             /* phase changed: give an interrupt 3 ms more */
                for (n = 0; n < 2000; n++) {
                    if (irq_cnt[0] + irq_cnt[1] != before) { r->late_int = 1; break; }
                    (void)CIA_PRA;
                }
                r->late_polls = n;
            }
            r->bsr_at_fire = irq_bsr;
            r->gayle_at_fire = irq_gayle;
            CHIP(R_ICR) = 0;
            *(volatile UBYTE *)0xDA9800UL = g9800_old;
            CHIP(R_MR) = 0;
            (void)CHIP(R_RPI);
            r->csbs_after = CHIP(R_CSBS);
            /* finish the command the usual way: STATUS and MESSAGE IN by polling */
            for (;;) {
                UBYTE csbs, b;
                int phase;
                if (!wait_set(R_CSBS, CSBS_REQ, T_REQ, W_REQ)) break;
                csbs = CHIP(R_CSBS);
                if (!(csbs & CSBS_BSY)) break;
                phase = (csbs >> 2) & 7;
                CHIP(R_TCR) = (UBYTE)phase;
                if (phase == PH_STATUS) { if (!get_byte(&b)) break; st = b; }
                else if (phase == PH_MSG_IN) { if (!get_byte(&b)) break; if (b == 0) { CHIP(R_TCR) = 0; wait_clear(R_CSBS, CSBS_BSY, T_ACK, W_BSYEND); r->cmd_ok = 1; break; } }
                else if (phase == PH_DATA_IN) { if (!get_byte(&b)) break; }
                else break;
            }
        }
    }
    chip_idle();
    /* can the chip's interrupt latch set at all? Monitor-busy with the bus
     * free = an immediate "loss of BSY" interrupt on any 5380 */
    {
        ULONG before = irq_cnt[0] + irq_cnt[1];
        CHIP(R_MR) = 0x04;                           /* MR: monitor busy */
        for (n = 0; n < 200; n++) (void)CIA_PRA;     /* 0.3 ms */
        r->monbsy_bsr = CHIP(R_BSR);
        r->monbsy_g9000 = *(volatile UBYTE *)0xDA9000UL;
        r->monbsy_int2 = irq_cnt[0] + irq_cnt[1] - before;
        CHIP(R_MR) = 0;
        (void)CHIP(R_RPI);
    }
    irq_on = 0;
    SEL_IDE();
    RemIntServer(INTB_PORTS, &is2);
    RemIntServer(INTB_EXTER, &is6);
    ReleaseSemaphore(&buslock);
    r->int2 = irq_cnt[0]; r->int6 = irq_cnt[1]; r->other = irq_other;
    r->status = (ULONG)st;
    io->io_Actual = sizeof(*r);
}

/* a command for a unit: SCSI on the card, or a drive on the IDE port */
static int unit_cmd(struct DFUnit *un, const UBYTE *cdb, int cdblen, UBYTE *data, ULONG len, int out,
                    ULONG *actual)
{
    if (un->ide)
        return ide_cmd(un, cdb, cdblen, data, len, out, actual);
    return scsi_do(un, cdb, cdblen, data, len, out, actual);
}

static int request_sense(struct DFUnit *un, UBYTE *buf, int len, ULONG *got)
{
    UBYTE cdb[6] = { 0x03, 0, 0, 0, (UBYTE)len, 0 };
    return unit_cmd(un, cdb, 6, buf, len, 0, got);
}

/* INQUIRY + READ CAPACITY (a drive still spinning up gets a few tries).
 * 1 = there, 0 = no such LUN, -1 = no target answers at that id */
static int unit_probe(struct DFUnit *un)
{
    static UBYTE inq[36];
    UBYTE cdb[10], cap[8];
    ULONG got;
    int i, st;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12; cdb[4] = 36;
    st = unit_cmd(un, cdb, 6, inq, 36, 0, &got);
    if (st == -1 && !un->ide)
        return -1;                                    /* no target at that id */
    if (st < 0 || got < 5 || (inq[0] & 0xe0))        /* no device, or no LUN there */
        return 0;
    un->devtype = inq[0] & 0x1f;
    un->removable = (inq[1] & 0x80) != 0;
    /* write protection (TD_PROTSTATUS): bit 7 of byte 2 of the MODE SENSE(6)
     * header - page $3F, or page 0 for a drive that refuses "all pages"; a
     * drive without MODE SENSE at all counts as writable, a CD-ROM never is.
     * An ATA disk has no such bit (the translation says 0: no WP there
     * unless a write fails with it, ide.c ata_fail). */
    un->wp = un->devtype == 5;
    if (!un->ide || un->atapi) {
        UBYTE hdr[4];
        for (i = 0; i < 2; i++) {
            memset(cdb, 0, sizeof(cdb));
            cdb[0] = 0x1a; cdb[2] = i ? 0x00 : 0x3f; cdb[4] = 4;
            st = unit_cmd(un, cdb, 6, hdr, 4, 0, &got);
            if (st == 0 && got >= 3) { if (hdr[2] & 0x80) un->wp = 1; break; }
            if (st == 2) request_sense(un, sense_buf, 18, &got);
        }
    }
    for (i = 0; i < 3; i++) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x25;
        st = unit_cmd(un, cdb, 10, cap, 8, 0, &got);
        if (st == 0 && got == 8) {
            un->blocks = be32(cap) + 1;
            un->bsize = be32(cap + 4);
            if (!un->bsize) un->bsize = 512;
            return 1;
        }
        if (st == 2)
            request_sense(un, sense_buf, 18, &got);     /* clears a unit attention */
    }
    /* there, but no medium / not ready: the block size stays unknown (0)
     * until a READ CAPACITY works - rw probes again on every request until
     * then, and a medium change clears it (fire_change). 1.0.4 guessed 512
     * here and never asked again, so a CD still spinning up at the open
     * was read at 512-byte blocks for good: wrong sectors, status GOOD. */
    un->blocks = 0;
    un->bsize = 0;
    return 1;
}

/* ---- block transfers ---------------------------------------------------- */

/* the unit of a request: ours, or (through scsi.device) a unit it had open before the takeover */
static struct DFUnit *unit_of(struct IOStdReq *io)
{
    if (df_ours(io->io_Unit)) return (struct DFUnit *)io->io_Unit;
    return ide_mapped(io->io_Unit);
}

static void reply(struct IOStdReq *io)
{
    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg(&io->io_Message);
}

/* io_Offset (+ io_Actual as the high 32 bits for 64-bit commands) bytes */
static void count_size(ULONG len)
{
    static const ULONG lim[7] = { 512, 2048, 8192, 32768, 65536, 131072, 524288 };
    int i = 0;
    while (i < 7 && len > lim[i]) i++;
    stats.size[i]++;
}

static void fire_change(struct DFUnit *un);

/* the io_Error for a READ/WRITE(10) that ended with CHECK CONDITION, from
 * the sense key. A failed write is write protection only when the drive
 * says DATA PROTECT; a failed read a bad sector only on MEDIUM ERROR. */
static BYTE sense_error(struct DFUnit *un)
{
    switch (sense_buf[2] & 0x0f) {
    case 0x02:                                    /* NOT READY: no medium, or still spinning up */
        return un->removable || sense_buf[12] == 0x3a ? TDERR_DiskChanged : TDERR_NotSpecified;
    case 0x03: return TDERR_BadSecSum;            /* MEDIUM ERROR */
    case 0x05:                                    /* ILLEGAL REQUEST: LBA out of range */
        return sense_buf[12] == 0x21 ? TDERR_SeekError : TDERR_NotSpecified;
    case 0x06: return TDERR_DiskChanged;          /* UNIT ATTENTION */
    case 0x07: return TDERR_WriteProt;            /* DATA PROTECT */
    }
    return TDERR_NotSpecified;
}

static void rw(struct IOStdReq *io, int out, int is64)
{
    struct DFUnit *un = unit_of(io);
    ULONG hi = is64 ? io->io_Actual : 0, lo = io->io_Offset, len = io->io_Length, done = 0;
    ULONG bs, lba, nblk;
    UBYTE *data = io->io_Data;
    int retried = 0;

    if (out) { stats.writes++; stats.wbytes += len; } else { stats.reads++; stats.rbytes += len; }
    count_size(len);
    io->io_Actual = 0;
    if (!un->bsize && unit_probe(un) <= 0) { io->io_Error = TDERR_DiskChanged; return; }
    if (!un->bsize) {                            /* still no capacity: no medium, or not ready yet */
        io->io_Error = un->removable ? TDERR_DiskChanged : TDERR_NotSpecified;
        return;
    }
    bs = un->bsize;
    if (len % bs) { io->io_Error = IOERR_BADLENGTH; return; }
    /* byte offset -> block, 64-bit: a shift for the usual power-of-two
     * sizes, a division for the rest (520-byte array disks, 2352-byte CD
     * sectors - the shift's floor(log2) put those on the wrong block).
     * Whole blocks only, and the block must fit 32 bits. */
    if (bs & (bs - 1)) {
        unsigned long long off = ((unsigned long long)hi << 32) | lo, b = off / bs;
        if (off % bs) { io->io_Error = IOERR_BADLENGTH; return; }
        if (b >> 32) { io->io_Error = IOERR_BADADDRESS; return; }
        lba = (ULONG)b;
    } else {
        int sh = 0;
        ULONG b = bs;
        while (b > 1) { b >>= 1; sh++; }
        if (lo % bs) { io->io_Error = IOERR_BADLENGTH; return; }
        lba = (lo >> sh) | (sh ? hi << (32 - sh) : 0);
        if (sh < 32 && (hi >> sh)) { io->io_Error = IOERR_BADADDRESS; return; }
    }
    /* past the end: refused before anything is sent (the drive's own
     * ILLEGAL REQUEST came back as a bad sector / write protection) */
    if (un->blocks && len && (lba >= un->blocks || len / bs > un->blocks - lba)) {
        io->io_Error = TDERR_SeekError;
        return;
    }
    while (done < len) {
        UBYTE cdb[10];
        ULONG want = len - done, got;
        int st;
        if (want > CHUNK && !(un->ide && !un->atapi)) want = CHUNK;   /* an IDE disk has no Forbid() */
        nblk = want / bs;
        if (nblk > 0xffff) nblk = 0xffff;
        want = nblk * bs;                        /* whole blocks: a chunk of 64 KB is not one on a 520-byte disk */
        cdb[0] = out ? 0x2a : 0x28; cdb[1] = 0;
        cdb[2] = (UBYTE)(lba >> 24); cdb[3] = (UBYTE)(lba >> 16); cdb[4] = (UBYTE)(lba >> 8); cdb[5] = (UBYTE)lba;
        cdb[6] = 0; cdb[7] = (UBYTE)(nblk >> 8); cdb[8] = (UBYTE)nblk; cdb[9] = 0;
        st = unit_cmd(un, cdb, 10, data + done, want, out, &got);
        if (st != 0) {
            io->io_Error = TDERR_NotSpecified;       /* no target, a bus error, BUSY, RESERVATION CONFLICT, ... */
            if (st == 2) {
                memset(sense_buf, 0, sizeof(sense_buf));
                if (request_sense(un, sense_buf, 18, &got) == 0)
                    io->io_Error = sense_error(un);
                if ((sense_buf[2] & 0x0f) == 0x06) {
                    /* UNIT ATTENTION: a reset or a power glitch on a fixed disk -
                     * send the chunk once more. A removable medium may have been
                     * swapped: tell the change interrupts (the attention is used
                     * up now, so the poller would not see it) and fail. */
                    if (un->removable) { fire_change(un); un->medium = 1; }
                    else if (!retried) { retried = 1; io->io_Error = 0; continue; }   /* a success after the retry is a success */
                }
            }
            io->io_Actual = done;
            return;
        }
        done += got;
        lba += nblk;
        if (got < want) { io->io_Error = TDERR_NotSpecified; break; }
    }
    io->io_Actual = done;
}

/* HD_SCSICMD: a SCSICmd from a program (HDToolBox, CD-ROM file systems) */
static void scsicmd(struct IOStdReq *io)
{
    struct DFUnit *un = unit_of(io);
    struct SCSICmd *sc = (struct SCSICmd *)io->io_Data;
    ULONG got = 0;
    int st;

    if (!sc || io->io_Length < sizeof(struct SCSICmd) || sc->scsi_CmdLength > 16) {
        io->io_Error = IOERR_BADLENGTH;
        return;
    }
    st = unit_cmd(un, sc->scsi_Command, sc->scsi_CmdLength, (UBYTE *)sc->scsi_Data,
                 sc->scsi_Length, !(sc->scsi_Flags & SCSIF_READ), &got);
    sc->scsi_Actual = got;
    sc->scsi_CmdActual = sc->scsi_CmdLength;
    sc->scsi_SenseActual = 0;
    io->io_Actual = sizeof(struct SCSICmd);
    if (st < 0) {
        /* no target answered the selection; the data phase went wrong, the
         * target ran out of time or lost the bus; the port's handshake failed */
        sc->scsi_Status = 0;
        io->io_Error = st == DF_PHASE ? HFERR_Phase : st == DF_BUS ? HFERR_DMA : HFERR_SelTimeout;
        return;
    }
    sc->scsi_Status = (UBYTE)st;
    if (st != 0) {
        io->io_Error = HFERR_BadStatus;
        if (st == 2 && (sc->scsi_Flags & SCSIF_AUTOSENSE) && sc->scsi_SenseData && sc->scsi_SenseLength) {
            ULONG sg = 0;
            int n = sc->scsi_SenseLength > 255 ? 255 : sc->scsi_SenseLength;
            if (request_sense(un, sc->scsi_SenseData, n, &sg) == 0)
                sc->scsi_SenseActual = (UWORD)sg;
        }
    }
}

static void geometry(struct IOStdReq *io)
{
    struct DFUnit *un = unit_of(io);
    struct DriveGeometry *dg = (struct DriveGeometry *)io->io_Data;
    if (io->io_Length < sizeof(struct DriveGeometry)) { io->io_Error = IOERR_BADLENGTH; return; }
    if (!un->bsize || !un->blocks) unit_probe(un);
    memset(dg, 0, sizeof(*dg));
    dg->dg_SectorSize = un->bsize ? un->bsize : 512;     /* no medium: a size for the caller's sums only (0 sectors) */
    dg->dg_TotalSectors = un->blocks;
    if (un->ide && !un->atapi) {
        dg->dg_Heads = un->heads;
        dg->dg_TrackSectors = un->secs;
        dg->dg_CylSectors = (ULONG)un->heads * un->secs;
        dg->dg_Cylinders = un->blocks / dg->dg_CylSectors;
    } else {
        dg->dg_Heads = 1;
        dg->dg_TrackSectors = 32;
        dg->dg_CylSectors = 32;
        dg->dg_Cylinders = un->blocks / 32;
    }
    dg->dg_BufMemType = MEMF_PUBLIC;
    dg->dg_DeviceType = un->devtype;
    dg->dg_Flags = un->removable ? DGF_REMOVABLE : 0;
    io->io_Actual = sizeof(struct DriveGeometry);
}

static const UWORD supported[] = {
    CMD_RESET, CMD_READ, CMD_WRITE, CMD_UPDATE, CMD_CLEAR, CMD_STOP, CMD_START, CMD_FLUSH,
    TD_MOTOR, TD_SEEK, TD_FORMAT, TD_REMOVE, TD_CHANGENUM, TD_CHANGESTATE, TD_PROTSTATUS,
    TD_GETDRIVETYPE, TD_GETNUMTRACKS, TD_ADDCHANGEINT, TD_REMCHANGEINT, TD_GETGEOMETRY,
    TD_EJECT, TD_READ64, TD_WRITE64, TD_SEEK64, TD_FORMAT64, HD_SCSICMD,
    NSCMD_DEVICEQUERY, NSCMD_TD_READ64, NSCMD_TD_WRITE64, NSCMD_TD_SEEK64, NSCMD_TD_FORMAT64, 0
};

/* commands that need the bus: run in the driver task */
static int needs_task(UWORD cmd)
{
    switch (cmd) {
    case CMD_READ: case CMD_WRITE: case TD_FORMAT: case TD_READ64: case TD_WRITE64: case TD_FORMAT64:
    case NSCMD_TD_READ64: case NSCMD_TD_WRITE64: case NSCMD_TD_FORMAT64: case HD_SCSICMD:
    case TD_GETGEOMETRY: case TD_CHANGESTATE: case TD_EJECT: case TD_MOTOR: case CMD_UPDATE:
    case CMD_RESET: case 0x7ff3:
    case CMD_FLUSH:         /* nothing to do, but it must not be answered
                             * ahead of the requests queued before it
                             * (devsoak's flush-gate caught it as quick) */
        return 1;
    }
    return 0;
}

static int medium_in(struct DFUnit *un);

static void perform(struct IOStdReq *io)
{
    struct DFUnit *un = unit_of(io);
    io->io_Error = 0;
    if (!un && (io->io_Command < DF_STATS || io->io_Command > 0x7ff2)) {  /* a scsi.device unit with no drive behind it */
        io->io_Error = TDERR_BadUnitNum;
        reply(io);
        return;
    }
    DBG({ dbg_s("io "); dbg_x(io->io_Command, 4); dbg_s(" len "); dbg_x(io->io_Length, 8);
          dbg_s(" unit "); dbg_x((ULONG)un, 8); dbg_s("\r\n"); });
    switch (io->io_Command) {
    case CMD_READ:          rw(io, 0, 0); break;
    case CMD_WRITE:
    case TD_FORMAT:         rw(io, 1, 0); break;
    case TD_READ64:
    case NSCMD_TD_READ64:   rw(io, 0, 1); break;
    case TD_WRITE64:
    case TD_FORMAT64:
    case NSCMD_TD_WRITE64:
    case NSCMD_TD_FORMAT64: rw(io, 1, 1); break;
    case HD_SCSICMD:        scsicmd(io); break;
    case TD_GETGEOMETRY:    geometry(io); break;
    case TD_CHANGESTATE:
        io->io_Actual = medium_in(un) ? 0 : 1;  /* 0 = a disk is in */
        break;
    case TD_EJECT: {
        UBYTE cdb[6] = { 0x1b, 0, 0, 0, (UBYTE)(io->io_Length ? 2 : 3), 0 };   /* START STOP UNIT */
        ULONG got;
        if (unit_cmd(un, cdb, 6, NULL, 0, 0, &got) != 0) io->io_Error = TDERR_NotSpecified;
        break;
    }
    case TD_MOTOR:
        io->io_Actual = 1;
        break;
    case CMD_UPDATE: {
        /* the drive's write cache, if it has one: SYNCHRONIZE CACHE (an ATA
         * disk: FLUSH CACHE when IDENTIFY lists it, ide.c). Removable units
         * too - CF cards and Microdrives say removable, and a Microdrive
         * has a real write cache; only a unit known to have no medium is
         * skipped. A drive without the command answers ILLEGAL REQUEST:
         * nothing to flush, the sense is cleared. */
        UBYTE cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        ULONG got;
        if (!(un->removable && un->medium == 2) && unit_cmd(un, cdb, 10, NULL, 0, 0, &got) == 2)
            request_sense(un, sense_buf, 18, &got);
        io->io_Actual = 0;
        break;
    }
    case NSCMD_DEVICEQUERY: {
        struct NSDeviceQueryResult *q = (struct NSDeviceQueryResult *)io->io_Data;
        if (!q || io->io_Length < 16) { io->io_Error = IOERR_BADLENGTH; break; }
        q->nsdqr_DevQueryFormat = 0;
        q->nsdqr_SizeAvailable = 16;
        q->nsdqr_DeviceType = NSDEVTYPE_TRACKDISK;
        q->nsdqr_DeviceSubType = 0;
        q->nsdqr_SupportedCommands = (UWORD *)supported;
        io->io_Actual = 16;
        break;
    }
    case CMD_RESET:
        /* the way out of a hung bus a program has (HDToolBox, a file system
         * after errors): RST on the SCSI bus, and the unit's cached state goes
         * so the next request reads the capacity again. Every target on the
         * bus reports a UNIT ATTENTION afterwards (rw retries for it). The
         * IDE port has no reset here yet (audit L6). */
        if (!un->ide) {
            ObtainSemaphore(&buslock);
            SEL_SCSI();
            bus_reset();
            SEL_IDE();
            ReleaseSemaphore(&buslock);
        }
        if (!un->ide || un->atapi) un->bsize = 0;
        io->io_Actual = 0;
        break;
    case CMD_CLEAR: case CMD_STOP: case CMD_START: case CMD_FLUSH:
    case TD_SEEK: case TD_SEEK64: case NSCMD_TD_SEEK64: case TD_REMOVE:
        io->io_Actual = 0;
        break;
    case TD_ADDCHANGEINT:
        /* the request stays with us (unanswered) until TD_REMCHANGEINT or
         * AbortIO; the task Cause()s its interrupt when the medium changes */
        Forbid();
        AddTail(&un->chgints, &io->io_Message.mn_Node);
        Permit();
        return;
    case TD_REMCHANGEINT: {
        /* the request that added this interrupt is answered and goes */
        struct Node *n, *next;
        Forbid();
        for (n = un->chgints.lh_Head; (next = n->ln_Succ); n = next) {
            struct IOStdReq *add = (struct IOStdReq *)n;
            if (add->io_Data == io->io_Data) {
                Remove(n);
                if (add != io) { add->io_Error = 0; ReplyMsg(&add->io_Message); }
                break;
            }
        }
        Permit();
        io->io_Actual = 0;
        break;
    }
    case TD_CHANGENUM:      io->io_Actual = un->changes; break;
    case TD_PROTSTATUS:     io->io_Actual = un->wp; break;      /* from the last probe (every medium change probes again) */
    case TD_GETDRIVETYPE:   io->io_Actual = DRIVE3_5; break;
    case TD_GETNUMTRACKS:   io->io_Actual = un->blocks / 32; break;
    case 0x7ff3:            /* DF_IRQPROBE */
        irqprobe(io);
        break;
    case 0x7ff2:            /* DF_DIAG */
        io->io_Actual = ide_diag(io->io_Data, io->io_Length);
        break;
    case DF_OPTS:           /* io_Offset = the new options; io_Actual = the old ones */
        io->io_Actual = opts;
        opts = io->io_Offset;
        break;
    case DF_STATS:          /* io_Length 0: read and clear */
        if (io->io_Data && io->io_Length >= sizeof(stats)) {
            memcpy(io->io_Data, &stats, sizeof(stats));
            io->io_Actual = sizeof(stats);
        }
        memset(&stats, 0, sizeof(stats));
        break;
    default:                io->io_Error = IOERR_NOCMD; break;
    }
    reply(io);
}

/* ---- the driver task ---------------------------------------------------- */

static struct Task *opener_task;        /* signalled when the task is up */

/* TEST UNIT READY: 1 = a medium is in. A UNIT ATTENTION (the medium may
 * have changed, a reset) counts as a change too. *changed is set when the
 * answer differs from the last one. */
static int medium_in(struct DFUnit *un)
{
    UBYTE cdb[6] = { 0, 0, 0, 0, 0, 0 };
    ULONG got;
    int st = unit_cmd(un, cdb, 6, NULL, 0, 0, &got), in, ua = 0, k;
    /* a drive may queue several UNIT ATTENTIONs for one event (a ZuluSCSI
     * does after a disc swap); each REQUEST SENSE clears one, so ask again
     * until the real answer comes */
    for (k = 0; st == 2 && k < 4; k++) {
        request_sense(un, sense_buf, 18, &got);
        if ((sense_buf[2] & 0x0f) != 0x06) break;
        ua = 1;
        st = unit_cmd(un, cdb, 6, NULL, 0, 0, &got);
    }
    in = st == 0;
    /* a change: the medium went or came, or the drive says it may have
     * changed. A drive can say that twice for one swap (a second attention
     * once the new disc is loaded), so an attention alone counts only when
     * nothing was reported in the last poll interval. */
    if ((UBYTE)(in ? 1 : 2) != un->medium || (ua && !un->quiet)) {
        if (un->medium || ua)                   /* not the first look */
            fire_change(un);
        un->medium = in ? 1 : 2;
    }
    return in;
}

/* a medium change: count it, read the capacity again, tell the change interrupts */
static void fire_change(struct DFUnit *un)
{
    struct Node *n;
    un->changes++;
    un->bsize = 0;                              /* READ CAPACITY again: a new disc */
    un->quiet = 2;
    Forbid();
    for (n = un->chgints.lh_Head; n->ln_Succ; n = n->ln_Succ)
        Cause((struct Interrupt *)((struct IOStdReq *)n)->io_Data);
    Permit();
}

/* every POLL_SECS: each removable unit somebody wants change news about */
#define POLL_SECS 2
static void poll_units(void)
{
    int i;
    for (i = -2; i < MAX_UNITS; i++) {
        struct DFUnit *un = i < 0 ? ide_units[i + 2] : units[i];
        if (un && un->quiet) un->quiet--;
        if (un && un->removable && un->chgints.lh_Head->ln_Succ)
            medium_in(un);
    }
}

static void task_main(void)
{
    struct MsgPort *port, *tport = NULL;
    struct timerequest *tr = NULL;
    struct IOStdReq *io;
    ULONG tmask = 0;
    BYTE sig = AllocSignal(-1);
    port = sig >= 0 ? (struct MsgPort *)AllocMem(sizeof(struct MsgPort), MEMF_PUBLIC | MEMF_CLEAR) : NULL;
    if (port) {
        port->mp_Node.ln_Type = NT_MSGPORT;
        port->mp_Flags = PA_SIGNAL;
        port->mp_SigBit = sig;
        port->mp_SigTask = FindTask(NULL);
        port->mp_MsgList.lh_Head = (struct Node *)&port->mp_MsgList.lh_Tail;
        port->mp_MsgList.lh_TailPred = (struct Node *)&port->mp_MsgList.lh_Head;
        /* the sleep timer for the waits (UNIT_MICROHZ, 1 ms steps) */
        {
            struct MsgPort *sp = CreateMsgPort();
            struct timerequest *st = sp ? (struct timerequest *)CreateIORequest(sp, sizeof(*st)) : NULL;
            if (st && OpenDevice((STRPTR)"timer.device", UNIT_MICROHZ, (struct IORequest *)st, 0) == 0) {
                sleep_task = FindTask(NULL);
                sleep_tr = st;
            }
        }
        /* the poll timer (no timer: no change notification, the rest works) */
        tport = CreateMsgPort();
        tr = tport ? (struct timerequest *)CreateIORequest(tport, sizeof(*tr)) : NULL;
        if (tr && OpenDevice((STRPTR)"timer.device", UNIT_VBLANK, (struct IORequest *)tr, 0) == 0) {
            tmask = 1UL << tport->mp_SigBit;
            tr->tr_node.io_Command = TR_ADDREQUEST;
            tr->tr_time.tv_secs = POLL_SECS;
            tr->tr_time.tv_micro = 0;
            SendIO((struct IORequest *)tr);
        } else
            tr = NULL;
    }
    cmdport = port;
    Signal(opener_task, SIGF_SINGLE);
    if (!port)
        return;
    for (;;) {
        ULONG got = Wait((1UL << sig) | tmask);
        if (tr && (got & tmask) && GetMsg(tport)) {
            poll_units();
            tr->tr_node.io_Command = TR_ADDREQUEST;
            tr->tr_time.tv_secs = POLL_SECS;
            tr->tr_time.tv_micro = 0;
            SendIO((struct IORequest *)tr);
        }
        while ((io = (struct IOStdReq *)GetMsg(port))) {
            if (io->io_Command == 0xffff) {     /* our own "stop" */
                if (tr) {
                    AbortIO((struct IORequest *)tr);
                    WaitIO((struct IORequest *)tr);
                    CloseDevice((struct IORequest *)tr);
                    DeleteIORequest((struct IORequest *)tr);
                }
                if (tport) DeleteMsgPort(tport);
                Forbid();
                cmdport = NULL;
                FreeMem(port, sizeof(struct MsgPort));
                ReplyMsg(&io->io_Message);
                RemTask(NULL);
            }
            perform(io);
        }
    }
}

static int start_task(void)
{
    static UBYTE stack[8192] __attribute__((aligned(4)));
    static struct Task tc;
    if (cmdport) return 1;
    memset(&tc, 0, sizeof(tc));
    tc.tc_Node.ln_Type = NT_TASK;
    tc.tc_Node.ln_Name = (char *)df_devname;
    tc.tc_Node.ln_Pri = 1;
    tc.tc_SPLower = stack;
    tc.tc_SPUpper = stack + sizeof(stack);
    tc.tc_SPReg = tc.tc_SPUpper;
    tc.tc_MemEntry.lh_Head = (struct Node *)&tc.tc_MemEntry.lh_Tail;
    tc.tc_MemEntry.lh_TailPred = (struct Node *)&tc.tc_MemEntry.lh_Head;
    opener_task = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);
    if (!AddTask(&tc, (APTR)task_main, NULL))
        return 0;
    Wait(SIGF_SINGLE);
    task = &tc;
    return cmdport != NULL;
}

static void stop_task(void)
{
    static struct IOStdReq quit;
    struct MsgPort reply_port;
    if (!cmdport) return;
    memset(&reply_port, 0, sizeof(reply_port));
    reply_port.mp_Node.ln_Type = NT_MSGPORT;
    reply_port.mp_Flags = PA_SIGNAL;
    reply_port.mp_SigBit = SIGB_SINGLE;
    reply_port.mp_SigTask = FindTask(NULL);
    reply_port.mp_MsgList.lh_Head = (struct Node *)&reply_port.mp_MsgList.lh_Tail;
    reply_port.mp_MsgList.lh_TailPred = (struct Node *)&reply_port.mp_MsgList.lh_Head;
    memset(&quit, 0, sizeof(quit));
    quit.io_Message.mn_ReplyPort = &reply_port;
    quit.io_Message.mn_Length = sizeof(quit);
    quit.io_Command = 0xffff;
    SetSignal(0, SIGF_SINGLE);
    PutMsg(cmdport, &quit.io_Message);
    WaitPort(&reply_port);
    GetMsg(&reply_port);
    task = NULL;
}

/* An id that did not answer is not selected again for 3 s: HDToolBox opens
 * every id with every LUN, twice, and each try at an empty id costs the
 * 250 ms selection timeout. */
static ULONG absent_at[7];              /* DateStamp ticks + 1; 0 = not marked */

static ULONG now_ticks(void)
{
    struct DateStamp ds;
    struct DosLibrary *DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (!DOSBase) return 0;
    DateStamp(&ds);
    CloseLibrary((struct Library *)DOSBase);
    return (ULONG)ds.ds_Days * 4320000UL + (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

static void mark_absent(int id) { absent_at[id] = now_ticks() + 1; }

static int id_absent(int id)
{
    ULONG t;
    if (!absent_at[id]) return 0;
    t = now_ticks() + 1;
    if (t >= absent_at[id] && t - absent_at[id] < 150) return 1;
    absent_at[id] = 0;
    return 0;
}

int df_ours(struct Unit *u)
{
    int i;
    if (!u) return 0;
    if (ide_units[0] && u == &ide_units[0]->u) return 1;
    if (ide_units[1] && u == &ide_units[1]->u) return 1;
    for (i = 0; i < MAX_UNITS; i++)
        if (units[i] && u == &units[i]->u) return 1;
    return 0;
}

/* ---- device vectors ----------------------------------------------------- */

void df_open(REG(a1, struct IOStdReq *io), REG(d0, ULONG unitnum), REG(d1, ULONG flags),
             REG(a6, struct Device *dev))
{
    DBG({ dbg_s("open "); dbg_x(unitnum, 4); dbg_s("\r\n"); });
    struct DFUnit *un;
    int id = unitnum % 10, lun = unitnum / 10;

    dev->dd_Library.lib_OpenCnt++;          /* no expunge while we may Wait() */
    /* one open at a time: starting the task and probing a new unit both
     * Wait(), which breaks exec's Forbid - two first opens at once got two
     * unit structures (one orphaned: TDERR_BadUnitNum) and could AddTask
     * the same task twice */
    ObtainSemaphore(&openlock);
    io->io_Error = IOERR_OPENFAIL;
    if (!start_task()) goto out;
    /* the port is ours from the first open on: before it is ever switched */
    if (!ide_takeover()) { io->io_Error = IOERR_UNITBUSY; goto out; }
    if (unitnum == IDE_UNIT0 || unitnum == IDE_UNIT0 + 1) {
        if (!(un = ide_units[unitnum - IDE_UNIT0])) { io->io_Error = TDERR_BadUnitNum; goto out; }
        goto opened;
    }
    if (id > 6 || lun > 7) goto out;
    un = units[unitnum];
    if (!un && id_absent(id)) { io->io_Error = TDERR_BadUnitNum; goto out; }
    if (!un) {
        if (!(un = AllocMem(sizeof(*un), MEMF_PUBLIC | MEMF_CLEAR))) goto out;
        un->id = (UBYTE)id;
        un->lun = (UBYTE)lun;
        un->u.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        un->u.unit_MsgPort.mp_Flags = PA_IGNORE;
        DF_NEWLIST(&un->chgints);
        int r = unit_probe(un);
        if (r <= 0) {                       /* nothing at that id/LUN */
            if (r < 0) mark_absent(id);
            FreeMem(un, sizeof(*un));
            io->io_Error = TDERR_BadUnitNum;
            goto out;
        }
        units[unitnum] = un;
    }
opened:
    un->u.unit_OpenCnt++;
    io->io_Unit = &un->u;
    io->io_Error = 0;
    dev->dd_Library.lib_OpenCnt++;
    dev->dd_Library.lib_Flags &= ~LIBF_DELEXP;
out:
    ReleaseSemaphore(&openlock);
    dev->dd_Library.lib_OpenCnt--;
}

static BPTR do_expunge(struct Device *dev)
{
    BPTR seg;
    int i;
    if (dev->dd_Library.lib_OpenCnt) { dev->dd_Library.lib_Flags |= LIBF_DELEXP; return 0; }
    stop_task();
    for (i = 0; i < MAX_UNITS; i++)
        if (units[i]) { FreeMem(units[i], sizeof(struct DFUnit)); units[i] = NULL; }
    seg = seglist;
    Remove(&dev->dd_Library.lib_Node);
    FreeMem((UBYTE *)dev - dev->dd_Library.lib_NegSize,
            dev->dd_Library.lib_NegSize + dev->dd_Library.lib_PosSize);
    return seg;
}

BPTR df_close(REG(a1, struct IOStdReq *io), REG(a6, struct Device *dev))
{
    struct DFUnit *un = unit_of(io);
    io->io_Device = (struct Device *)-1;
    io->io_Unit = (struct Unit *)-1;
    if (un) un->u.unit_OpenCnt--;
    if (!--dev->dd_Library.lib_OpenCnt && (dev->dd_Library.lib_Flags & LIBF_DELEXP))
        return do_expunge(dev);
    return 0;
}

BPTR df_expunge(REG(a6, struct Device *dev))
{
    return do_expunge(dev);
}

ULONG df_null(void) { return 0; }

void df_beginio(REG(a1, struct IOStdReq *io), REG(a6, struct Device *dev))
{
    io->io_Message.mn_Node.ln_Type = NT_MESSAGE;
    io->io_Error = 0;
    if (io->io_Command == TD_ADDCHANGEINT)
        io->io_Flags &= ~IOF_QUICK;         /* it stays with us until TD_REMCHANGEINT */
    if (needs_task(io->io_Command) && cmdport) {
        io->io_Flags &= ~IOF_QUICK;
        PutMsg(cmdport, &io->io_Message);
        return;
    }
    perform(io);                            /* quick: answered here */
}

LONG df_abortio(REG(a1, struct IOStdReq *io), REG(a6, struct Device *dev))
{
    /* a TD_ADDCHANGEINT request is the only one that waits; the rest finish */
    if (io->io_Command == TD_ADDCHANGEINT) {
        struct DFUnit *un = unit_of(io);
        struct Node *n;
        int pending = 0;
        Forbid();
        if (un)
            for (n = un->chgints.lh_Head; n->ln_Succ; n = n->ln_Succ)
                if (n == &io->io_Message.mn_Node) { Remove(n); pending = 1; break; }
        Permit();
        if (pending) {
            io->io_Error = IOERR_ABORTED;
            ReplyMsg(&io->io_Message);
        }
    }
    return 0;
}

static const APTR vectors[] = {
    (APTR)df_open, (APTR)df_close, (APTR)df_expunge, (APTR)df_null,
    (APTR)df_beginio, (APTR)df_abortio, (APTR)-1
};

/* called from start.S with the segment list */
struct Device *df_init(REG(d0, BPTR seg))
{
    struct Device *dev;
    SysBase = *(struct ExecBase **)4;
    dev = (struct Device *)MakeLibrary((APTR)vectors, NULL, NULL, sizeof(struct Device), 0);
    if (!dev) return NULL;
    seglist = seg;
    InitSemaphore(&buslock);
    InitSemaphore(&openlock);
    dev->dd_Library.lib_Node.ln_Type = NT_DEVICE;
    dev->dd_Library.lib_Node.ln_Name = (char *)df_devname;
    dev->dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    dev->dd_Library.lib_Version = 1;
    dev->dd_Library.lib_Revision = 6;
    dev->dd_Library.lib_IdString = (APTR)df_idstring;
    dfdev = dev;
    AddDevice(dev);
    return dev;
}
