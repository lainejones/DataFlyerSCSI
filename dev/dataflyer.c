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

static void chip_idle(void)
{
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
    CHIP(R_ICR) = ICR_DBUS;
    CHIP(R_ICR) = ICR_DBUS | ICR_ACK;
    if (!wait_clear(R_CSBS, CSBS_REQ, T_ACK, W_ACK))
        return 0;
    CHIP(R_ICR) = 0;
    return 1;
}

static int get_byte(UBYTE *b)
{
    *b = CHIP(R_CSD);
    CHIP(R_ICR) = ICR_ACK;
    if (!wait_clear(R_CSBS, CSBS_REQ, T_ACK, W_ACK))
        return 0;
    CHIP(R_ICR) = 0;
    return 1;
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
    for (t = 0; t < T_REQ; ) {
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
 * DF_OPTS bit 0) only for whole blocks of a READ or WRITE: once the target
 * sends the first byte of a block it sends all of it. Any other command
 * (INQUIRY, MODE SENSE, ...) may end its data early, and a blind read of a
 * byte that never comes holds the bus for good. */
static UBYTE blockxfer;

static ULONG burst(UBYTE *p, ULONG n, int out)
{
    ULONG done = 0;
    while (done < n) {
        ULONG want = n - done, got;
        if (want > IRQ_BLOCK) want = IRQ_BLOCK;
        Disable();
        if (out)
            got = (blockxfer && (opts & 1)) ? df_dmaout_run(p + done, want) : df_dmaout(p + done, want);
        else
            got = blockxfer ? df_dmain(p + done, want) : df_dmain_byte(p + done, want);
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
        if (!r) break;
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
        if (!r) break;
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

/* one SCSI command; the caller holds the bus (Forbid + SCSI selected).
 * Returns the status byte, or -1 (no target / bus error). */
static int run_cmd(int id, int lun, const UBYTE *cdb, int cdblen,
                   UBYTE *data, ULONG len, int out, ULONG *actual)
{
    int status = -1, ci = 0;
    ULONG di = 0;
    UBYTE c0 = cdb[0], c1 = cdb[1];

    *actual = 0;
    if (!select_target(id))
        return -1;
    for (;;) {
        UBYTE csbs, b;
        int phase;
        if (!wait_set(R_CSBS, CSBS_REQ, T_REQ, W_REQ))
            break;
        csbs = CHIP(R_CSBS);
        if (!(csbs & CSBS_BSY))
            break;
        phase = (csbs >> 2) & 7;
        CHIP(R_TCR) = (UBYTE)phase;
        switch (phase) {
        case PH_COMMAND:
            /* SCSI-1 style LUN in byte 1 (the target is selected without IDENTIFY) */
            b = ci == 0 ? c0 : ci == 1 ? (UBYTE)((c1 & 0x1f) | (lun << 5)) : ci < cdblen ? cdb[ci] : 0;
            if (!put_byte(b)) goto busfail;
            ci++;
            break;
        case PH_DATA_IN:
            if (!out && di < len && !pio_only) {
                di += dma_in(data + di, len - di);
                break;
            }
            if (!get_byte(&b)) goto busfail;     /* more than asked for: dropped */
            if (!out && di < len) data[di] = b;
            di++;
            break;
        case PH_DATA_OUT:
            if (out && di < len && !pio_only) {
                di += dma_out(data + di, len - di);
                break;
            }
            if (!put_byte(out && di < len ? data[di] : 0)) goto busfail;
            di++;
            break;
        case PH_STATUS:
            if (!get_byte(&b)) goto busfail;
            status = b;
            break;
        case PH_MSG_IN:
            if (!get_byte(&b)) goto busfail;
            if (b == 0x00) {                 /* COMMAND COMPLETE */
                CHIP(R_TCR) = 0;
                wait_clear(R_CSBS, CSBS_BSY, T_ACK, W_BSYEND);
                *actual = di < len ? di : len;
                return status;
            }
            break;
        case PH_MSG_OUT:
            if (!put_byte(0x08)) goto busfail;   /* NOP */
            break;
        default:
            goto busfail;
        }
    }
    *actual = di < len ? di : len;
    chip_idle();
    return status;
busfail:
    chip_idle();
    *actual = di < len ? di : len;
    return -1;
}


/* the bus for one command: the port is SCSI for its length, then IDE again.
 * No Forbid(): nothing but this driver touches the port (it owns scsi.device
 * too), and the waits may sleep. The byte bursts run with interrupts off. */
static int scsi_do(int id, int lun, const UBYTE *cdb, int cdblen,
                   UBYTE *data, ULONG len, int out, ULONG *actual)
{
    int st;
    ObtainSemaphore(&buslock);
    stats.cmds++;
    SEL_SCSI();
    DBG({ int k; dbg_s("scsi "); dbg_x(id, 1); dbg_c(' ');
          for (k = 0; k < cdblen; k++) dbg_x(cdb[k], 2);
          dbg_s(" len "); dbg_x(len, 8); dbg_s(out ? " out\r\n" : " in\r\n"); });
    blockxfer = (cdb[0] == 0x08 || cdb[0] == 0x28 || cdb[0] == 0xa8 ||
                 cdb[0] == 0x0a || cdb[0] == 0x2a || cdb[0] == 0xaa) && !(len & 511);
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
    return scsi_do(un->id, un->lun, cdb, cdblen, data, len, out, actual);
}

static ULONG be32(const UBYTE *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }

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
    if (st < 0 && !un->ide)
        return -1;                                    /* no target at that id */
    if (st < 0 || got < 5 || (inq[0] & 0xe0))        /* no device, or no LUN there */
        return 0;
    un->devtype = inq[0] & 0x1f;
    un->removable = (inq[1] & 0x80) != 0;
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
    un->blocks = 0;
    un->bsize = 512;
    return 1;                                /* there, but no medium / not ready */
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

static void rw(struct IOStdReq *io, int out, int is64)
{
    struct DFUnit *un = unit_of(io);
    ULONG hi = is64 ? io->io_Actual : 0, lo = io->io_Offset, len = io->io_Length, done = 0;
    ULONG bs, lba, nblk;
    UBYTE *data = io->io_Data;

    if (out) { stats.writes++; stats.wbytes += len; } else { stats.reads++; stats.rbytes += len; }
    count_size(len);
    io->io_Actual = 0;
    if (!un->bsize && unit_probe(un) <= 0) { io->io_Error = TDERR_DiskChanged; return; }
    bs = un->bsize ? un->bsize : 512;
    if ((lo % bs) || (len % bs)) { io->io_Error = IOERR_BADLENGTH; return; }
    /* byte offset -> block: 64-bit, bs a power of two from 512 up */
    {
        int sh = 0;
        ULONG b = bs;
        while (b > 1) { b >>= 1; sh++; }
        lba = (lo >> sh) | (sh ? hi << (32 - sh) : 0);
        if (sh < 32 && (hi >> sh)) { io->io_Error = IOERR_BADADDRESS; return; }
    }
    while (done < len) {
        UBYTE cdb[10];
        ULONG want = len - done, got;
        int st;
        if (want > CHUNK && !(un->ide && !un->atapi)) want = CHUNK;   /* an IDE disk has no Forbid() */
        nblk = want / bs;
        if (nblk > 0xffff) nblk = 0xffff, want = nblk * bs;
        cdb[0] = out ? 0x2a : 0x28; cdb[1] = 0;
        cdb[2] = (UBYTE)(lba >> 24); cdb[3] = (UBYTE)(lba >> 16); cdb[4] = (UBYTE)(lba >> 8); cdb[5] = (UBYTE)lba;
        cdb[6] = 0; cdb[7] = (UBYTE)(nblk >> 8); cdb[8] = (UBYTE)nblk; cdb[9] = 0;
        st = unit_cmd(un, cdb, 10, data + done, want, out, &got);
        if (st != 0) {
            if (st == 2) request_sense(un, sense_buf, 18, &got);
            io->io_Error = st < 0 ? TDERR_NotSpecified : (out ? TDERR_WriteProt : TDERR_BadSecSum);
            if (st == 2 && (sense_buf[2] & 0x0f) == 0x06) { un->bsize = 0; un->changes++; io->io_Error = TDERR_DiskChanged; }
            if (st == 2 && (sense_buf[2] & 0x0f) == 0x07) io->io_Error = TDERR_WriteProt;
            if (st == 2 && (sense_buf[2] & 0x0f) == 0x03) io->io_Error = out ? TDERR_SeekError : TDERR_BadSecSum;
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
        sc->scsi_Status = 0;
        io->io_Error = HFERR_SelTimeout;
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
    dg->dg_SectorSize = un->bsize ? un->bsize : 512;
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
    case 0x7ff3:
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
    case CMD_UPDATE: {                          /* the drive's write cache, if it has one */
        UBYTE cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        ULONG got;
        if (!un->removable && unit_cmd(un, cdb, 10, NULL, 0, 0, &got) == 2)
            request_sense(un, sense_buf, 18, &got);  /* an old drive without it: fine */
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
    case CMD_RESET: case CMD_CLEAR: case CMD_STOP: case CMD_START: case CMD_FLUSH:
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
    case TD_PROTSTATUS:     io->io_Actual = 0; break;
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
        if (un->medium || ua) {                 /* not the first look */
            struct Node *n;
            un->changes++;
            un->bsize = 0;                      /* READ CAPACITY again: a new disc */
            un->quiet = 2;
            Forbid();
            for (n = un->chgints.lh_Head; n->ln_Succ; n = n->ln_Succ)
                Cause((struct Interrupt *)((struct IOStdReq *)n)->io_Data);
            Permit();
        }
        un->medium = in ? 1 : 2;
    }
    return in;
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
    dev->dd_Library.lib_Node.ln_Type = NT_DEVICE;
    dev->dd_Library.lib_Node.ln_Name = (char *)df_devname;
    dev->dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    dev->dd_Library.lib_Version = 1;
    dev->dd_Library.lib_Revision = 3;
    dev->dd_Library.lib_IdString = (APTR)df_idstring;
    dfdev = dev;
    AddDevice(dev);
    return dev;
}
