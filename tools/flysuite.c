/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 *
 * flysuite - one test program for dataflyer.device (and any other
 * trackdisk-style SCSI driver): HD_SCSICMD conformance, big and edge
 * requests, concurrency, a soak, the BlueSCSI/ZuluSCSI Toolbox, and on an
 * emulator a CDB fuzzer. docs/flysuite.md has every test and the safety rules.
 *
 *   flysuite UNIT=<n> [DEVICE=<name>] [UNIT2=<n>] [TESTS=all|conf,big,edge,conc,soak,toolbox,fuzz,latency,dma16]
 *            [MINUTES=<n>] [SECONDS=<n>] [WRITEFILE=<vol:dir>] [RAWWRITE=<first>,<count>]
 *            [FUZZ] [FUZZN=<n>] [IKNOW] [TOOLBOX] [WORKERS=<n>] [TIMEOUT=<s>] [SEED=<n>]
 *            [LOG=<file>] [QUIET]
 *
 * One PASS / FAIL / SKIP / HANG line per check, then a summary.
 * Return code: 0 all passed, 5 a FAIL, 10 a HANG (or the unit would not
 * open), 20 bad arguments, no memory, or a refused request.
 *
 * Nothing is ever written to the unit unless RAWWRITE=first,count is given,
 * and then only inside that range (saved first, written back at the end).
 * WRITEFILE writes files into the drawer it names and deletes them again.
 *
 * Every request is bounded by timer.device: one that does not come back
 * within TIMEOUT seconds (30) is a HANG, with what was in flight; AbortIO is
 * tried, and a request that still does not come back is left with the driver
 * (its memory is never freed) and the unit is not used again. Calls that
 * cannot be timed (OpenDevice, DOS) run in a child process the main task
 * watches. Any single request over 2 s is logged as an OUTLIER.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/filehandler.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/newstyle.h>
#include <devices/timer.h>
#include <devices/hardblocks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

static const char version[] = "$VER: flysuite 1.1.3 (12.11.2026)";

struct Device *TimerBase;

#define OUTLIER_MS 2000
#define E_HANG   (-100)              /* no answer in time */
#define E_SHORT  (-101)              /* answered, but less than asked for */
#define E_RANGE  (-102)              /* not expressible with that command */
#define E_REFUSE (-103)              /* a write outside RAWWRITE: never sent */
#define GUARD    32
#define GUARDB   0xa5

#define R_PASS 0
#define R_FAIL 1
#define R_SKIP 2
#define R_HANG 3

/* ---- settings ----------------------------------------------------------- */

static const char *g_dev = "dataflyer.device";
static const char *g_devbase = "dataflyer.device";  /* g_dev without a path: the name in mount entries */
/* 1 = DEVICE is dataflyer.device: units 100/101 are the A1200's IDE port
 * through the driver (scsi.device units 0/1 after its takeover), and its
 * DF_ commands exist. 0 = another driver: every unit is a plain SCSI/disk
 * unit of that driver; IDE=, RAWIDE= and the counters are off. */
static int g_df = 1;
static int g_danger;                 /* DANGER: the tests that may wedge or crash the machine */
static LONG g_ide = 100;             /* IDE=: the IDE unit for conc/mix; -1 = none (not dataflyer.device) */

/* case-insensitive equal */
static int streqi(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}
static LONG g_unit, g_unit2 = -1;
static ULONG g_tmo = 30000;          /* ms */
static ULONG g_minutes = 1, g_secs = 20, g_workers = 4, g_fuzzn = 400, g_seed;
static const char *g_wfile;
static ULONG g_rwfirst, g_rwcount;   /* RAWWRITE; count 0 = no raw writes */
static int g_fuzz, g_iknow, g_tbforce, g_quiet;
static ULONG g_skipids;              /* SKIPIDS: SCSI ids never opened (a network unit) */
static volatile int g_break;
static BPTR g_log;
static struct Task *g_main;
static BYTE g_jobsig = -1;
static ULONG g_t0secs;
static volatile int g_stuck;         /* a request never came back: the unit is not used again */
static volatile LONG n_res[4], n_outl;
static volatile ULONG g_maxms;

/* the unit under test */
static struct {
    ULONG blocks, bs, sh;
    int devtype, removable, pattern, emu, toolbox, tbcount;
    char vendor[9], product[17], rev[5];
    UBYTE inq[36];
} U;

/* ---- time --------------------------------------------------------------- */

static ULONG now_ms(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return (tv.tv_secs - g_t0secs) * 1000UL + tv.tv_micro / 1000UL;
}

/* ---- output: the main task prints; other tasks queue lines for it ------ */

#define RING 64
static char ring[RING][168];
static UBYTE ring_show[RING];
static volatile int r_head, r_tail;
static volatile ULONG r_lost;

/* The console goes through the DOS buffer of Output(), as the shell's own
 * commands do: under "Execute script >RAM:file" every command of the script
 * shares that one handle, and libnix's stdio wrote past its buffer with
 * Write(). The buffered output of the command before (an Echo, RDBMount)
 * was then written later at the position the buffer remembered, and the
 * file ended there - the rest of flysuite's output never showed. */
/* The LOG file is opened, appended and closed for every line: a file
 * system commits a file on Close, and a machine taken down by the driver
 * under test (a Guru, a silent reset) would otherwise lose the whole log
 * - FFS drops an unclosed file, and a drawer made just before, in the
 * validation after the reboot. This way the last line on disk names the
 * test that was running. */
static const char *g_logname;
static void out_line(int show, const char *s)
{
    if (show) { FPuts(Output(), (STRPTR)s); Flush(Output()); }
    if (g_logname) {
        BPTR fh = Open((STRPTR)g_logname, MODE_READWRITE);
        if (fh) { Seek(fh, 0, OFFSET_END); FPuts(fh, (STRPTR)s); Close(fh); }
    }
}

static void emit(int show, const char *s)
{
    if (FindTask(NULL) != g_main) {
        Forbid();
        if ((r_head + 1) % RING != r_tail) {
            strncpy(ring[r_head], s, sizeof(ring[0]) - 2);
            ring[r_head][sizeof(ring[0]) - 2] = 0;
            if (!strchr(ring[r_head], '\n')) strcat(ring[r_head], "\n");
            ring_show[r_head] = (UBYTE)show;
            r_head = (r_head + 1) % RING;
        } else
            r_lost++;
        Permit();
        return;
    }
    out_line(show, s);
}

static void drain(void)
{
    while (r_tail != r_head) {
        out_line(ring_show[r_tail], ring[r_tail]);
        r_tail = (r_tail + 1) % RING;
    }
    if (r_lost) {
        char b[64];
        sprintf(b, "    (%lu lines from the workers lost: queue full)\n", (unsigned long)r_lost);
        r_lost = 0;
        out_line(1, b);
    }
}

/* console only (headers, refusals, usage), through out_line's DOS buffer */
static void con(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (g_main && FindTask(NULL) != g_main) { emit(1, b); return; }
    FPuts(Output(), (STRPTR)b);
    Flush(Output());
}

/* an information line (not shown with QUIET, always in the log) */
static void say(const char *fmt, ...)
{
    char b[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b) - 2, fmt, ap);
    va_end(ap);
    strcat(b, "\n");
    emit(!g_quiet, b);
}

static const char *const kname[4] = { "PASS", "FAIL", "SKIP", "HANG" };

static void res(int k, const char *grp, const char *fmt, ...)
{
    char b[240];
    int n;
    va_list ap;
    n = sprintf(b, "%s %-7s ", kname[k], grp);
    va_start(ap, fmt);
    vsnprintf(b + n, sizeof(b) - n - 2, fmt, ap);
    va_end(ap);
    strcat(b, "\n");
    Forbid();
    n_res[k]++;
    Permit();
    emit(k != R_PASS || !g_quiet, b);
}

static int brk(void)
{
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) g_break = 1;
    return g_break;
}

/* ---- small helpers ------------------------------------------------------ */

static ULONG rnd(ULONG *s)
{
    ULONG x = *s ? *s : 0x9e3779b9UL;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static ULONG be32(const UBYTE *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }
static ULONG be16(const UBYTE *p) { return ((ULONG)p[0] << 8) | p[1]; }
static void put32(UBYTE *p, ULONG v) { p[0] = (UBYTE)(v >> 24); p[1] = (UBYTE)(v >> 16); p[2] = (UBYTE)(v >> 8); p[3] = (UBYTE)v; }
static ULONG umin(ULONG a, ULONG b) { return a < b ? a : b; }

/* case-insensitive "is n in h" */
static int has(const char *h, const char *n)
{
    int i, k;
    for (i = 0; h[i]; i++) {
        for (k = 0; n[k]; k++) {
            char a = h[i + k], b = n[k];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) break;
        }
        if (!n[k]) return 1;
    }
    return 0;
}

/* buffers: a size header, the data, GUARD guard bytes; never freed once a
 * request got stuck (the driver may still write into any of them) */
static UBYTE *gbuf(ULONG len, ULONG memf)
{
    ULONG *p = AllocMem(len + GUARD + 8, memf | MEMF_PUBLIC);
    UBYTE *b;
    if (!p) return NULL;
    p[0] = len + GUARD + 8;
    b = (UBYTE *)(p + 2);
    memset(b + len, GUARDB, GUARD);
    return b;
}

static void gfree(UBYTE *b)
{
    ULONG *p;
    if (!b || g_stuck) return;
    p = (ULONG *)b - 2;
    FreeMem(p, p[0]);
}

static void guard_set(UBYTE *b, ULONG len) { memset(b + len, GUARDB, GUARD); }

static int guard_ok(const UBYTE *b, ULONG len)
{
    int i;
    for (i = 0; i < GUARD; i++) if (b[len + i] != GUARDB) return 0;
    return 1;
}

/* the pattern disk: every 512-byte block = its number + "BLK!", repeated */
static int pattern_ok(const UBYTE *buf, ULONG first, ULONG n)
{
    ULONG b, k;
    for (b = 0; b < n; b++)
        for (k = 0; k < 512; k += 8)
            if (be32(buf + b * 512 + k) != first + b || memcmp(buf + b * 512 + k + 4, "BLK!", 4))
                return 0;
    return 1;
}

/* a pseudo-random fill that can be regenerated chunk by chunk */
static void genfill(UBYTE *p, ULONG n, ULONG *st)
{
    ULONG i = 0;
    while (i + 4 <= n) { put32(p + i, rnd(st)); i += 4; }
    if (i < n) {
        UBYTE w[4];
        put32(w, rnd(st));
        while (i < n) { p[i] = w[i & 3]; i++; }
    }
}

/* ---- one I/O context per task: a request, its port, a timer ------------- */

struct Ctx {
    struct MsgPort *port, *tport;
    struct IOStdReq *io;
    struct timerequest *tr;
    struct SCSICmd *sc;               /* on the heap: a stuck request may still write them */
    UBYTE *cdb, *sense;
    int opened, stuck, hungnow;
    ULONG bs, sh, t0;
    ULONG reqs, outliers, hangs, maxms, lastms;
    char tag[24];
};

static struct Ctx *ctx_new(const char *tag)
{
    struct Ctx *c = AllocMem(sizeof(*c), MEMF_PUBLIC | MEMF_CLEAR);
    if (!c) return NULL;
    strncpy(c->tag, tag, sizeof(c->tag) - 1);
    c->bs = 512; c->sh = 9;
    c->port = CreateMsgPort();
    c->tport = CreateMsgPort();
    c->io = c->port ? (struct IOStdReq *)CreateIORequest(c->port, sizeof(struct IOStdReq)) : NULL;
    c->tr = c->tport ? (struct timerequest *)CreateIORequest(c->tport, sizeof(struct timerequest)) : NULL;
    c->sc = AllocMem(sizeof(struct SCSICmd), MEMF_PUBLIC | MEMF_CLEAR);
    c->cdb = AllocMem(32, MEMF_PUBLIC | MEMF_CLEAR);
    c->sense = AllocMem(256 + GUARD, MEMF_PUBLIC | MEMF_CLEAR);
    if (!c->io || !c->tr || !c->sc || !c->cdb || !c->sense ||
        OpenDevice((STRPTR)"timer.device", UNIT_VBLANK, (struct IORequest *)c->tr, 0)) {
        if (c->tr) { DeleteIORequest((struct IORequest *)c->tr); c->tr = NULL; }
        if (c->io) DeleteIORequest((struct IORequest *)c->io);
        if (c->port) DeleteMsgPort(c->port);
        if (c->tport) DeleteMsgPort(c->tport);
        if (c->sc) FreeMem(c->sc, sizeof(struct SCSICmd));
        if (c->cdb) FreeMem(c->cdb, 32);
        if (c->sense) FreeMem(c->sense, 256 + GUARD);
        FreeMem(c, sizeof(*c));
        return NULL;
    }
    if (!TimerBase) {
        struct timeval tv;
        TimerBase = c->tr->tr_node.io_Device;
        GetSysTime(&tv);
        g_t0secs = tv.tv_secs;
    }
    return c;
}

static void ctx_free(struct Ctx *c)
{
    if (!c) return;
    CloseDevice((struct IORequest *)c->tr);
    DeleteIORequest((struct IORequest *)c->tr);
    DeleteMsgPort(c->tport);
    if (c->stuck)
        return;                       /* the request, its port and buffers stay with the driver */
    if (c->opened) CloseDevice((struct IORequest *)c->io);
    DeleteIORequest((struct IORequest *)c->io);
    DeleteMsgPort(c->port);
    FreeMem(c->sc, sizeof(struct SCSICmd));
    FreeMem(c->cdb, 32);
    FreeMem(c->sense, 256 + GUARD);
    FreeMem(c, sizeof(*c));
}

static void ctx_bs(struct Ctx *c, ULONG bs)
{
    c->bs = bs ? bs : 512;
    c->sh = 0;
    while ((1UL << c->sh) < c->bs) c->sh++;
}

static const char *cmdname(UWORD cmd)
{
    switch (cmd) {
    case CMD_READ: return "CMD_READ";
    case CMD_WRITE: return "CMD_WRITE";
    case CMD_UPDATE: return "CMD_UPDATE";
    case TD_READ64: return "TD_READ64";
    case TD_WRITE64: return "TD_WRITE64";
    case NSCMD_TD_READ64: return "NSCMD_TD_READ64";
    case NSCMD_TD_WRITE64: return "NSCMD_TD_WRITE64";
    case TD_GETGEOMETRY: return "TD_GETGEOMETRY";
    case TD_CHANGESTATE: return "TD_CHANGESTATE";
    case TD_PROTSTATUS: return "TD_PROTSTATUS";
    case TD_CHANGENUM: return "TD_CHANGENUM";
    case NSCMD_DEVICEQUERY: return "NSCMD_DEVICEQUERY";
    case HD_SCSICMD: return "HD_SCSICMD";
    }
    return "command";
}

/* what a request is, for a HANG or an OUTLIER line */
static void desc(struct Ctx *c, char *b)
{
    struct IOStdReq *io = c->io;
    if (io->io_Command == HD_SCSICMD) {
        struct SCSICmd *sc = c->sc;
        int k, n = sprintf(b, "HD_SCSICMD CDB");
        for (k = 0; k < sc->scsi_CmdLength && k < 16; k++) n += sprintf(b + n, " %02x", c->cdb[k]);
        sprintf(b + n, ", %lu bytes %s", (unsigned long)sc->scsi_Length, (sc->scsi_Flags & SCSIF_READ) ? "in" : "out");
    } else {
        ULONG blk = (io->io_Offset >> c->sh) | (c->sh ? io->io_Actual << (32 - c->sh) : 0);
        if (io->io_Command == CMD_READ || io->io_Command == CMD_WRITE) blk = io->io_Offset >> c->sh;
        sprintf(b, "%s (%u) %lu bytes at block %lu", cmdname(io->io_Command), io->io_Command,
                (unsigned long)io->io_Length, (unsigned long)blk);
    }
}

/* wait for io or the timer: 1 = io is back (and taken off the port) */
static int wait_back(struct Ctx *c, struct IORequest *io, ULONG ms)
{
    struct timerequest *tr = c->tr;
    ULONG mask = (1UL << c->port->mp_SigBit) | (1UL << c->tport->mp_SigBit);
    tr->tr_node.io_Command = TR_ADDREQUEST;
    tr->tr_time.tv_secs = ms / 1000;
    tr->tr_time.tv_micro = (ms % 1000) * 1000;
    SendIO((struct IORequest *)tr);
    for (;;) {
        if (CheckIO(io)) {
            WaitIO(io);
            AbortIO((struct IORequest *)tr);
            WaitIO((struct IORequest *)tr);
            SetSignal(0, 1UL << c->tport->mp_SigBit);     /* the abort's reply: no stale wake-up next time */
            return 1;
        }

        if (CheckIO((struct IORequest *)tr)) {
            WaitIO((struct IORequest *)tr);
            if (CheckIO(io)) { WaitIO(io); return 1; }
            return 0;
        }
        Wait(mask);
    }
}

static void bio_start(struct Ctx *c, UWORD cmd, APTR data, ULONG len, ULONG off, ULONG hi)
{
    struct IOStdReq *io = c->io;
    io->io_Command = cmd;
    io->io_Data = data;
    io->io_Length = len;
    io->io_Offset = off;
    io->io_Actual = hi;
    io->io_Flags = 0;
    io->io_Error = 0;
    c->t0 = now_ms();
    SendIO((struct IORequest *)io);
}

static LONG bio_end(struct Ctx *c)
{
    struct IOStdReq *io = c->io;
    char d[120];
    ULONG ms;
    if (!wait_back(c, (struct IORequest *)io, g_tmo)) {
        desc(c, d);
        c->hangs++;
        c->hungnow = 1;
        res(R_HANG, c->tag, "%s: no answer in %lu s; AbortIO", d, (unsigned long)(g_tmo / 1000));
        AbortIO((struct IORequest *)io);
        if (!wait_back(c, (struct IORequest *)io, 5000)) {
            emit(1, "     still not back 5 s after AbortIO: the request is left with the driver, the unit is not used again\n");
            Forbid();
            c->port->mp_Flags = PA_IGNORE;
            c->stuck = 1;
            g_stuck = 1;
            Permit();
        } else {
            char b[80];
            sprintf(b, "     back after AbortIO, io_Error %d\n", io->io_Error);
            emit(1, b);
        }
        return E_HANG;
    }
    ms = now_ms() - c->t0;
    c->reqs++;
    c->lastms = ms;
    if (ms > c->maxms) c->maxms = ms;
    if (ms > g_maxms) g_maxms = ms;
    if (ms > OUTLIER_MS) {
        char b[200];
        desc(c, d);
        c->outliers++;
        Forbid(); n_outl++; Permit();
        sprintf(b, "OUTLIER %-7s %s took %lu ms (io_Error %d)\n", c->tag, d, (unsigned long)ms, io->io_Error);
        emit(1, b);
    }
    return io->io_Error;
}

static LONG bio(struct Ctx *c, UWORD cmd, APTR data, ULONG len, ULONG off, ULONG hi)
{
    if (c->stuck) { c->hungnow = 1; return E_HANG; }
    bio_start(c, cmd, data, len, off, hi);
    return bio_end(c);
}

/* HD_SCSICMD; the CDB is copied into the context */
static void scsi_setup(struct Ctx *c, const UBYTE *cdb, int cdblen, APTR data, ULONG len, UBYTE flags, int senselen)
{
    struct SCSICmd *sc = c->sc;
    memset(sc, 0, sizeof(*sc));
    memset(c->cdb, 0, 32);
    memcpy(c->cdb, cdb, cdblen > 32 ? 32 : cdblen);
    memset(c->sense, 0, 256);
    memset(c->sense + 256, GUARDB, GUARD);
    sc->scsi_Command = c->cdb;
    sc->scsi_CmdLength = (UWORD)cdblen;
    sc->scsi_Data = (UWORD *)data;
    sc->scsi_Length = len;
    sc->scsi_Flags = flags;
    sc->scsi_SenseData = senselen ? c->sense : NULL;
    sc->scsi_SenseLength = (UWORD)senselen;
}

static LONG scsi(struct Ctx *c, const UBYTE *cdb, int cdblen, APTR data, ULONG len, UBYTE flags, int senselen)
{
    scsi_setup(c, cdb, cdblen, data, len, flags, senselen);
    return bio(c, HD_SCSICMD, c->sc, sizeof(struct SCSICmd), 0, 0);
}

#define ST(c)   ((c)->sc->scsi_Status)
#define ACT(c)  ((c)->sc->scsi_Actual)
#define SACT(c) ((c)->sc->scsi_SenseActual)
#define SKEY(c) ((c)->sense[2] & 0x0f)
#define ASC(c)  ((c)->sense[12])
#define ASCQ(c) ((c)->sense[13])

static int mk_read(UBYTE *cdb, int kind, ULONG lba, ULONG n)
{
    memset(cdb, 0, 16);
    switch (kind) {
    case 6:
        cdb[0] = 0x08; cdb[1] = (UBYTE)((lba >> 16) & 0x1f); cdb[2] = (UBYTE)(lba >> 8); cdb[3] = (UBYTE)lba;
        cdb[4] = (UBYTE)n;
        return 6;
    case 12:
        cdb[0] = 0xa8; put32(cdb + 2, lba); put32(cdb + 6, n);
        return 12;
    case 16:
        cdb[0] = 0x88; put32(cdb + 6, lba); put32(cdb + 10, n);
        return 16;
    }
    cdb[0] = 0x28; put32(cdb + 2, lba); cdb[7] = (UBYTE)(n >> 8); cdb[8] = (UBYTE)n;
    return 10;
}

/* block -> byte offset, 64-bit */
static void off64(struct Ctx *c, ULONG lba, ULONG *lo, ULONG *hi)
{
    *lo = lba << c->sh;
    *hi = c->sh ? lba >> (32 - c->sh) : 0;
}

/* the read paths */
#define P_CMD  0
#define P_TD64 1
#define P_NSD  2
#define P_SCSI 3
static const char *const pname[4] = { "CMD_READ", "TD_READ64", "NSCMD_TD_READ64", "SCSI READ(10)" };
static const char *const wname[4] = { "CMD_WRITE", "TD_WRITE64", "NSCMD_TD_WRITE64", "SCSI WRITE(10)" };

static LONG rd(struct Ctx *c, int path, ULONG lba, ULONG n, UBYTE *buf)
{
    ULONG lo, hi, len = n << c->sh;
    LONG e;
    off64(c, lba, &lo, &hi);
    if (path == P_SCSI) {
        UBYTE cdb[16];
        if (n > 0xffff) return E_RANGE;
        mk_read(cdb, 10, lba, n);
        e = scsi(c, cdb, 10, buf, len, SCSIF_READ | SCSIF_AUTOSENSE, 18);
        if (!e && (ST(c) || ACT(c) != len)) e = E_SHORT;
        return e;
    }
    /* CMD_READ has a 32-bit byte offset: it can't start, or end, past 4 GB
     * (an 8 GB disk's upper half is TD64/NSD/SCSI only) - not even sent */
    if (path == P_CMD && (hi || (unsigned long long)lo + len > 0x100000000ULL)) return E_RANGE;
    e = bio(c, path == P_CMD ? CMD_READ : path == P_TD64 ? TD_READ64 : NSCMD_TD_READ64, buf, len, lo, hi);
    if (!e && c->io->io_Actual != len) e = E_SHORT;
    return e;
}

/* the only place anything is written to a unit: inside the given range, or refused */
static LONG wr_r(struct Ctx *c, int path, ULONG lba, ULONG n, UBYTE *buf, ULONG first, ULONG count)
{
    ULONG lo, hi, len = n << c->sh;
    LONG e;
    if (!count || lba < first || n > count || lba - first > count - n) {
        res(R_FAIL, "soak", "internal: a write of %lu blocks at %lu is outside RAWWRITE - refused, not sent",
            (unsigned long)n, (unsigned long)lba);
        return E_REFUSE;
    }
    off64(c, lba, &lo, &hi);
    if (path == P_SCSI) {
        UBYTE cdb[10];
        memset(cdb, 0, 10);
        cdb[0] = 0x2a; put32(cdb + 2, lba); cdb[7] = (UBYTE)(n >> 8); cdb[8] = (UBYTE)n;
        e = scsi(c, cdb, 10, buf, len, SCSIF_WRITE | SCSIF_AUTOSENSE, 18);
        if (!e && (ST(c) || ACT(c) != len)) e = E_SHORT;
        return e;
    }
    if (path == P_CMD && (hi || (unsigned long long)lo + len > 0x100000000ULL)) path = P_TD64;   /* see rd() */
    e = bio(c, path == P_CMD ? CMD_WRITE : path == P_TD64 ? TD_WRITE64 : NSCMD_TD_WRITE64, buf, len, lo, hi);
    if (!e && c->io->io_Actual != len) e = E_SHORT;
    return e;
}

static LONG wr(struct Ctx *c, int path, ULONG lba, ULONG n, UBYTE *buf)
{
    return wr_r(c, path, lba, n, buf, g_rwfirst, g_rwcount);
}

/* CMD_READ where it reaches (all of it within the first 4 GB), else TD_READ64 */
static int low_path(struct Ctx *c, ULONG lba, ULONG n)
{
    return ((unsigned long long)lba + n) << c->sh > 0x100000000ULL ? P_TD64 : P_CMD;
}

/* a path picked at random, made one that reaches lba..lba+n-1 */
static int reach(struct Ctx *c, int p, ULONG lba, ULONG n)
{
    return p == P_CMD ? low_path(c, lba, n) : p;
}

static void errtxt(char *b, LONG e)
{
    if (e == E_HANG) strcpy(b, "HANG");
    else if (e == E_SHORT) strcpy(b, "short/bad status");
    else if (e == E_RANGE) strcpy(b, "n/a");
    else sprintf(b, "io_Error %ld", (long)e);
}

/* ---- child processes for the calls that cannot be timed ---------------- */

struct Job {
    void (*fn)(struct Job *);
    struct Task *parent;
    volatile UBYTE used, done, hung;
    volatile ULONG beat;
    char what[100];
    char name[32];
    /* in */
    struct Ctx *c;
    struct Task *task;
    LONG unit;
    int kind, idx, closeit;
    /* out */
    LONG err;
    ULONG ms, maxms, n1, n2, n3, n4, n5;
};

#define NJOBS 32
static struct Job jobs[NJOBS];

static void beat(struct Job *j, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->what, sizeof(j->what), fmt, ap);
    va_end(ap);
    j->beat = now_ms();
}

static void job_entry(void)
{
    struct Task *me = FindTask(NULL);
    const char *n = me->tc_Node.ln_Name;
    ULONG v = 0;
    struct Job *j;
    while (*n && *n != '$') n++;
    if (*n) n++;
    for (; *n; n++) v = v * 16 + (*n <= '9' ? *n - '0' : (*n | 32) - 'a' + 10);
    j = (struct Job *)v;
    j->beat = now_ms();
    j->fn(j);
    Forbid();                         /* we are gone before the parent can run on */
    j->done = 1;
    Signal(j->parent, 1UL << g_jobsig);
}

static struct Job *job_new(void)
{
    int i;
    for (i = 0; i < NJOBS; i++)
        if (!jobs[i].used || (jobs[i].done && !jobs[i].hung)) {
            memset(&jobs[i], 0, sizeof(jobs[i]));
            return &jobs[i];
        }
    return NULL;
}

static LONG g_jobpri;                 /* the priority of the next job (RACEOPEN: 10) */
static struct Task *g_lastjob;

static int job_start(struct Job *j, void (*fn)(struct Job *))
{
    struct TagItem t[5];
    struct Process *pr;
    j->fn = fn;
    j->parent = g_main;
    j->done = 0;
    j->used = 1;
    j->beat = now_ms();
    strcpy(j->what, "starting");
    sprintf(j->name, "flysuite job $%08lx", (unsigned long)j);
    t[0].ti_Tag = NP_Entry;     t[0].ti_Data = (ULONG)job_entry;
    t[1].ti_Tag = NP_Name;      t[1].ti_Data = (ULONG)j->name;
    t[2].ti_Tag = NP_StackSize; t[2].ti_Data = 24576;
    t[3].ti_Tag = NP_Priority;  t[3].ti_Data = (ULONG)g_jobpri;
    t[4].ti_Tag = TAG_DONE;     t[4].ti_Data = 0;
    if (!(pr = CreateNewProc(t))) { j->used = 0; return 0; }
    g_lastjob = j->task = &pr->pr_Task;
    return 1;
}

static struct Ctx *M;                 /* the main task's context on the unit */

/* the main task's sleep: the timer, a job finishing, or CTRL-C */
static void tick(ULONG ms)
{
    struct timerequest *tr = M->tr;
    ULONG got;
    tr->tr_node.io_Command = TR_ADDREQUEST;
    tr->tr_time.tv_secs = ms / 1000;
    tr->tr_time.tv_micro = (ms % 1000) * 1000;
    SendIO((struct IORequest *)tr);
    got = Wait((1UL << M->tport->mp_SigBit) | (1UL << g_jobsig) | SIGBREAKF_CTRL_C);
    if (got & SIGBREAKF_CTRL_C) g_break = 1;
    if (!CheckIO((struct IORequest *)tr)) AbortIO((struct IORequest *)tr);
    WaitIO((struct IORequest *)tr);
    /* the aborted timer's reply leaves its signal set: without this the next
     * Wait() returns at once and the main task spins, starving the children */
    SetSignal(0, 1UL << M->tport->mp_SigBit);
}

/* what a stuck child is doing: its state, and who has the CPU */
static void job_state(struct Job *j)
{
    char b[200], rl[160];
    struct Node *n;
    int st = -1, pri = 0, len = 0;
    ULONG sw = 0;
    rl[0] = 0;
    Disable();
    if (!j->done && j->task) { st = j->task->tc_State; sw = j->task->tc_SigWait; pri = j->task->tc_Node.ln_Pri; }
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ && len < 130; n = n->ln_Succ)
        len += snprintf(rl + len, sizeof(rl) - len, " %.16s/%d", n->ln_Name ? n->ln_Name : "?", n->ln_Pri);
    Enable();
    sprintf(b, "     the child: state %d (%s), pri %d, waiting for $%08lx; ready tasks:%s\n", st,
            st == TS_READY ? "ready: no CPU" : st == TS_WAIT ? "waiting" : st == TS_RUN ? "running" : "?", pri,
            (unsigned long)sw, rl[0] ? rl : " none");
    emit(1, b);
}

/* 1 = all done; 0 = one of them did not move for tmo ms (a HANG) */
static int wait_jobs(struct Job **js, int n, ULONG tmo)
{
    for (;;) {
        int i, all = 1;
        drain();
        for (i = 0; i < n; i++) {
            struct Job *j = js[i];
            if (j->done) continue;
            all = 0;
            ULONG b = j->beat;                 /* first: a child may move it on while we look */
            if (!j->hung && (LONG)(now_ms() - b) > (LONG)tmo) {
                j->hung = 1;
                g_stuck = 1;
                res(R_HANG, "job", "a child process did not move for %lu s, at: %s",
                    (unsigned long)(tmo / 1000), j->what);
                job_state(j);
                return 0;
            }
        }
        if (all) return 1;
        tick(250);
    }
}

/* OpenDevice in a child: bounded. 0 or the open error; E_HANG */
static void j_open(struct Job *j)
{
    ULONG t0;
    beat(j, "OpenDevice %s unit %ld", g_dev, (long)j->unit);
    t0 = now_ms();
    j->err = OpenDevice((STRPTR)g_dev, j->unit, (struct IORequest *)j->c->io, 0) ? j->c->io->io_Error : 0;
    if (!j->err && !j->c->io->io_Device) j->err = -1;
    j->ms = now_ms() - t0;
    if (!j->err) {
        if (j->closeit) CloseDevice((struct IORequest *)j->c->io);
        else j->c->opened = 1;
    }
}

static LONG open_bounded(struct Ctx *c, LONG unit, int closeit, ULONG *ms)
{
    struct Job *j = job_new();
    if (!j) return -99;
    j->c = c; j->unit = unit; j->closeit = closeit;
    if (!job_start(j, j_open)) return -99;
    if (!wait_jobs(&j, 1, g_tmo)) return E_HANG;
    if (ms) *ms = j->ms;
    return j->err;
}

/* unit geometry for a context: blocks; 0 = failed */
static ULONG ctx_geom(struct Ctx *c, ULONG *bs)
{
    struct DriveGeometry *dg = AllocMem(sizeof(*dg) + 16, MEMF_PUBLIC | MEMF_CLEAR);
    ULONG blocks = 0;
    if (!dg) return 0;
    if (bio(c, TD_GETGEOMETRY, dg, sizeof(*dg), 0, 0) == 0) {
        blocks = dg->dg_TotalSectors;
        *bs = dg->dg_SectorSize;
        ctx_bs(c, *bs);
    }
    if (!c->stuck) FreeMem(dg, sizeof(*dg) + 16);
    return blocks;
}

/* a TEST UNIT READY that clears pending UNIT ATTENTIONs: the final status */
static int unit_ready(struct Ctx *c)
{
    UBYTE cdb[6] = { 0, 0, 0, 0, 0, 0 };
    int k;
    LONG e = 0;
    for (k = 0; k < 4; k++) {
        e = scsi(c, cdb, 6, NULL, 0, SCSIF_READ | SCSIF_AUTOSENSE, 18);
        if (e == E_HANG) return -1;
        if (ST(c) != 2) break;
    }
    return e == E_HANG ? -1 : ST(c);
}

/* ---- the toolbox (BlueSCSI / ZuluSCSI vendor commands) ------------------ */

#define TB_LIST  0xd0
#define TB_GET   0xd1
#define TB_COUNT 0xd2
#define TB_ENT   40

static LONG tb_count(struct Ctx *c, UBYTE *buf, int *count)
{
    UBYTE cdb[10] = { TB_COUNT, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    LONG e;
    buf[0] = 0;
    e = scsi(c, cdb, 10, buf, 1, SCSIF_READ | SCSIF_AUTOSENSE, 18);
    if (!e && ST(c) == 0 && ACT(c) == 1) { *count = buf[0]; return 0; }
    return e ? e : E_SHORT;
}

static LONG tb_list(struct Ctx *c, UBYTE *buf, ULONG len)
{
    UBYTE cdb[10] = { TB_LIST, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    return scsi(c, cdb, 10, buf, len, SCSIF_READ | SCSIF_AUTOSENSE, 18);
}

static LONG tb_get(struct Ctx *c, int idx, ULONG blk, UBYTE *buf)
{
    UBYTE cdb[10] = { TB_GET, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    cdb[1] = (UBYTE)idx;
    put32(cdb + 2, blk);
    return scsi(c, cdb, 10, buf, 4096, SCSIF_READ | SCSIF_AUTOSENSE, 18);
}

static ULONG tb_size(const UBYTE *e) { return be32(e + 36) | (e[35] ? 0xffffffffUL : 0); }  /* 40-bit, capped */

/* GET of a file index >= 32: byte 1 must reach the target unchanged */
static void tb_byte1(const char *grp, struct Ctx *c, UBYTE *list, int nent)
{
    UBYTE *a = gbuf(4096, MEMF_ANY), *b = gbuf(4096, MEMF_ANY);
    int i, done = 0;
    if (!a || !b) { res(R_SKIP, grp, "vendor CDB byte 1: no memory"); goto out; }
    for (i = 32; i < nent && done < 3; i++) {
        const UBYTE *ei = list + i * TB_ENT, *el = list + (i & 31) * TB_ENT;
        ULONG want = umin(tb_size(ei), 4096), got, gotlow;
        LONG e;
        if (ei[1] != 0) continue;                      /* a directory */
        e = tb_get(c, i, 0, a);
        if (e == E_HANG) goto out;
        got = ACT(c);
        e = e ? e : ST(c);
        tb_get(c, i & 31, 0, b);
        gotlow = ACT(c);
        if (e) { res(R_FAIL, grp, "vendor CDB $D1 (Toolbox GET) index %d: error %ld", i, (long)e); done++; continue; }
        if (got != want)
            res(R_FAIL, grp, "vendor CDB $D1 byte 1 = %d: %lu bytes back, file \"%.32s\" is %lu (index %d's size is %lu): byte 1 changed?",
                i, (unsigned long)got, (const char *)ei + 2, (unsigned long)want, i & 31, (unsigned long)umin(tb_size(el), 4096));
        else if (tb_size(ei) == tb_size(el) && !memcmp(a, b, got))
            res(R_SKIP, grp, "vendor CDB byte 1 = %d: files %d and %d look the same, can't tell", i, i, i & 31);
        else
            res(R_PASS, grp, "vendor CDB $D1 byte 1 = %d intact: \"%.32s\", %lu bytes (index %d gives %lu)",
                i, (const char *)ei + 2, (unsigned long)got, i & 31, (unsigned long)gotlow);
        done++;
    }
    if (!done) res(R_SKIP, grp, "vendor CDB byte 1: the Toolbox lists %d files, none from index 32 on", nent);
out:
    gfree(a); gfree(b);
}

/* ---- group 1: conformance ----------------------------------------------- */

static const char *grp;
#define CK(cond, ...) do { if (M->hungnow) M->hungnow = 0; else res((cond) ? R_PASS : R_FAIL, grp, __VA_ARGS__); } while (0)
#define HUNG() (M->hungnow ? (M->hungnow = 0, 1) : 0)

/* dataflyer.device's command counter since the last read (DF_STATS reads
 * and clears); 0xffffffff = not ours / not readable */
static ULONG df_cmds(void)
{
    ULONG st[24];
    memset(st, 0, sizeof(st));
    if (!g_df || g_stuck) return 0xffffffffUL;
    if (bio(M, 0x7ff0, st, sizeof(st), 0, 0) != 0 || M->io->io_Actual < 23 * 4) return 0xffffffffUL;
    return st[4];
}

static LONG inquiry(struct Ctx *c, UBYTE *buf, ULONG buflen, UBYTE alloc)
{
    UBYTE cdb[6] = { 0x12, 0, 0, 0, 0, 0 };
    cdb[4] = alloc;
    return scsi(c, cdb, 6, buf, buflen, SCSIF_READ | SCSIF_AUTOSENSE, 18);
}

/* a READ / MODE SENSE the target does not have: ILLEGAL REQUEST */
static int unsupported(struct Ctx *c) { return ST(c) == 2 && SACT(c) >= 3 && SKEY(c) == 5; }
static const char *tdname(LONG e);

static void t_conf_absent(void)
{
    struct Ctx *x = ctx_new("open");
    int id, myid = g_unit % 10, k;
    ULONG ms;
    LONG e;
    static const LONG bad[3] = { 7, 80, 102 };
    if (!x) return;
    for (id = 0; id < 7; id++) {
        if (id == myid || (g_unit2 >= 0 && id == g_unit2 % 10) || (g_skipids & (1UL << id))) continue;
        for (k = 0; k < 2; k++) {
            e = open_bounded(x, id, 1, &ms);
            if (e == E_HANG) goto out;
            if (!e) { say("    id %d: a target answers (not an absent id)", id); break; }
            /* dataflyer.device says TDERR_BadUnitNum; another driver may use its own code */
            CK((e == TDERR_BadUnitNum || !g_df) && ms < 3000, "absent SCSI id %d (%s open): error %ld in %lu ms",
               id, k ? "second" : "first", (long)e, (unsigned long)ms);
        }
    }
    if (g_unit < 10) {
        LONG lunit = g_unit + 10;
        e = open_bounded(x, lunit, 0, &ms);
        if (e == E_HANG) goto out;
        if (e)
            CK(ms < 3000, "LUN 1 of id %ld (unit %ld): open fails cleanly, error %ld in %lu ms",
               (long)g_unit, (long)lunit, (long)e, (unsigned long)ms);
        else {
            /* the target answers LUN 1: does it give LUN 0's data (a target that ignores the LUN)? */
            UBYTE *a = gbuf(512, MEMF_ANY), *b = gbuf(512, MEMF_ANY);
            LONG e1, e2;
            ULONG bs;
            ctx_geom(x, &bs);
            e1 = a ? bio(x, CMD_READ, a, 512, 0, 0) : -1;
            e2 = b ? bio(M, CMD_READ, b, 512, 0, 0) : -1;
            if (e1 != E_HANG && e2 != E_HANG)
                res(R_SKIP, grp, "LUN 1 of id %ld opens (in %lu ms); its block 0 %s LUN 0's: the target %s",
                    (long)g_unit, (unsigned long)ms, (!e1 && !e2 && !memcmp(a, b, 512)) ? "equals" : "differs from",
                    (!e1 && !e2 && !memcmp(a, b, 512)) ? "ignores the LUN (emulator?)" : "has a LUN 1");
            CloseDevice((struct IORequest *)x->io);
            x->opened = 0;
            gfree(a); gfree(b);
        }
    }
    for (k = 0; k < (g_df ? 3 : 2); k++) {        /* 102: dataflyer.device has no unit there; another driver may */
        e = open_bounded(x, bad[k], 1, &ms);
        if (e == E_HANG) goto out;
        CK(e != 0 && ms < 3000, "invalid unit %ld: open fails, error %ld in %lu ms", (long)bad[k], (long)e, (unsigned long)ms);
    }
out:
    if (!x->stuck) ctx_free(x);
}

static void t_conf(void)
{
    struct Ctx *c = M;
    UBYTE *b = gbuf(4096 + 8, MEMF_ANY), *ref = gbuf(4 * 4096, MEMF_ANY), *chip = gbuf(4100, MEMF_CHIP);
    UBYTE *fast = gbuf(4100, MEMF_FAST), inq[36], cdb[16];
    ULONG bs = U.bs, L, lba, exp;
    LONG e;
    int i, k, attempt;
    static const UBYTE lens[5] = { 5, 36, 255, 37, 7 };

    grp = "conf";
    if (!fast) { fast = gbuf(4100, MEMF_ANY); say("    no fast RAM: the \"fast\" buffers are in any RAM"); }
    if (!b || !ref || !chip || !fast) { res(R_SKIP, grp, "no memory"); goto out; }

    /* TEST UNIT READY */
    {
        int st = unit_ready(c);
        if (st < 0) goto out;
        CK(st == 0, "TEST UNIT READY: status %d", st);
        memset(cdb, 0, 16);
        e = scsi(c, cdb, 6, NULL, 0, SCSIF_READ, 0);
        CK(e == 0 && ST(c) == 0 && ACT(c) == 0, "zero-length TEST UNIT READY, no buffer, no sense: io_Error %ld, status %d",
           (long)e, ST(c));
        e = scsi(c, cdb, 12, NULL, 0, SCSIF_READ, 0);
        CK(e == 0 && ST(c) == 0, "TEST UNIT READY with CmdLength 12 (group 0: the target takes 6): io_Error %ld, status %d",
           (long)e, ST(c));
        e = scsi(c, cdb, 16, NULL, 0, SCSIF_READ, 0);
        CK(e == 0 && ST(c) == 0, "TEST UNIT READY with CmdLength 16: io_Error %ld, status %d", (long)e, ST(c));
    }

    /* INQUIRY */
    memset(b, 0, 300); guard_set(b, 36);
    e = inquiry(c, b, 36, 36);
    if (HUNG()) goto out;
    memcpy(inq, b, 36);
    exp = umin(inq[4] + 5UL, 36);
    CK(e == 0 && ST(c) == 0 && ACT(c) == exp && guard_ok(b, 36), "INQUIRY 36: actual %lu (expected %lu), type %d, \"%.8s\" \"%.16s\" \"%.4s\"",
       (unsigned long)ACT(c), (unsigned long)exp, inq[0] & 0x1f, (char *)inq + 8, (char *)inq + 16, (char *)inq + 32);
    for (i = 0; i < 5; i++) {
        L = lens[i];
        memset(b, 0, 300); guard_set(b, L);
        e = inquiry(c, b, L, (UBYTE)L);
        exp = umin(inq[4] + 5UL, L);
        /* the driver must hand back what the target sent; a target that sends less than its own
         * additional length says (WinUAE: 36 for 32+5) is a target note, not a driver failure */
        /* and a target that pads its answer to the allocation length (a ZuluSCSI: 255 for 45+5) is
         * within the standard too - the driver then rightly reports the whole allocation moved */
        CK(e == 0 && ST(c) == 0 && (ACT(c) == exp || ACT(c) == L || (ACT(c) < exp && ACT(c) >= umin(36, L))) &&
           !memcmp(b, inq, umin(ACT(c), 36)) && guard_ok(b, L),
           "INQUIRY allocation %lu, buffer %lu: actual %lu (additional length says %lu%s), guard %s", (unsigned long)L,
           (unsigned long)L, (unsigned long)ACT(c), (unsigned long)exp,
           ACT(c) == exp ? "" : ACT(c) == L ? ": the target pads to the allocation length" : ": the TARGET sends less than its header says",
           guard_ok(b, L) ? "ok" : "OVERRUN");
    }
    memset(b, 0, 300); guard_set(b, 20);
    e = inquiry(c, b, 20, 36);
    CK(e == 0 && ACT(c) == 20 && !memcmp(b, inq, 20) && guard_ok(b, 20),
       "INQUIRY 36 into a 20-byte buffer: actual %lu, guard %s", (unsigned long)ACT(c), guard_ok(b, 20) ? "ok" : "OVERRUN");
    memset(b, 0, 300); guard_set(b, 100);
    e = inquiry(c, b, 100, 36);
    CK(e == 0 && ACT(c) == umin(inq[4] + 5UL, 36) && !memcmp(b, inq, 36),
       "INQUIRY 36 into a 100-byte buffer: actual %lu (the target sends 36)", (unsigned long)ACT(c));
    e = inquiry(c, NULL, 0, 0);
    CK(e == 0 && ST(c) == 0 && ACT(c) == 0, "INQUIRY allocation 0, no buffer: io_Error %ld, status %d, actual %lu",
       (long)e, ST(c), (unsigned long)ACT(c));
    e = inquiry(c, NULL, 0, 36);
    CK(e == 0 && ST(c) == 0 && ACT(c) == 0, "INQUIRY 36 with no buffer (the driver drops the data): io_Error %ld, actual %lu",
       (long)e, (unsigned long)ACT(c));
    for (k = 0; k < 4; k++) {
        UBYTE *p = (k < 2 ? chip : fast) + (k & 1 ? 3 : 1);
        memset(p, 0, 40); guard_set(p, 36);
        e = inquiry(c, p, 36, 36);
        CK(e == 0 && !memcmp(p, inq, 36) && guard_ok(p, 36), "INQUIRY 36 into %s RAM at an odd address (+%d)",
           k < 2 ? "chip" : "fast", k & 1 ? 3 : 1);
    }

    /* REQUEST SENSE */
    {
        UBYTE rs[6] = { 0x03, 0, 0, 0, 18, 0 };
        memset(b, 0xee, 40); guard_set(b, 18);
        e = scsi(c, rs, 6, b, 18, SCSIF_READ, 0);
        CK(e == 0 && ST(c) == 0 && ACT(c) >= 8 && ACT(c) <= 18 && (b[0] & 0x7e) == 0x70 && guard_ok(b, 18),
           "REQUEST SENSE 18: actual %lu, response code $%02x, key %d", (unsigned long)ACT(c), b[0], b[2] & 15);
        rs[4] = 13;
        memset(b, 0xee, 40); guard_set(b, 13);
        e = scsi(c, rs, 6, b, 13, SCSIF_READ, 0);
        CK(e == 0 && ST(c) == 0 && ACT(c) >= 8 && ACT(c) <= 13 && guard_ok(b, 13),
           "REQUEST SENSE odd allocation 13: actual %lu", (unsigned long)ACT(c));
    }

    /* READ CAPACITY (10) and (16) */
    memset(cdb, 0, 16); cdb[0] = 0x25;
    memset(b, 0, 64); guard_set(b, 8);
    e = scsi(c, cdb, 10, b, 8, SCSIF_READ | SCSIF_AUTOSENSE, 18);
    CK(e == 0 && ST(c) == 0 && ACT(c) == 8 && be32(b) + 1 == U.blocks && be32(b + 4) == bs && guard_ok(b, 8),
       "READ CAPACITY(10): last block %lu, %lu bytes (geometry: %lu blocks of %lu)", (unsigned long)be32(b),
       (unsigned long)be32(b + 4), (unsigned long)U.blocks, (unsigned long)bs);
    memset(b, 0, 64);
    e = scsi(c, cdb, 6, b, 8, SCSIF_READ | SCSIF_AUTOSENSE, 18);
    CK(e == 0 && ST(c) == 0 && be32(b) + 1 == U.blocks, "READ CAPACITY(10) sent with CmdLength 6 (the driver pads with zeros): status %d",
       ST(c));
    memset(cdb, 0, 16); cdb[0] = 0x9e; cdb[1] = 0x10; cdb[13] = 32;
    memset(b, 0, 64); guard_set(b, 32);
    e = scsi(c, cdb, 16, b, 32, SCSIF_READ | SCSIF_AUTOSENSE, 18);
    if (!HUNG()) {
        if (e && unsupported(c))
            res(R_SKIP, grp, "READ CAPACITY(16): not supported by the target (ILLEGAL REQUEST, ASC $%02x)", ASC(c));
        else
            CK(e == 0 && ST(c) == 0 && ACT(c) >= 12 && be32(b) == 0 && be32(b + 4) + 1 == U.blocks && be32(b + 8) == bs &&
               guard_ok(b, 32), "READ CAPACITY(16) (a 16-byte CDB): io_Error %ld, actual %lu, last block %lu",
               (long)e, (unsigned long)ACT(c), (unsigned long)be32(b + 4));
    }

    if (!U.blocks) { res(R_SKIP, grp, "no medium: the READ checks need one"); goto sense_tests; }

    /* READ(6)/(10)/(12)/(16) of the same blocks. The middle of the unit is
     * where a mounted FFS keeps its root block, which the file system
     * rewrites whenever something touches the volume (the validator after
     * a reboot, a log written next door): when a compare fails, the
     * reference is read again, and if the block itself changed the
     * failures are withdrawn and the whole thing is done again elsewhere */
    lba = umin(U.blocks / 2, (1UL << 21) - 8);
    k = U.blocks >= 4 ? 4 : 1;
    if ((ULONG)k * bs > 4096) k = 1;
    for (attempt = 0; attempt < 3; attempt++) {
    LONG fails0 = n_res[R_FAIL];
    if (attempt) lba = (lba / 4) * 3 + 64 * attempt;   /* away from the root block */
    if (lba + 4 > U.blocks) lba = 0;
    e = rd(c, low_path(c, lba, k), lba, k, ref);
    CK(e == 0, "%s %d blocks at %lu (the reference): io_Error %ld", pname[low_path(c, lba, k)], k, (unsigned long)lba, (long)e);
    if (U.pattern) CK(pattern_ok(ref, lba, k), "the reference holds the pattern");
    {
        static const int kinds[4] = { 6, 10, 12, 16 };
        for (i = 0; i < 4; i++) {
            int n = mk_read(cdb, kinds[i], lba, k);
            memset(b, 0, k * bs + 8); guard_set(b, k * bs);
            e = scsi(c, cdb, n, b, k * bs, SCSIF_READ | SCSIF_AUTOSENSE, 18);
            if (HUNG()) goto out;
            if (e && unsupported(c) && kinds[i] >= 12)
                res(R_SKIP, grp, "READ(%d): not supported by the target (ILLEGAL REQUEST)", kinds[i]);
            else
                CK(e == 0 && ST(c) == 0 && ACT(c) == k * bs && !memcmp(b, ref, k * bs) && guard_ok(b, k * bs),
                   "READ(%d) %d blocks at %lu: status %d, actual %lu, data %s", kinds[i], k, (unsigned long)lba, ST(c),
                   (unsigned long)ACT(c), memcmp(b, ref, k * bs) ? "DIFFERENT" : "same");
        }
    }
    /* zero-length READ(10) */
    mk_read(cdb, 10, lba, 0);
    e = scsi(c, cdb, 10, b, 0, SCSIF_READ | SCSIF_AUTOSENSE, 18);
    CK(e == 0 && ST(c) == 0 && ACT(c) == 0, "READ(10) of 0 blocks: io_Error %ld, status %d, actual %lu", (long)e, ST(c),
       (unsigned long)ACT(c));
    /* odd buffers, chip and fast RAM */
    for (k = 0; k < 4; k++) {
        UBYTE *p = (k < 2 ? chip : fast) + (k & 1 ? 3 : 1);
        mk_read(cdb, 10, lba, 1);
        memset(p, 0, bs); guard_set(p, bs);
        e = scsi(c, cdb, 10, p, bs, SCSIF_READ | SCSIF_AUTOSENSE, 18);
        CK(e == 0 && ACT(c) == bs && !memcmp(p, ref, bs) && guard_ok(p, bs), "READ(10) 1 block into %s RAM at an odd address (+%d)",
           k < 2 ? "chip" : "fast", k & 1 ? 3 : 1);
    }
    if (low_path(c, lba, 1) == P_CMD) {    /* (only big blocks put lba past 4 GB) */
        memset(fast + 1, 0, bs); guard_set(fast + 1, bs);
        e = bio(c, CMD_READ, fast + 1, bs, lba << U.sh, 0);
        CK(e == 0 && c->io->io_Actual == bs && !memcmp(fast + 1, ref, bs) && guard_ok(fast + 1, bs),
           "CMD_READ 1 block into an odd buffer: io_Error %ld", (long)e);
        memset(chip, 0, bs); guard_set(chip, bs);
        e = bio(c, CMD_READ, chip, bs, lba << U.sh, 0);
        CK(e == 0 && !memcmp(chip, ref, bs) && guard_ok(chip, bs), "CMD_READ 1 block into chip RAM: io_Error %ld", (long)e);
    }
    if (n_res[R_FAIL] == fails0 || HUNG()) break;
    /* something failed: did the block change under the test? */
    if (rd(c, low_path(c, lba, k), lba, k, b) == 0 && memcmp(b, ref, k * bs)) {
        LONG n = n_res[R_FAIL] - fails0;
        Forbid(); n_res[R_FAIL] -= n; Permit();
        say("    block %lu changed between the reference and the compares (a mounted file system writing there: its root block?):"
            " the %ld FAIL(s) above are withdrawn, same tests again at another block", (unsigned long)lba, (long)n);
        continue;
    }
    break;
    }
    /* a buffer shorter than the data, and one longer */
    if (U.blocks > lba + 1 && 2 * bs <= 4096) {
        static const ULONG shortl[2] = { 612, 512 };
        e = rd(c, low_path(c, lba, 2), lba, 2, ref);
        for (i = 0; i < 2; i++) {
            ULONG sl = shortl[i] > bs ? bs + 100 : shortl[i];
            if (i == 1) sl = bs;
            mk_read(cdb, 10, lba, 2);
            memset(b, 0, 2 * bs); guard_set(b, sl);
            e = scsi(c, cdb, 10, b, sl, SCSIF_READ | SCSIF_AUTOSENSE, 18);
            CK(e == 0 && ST(c) == 0 && ACT(c) == sl && !memcmp(b, ref, sl) && guard_ok(b, sl),
               "READ(10) 2 blocks into a %lu-byte buffer: actual %lu, guard %s, then the next command works", (unsigned long)sl,
               (unsigned long)ACT(c), guard_ok(b, sl) ? "ok" : "OVERRUN");
        }
        mk_read(cdb, 10, lba, 1);
        memset(b, 0, 2 * bs); guard_set(b, 2 * bs);
        e = scsi(c, cdb, 10, b, 2 * bs, SCSIF_READ | SCSIF_AUTOSENSE, 18);
        CK(e == 0 && ST(c) == 0 && ACT(c) == bs && !memcmp(b, ref, bs), "READ(10) 1 block into a 2-block buffer: actual %lu",
           (unsigned long)ACT(c));
    }

sense_tests:
    /* MODE SENSE (6) and (10), all pages */
    {
        UBYTE ms6[6] = { 0x1a, 0, 0x3f, 0, 255, 0 }, ms10[10] = { 0x5a, 0, 0x3f, 0, 0, 0, 0, 0x03, 0xff, 0 };
        UBYTE *m = gbuf(1024, MEMF_ANY);
        ULONG full;
        if (m) {
            memset(m, 0, 1024); guard_set(m, 255);
            e = scsi(c, ms6, 6, m, 255, SCSIF_READ | SCSIF_AUTOSENSE, 18);
            if (!HUNG()) {
                if (e && unsupported(c))
                    res(R_SKIP, grp, "MODE SENSE(6) page $3F: not supported (ILLEGAL REQUEST)");
                else {
                    full = ACT(c);
                    CK(e == 0 && ST(c) == 0 && ACT(c) >= 4 && ACT(c) == umin(m[0] + 1UL, 255) && guard_ok(m, 255),
                       "MODE SENSE(6) page $3F: actual %lu, mode data length %d+1", (unsigned long)ACT(c), m[0]);
                    memcpy(b, m, 16);
                    memset(m, 0, 64); guard_set(m, 13);
                    ms6[4] = 13;
                    e = scsi(c, ms6, 6, m, 13, SCSIF_READ | SCSIF_AUTOSENSE, 18);
                    CK(e == 0 && ACT(c) == umin(full, 13) && !memcmp(m + 1, b + 1, umin(ACT(c), 13) - 1) && guard_ok(m, 13),
                       "MODE SENSE(6) odd allocation 13: actual %lu", (unsigned long)ACT(c));
                }
            }
            memset(m, 0, 1024); guard_set(m, 1023);
            e = scsi(c, ms10, 10, m, 1023, SCSIF_READ | SCSIF_AUTOSENSE, 18);
            if (!HUNG()) {
                if (e && unsupported(c))
                    res(R_SKIP, grp, "MODE SENSE(10) page $3F: not supported (ILLEGAL REQUEST)");
                else {
                    ULONG a1 = ACT(c), want = umin(be16(m) + 2, 1023), a2;
                    int g = guard_ok(m, 1023);
                    /* the same with an even allocation: the same count means it is what the target sends */
                    memset(m, 0, 1024); guard_set(m, 1024);
                    ms10[7] = 0x04; ms10[8] = 0x00;
                    scsi(c, ms10, 10, m, 1024, SCSIF_READ | SCSIF_AUTOSENSE, 18);
                    a2 = ACT(c);
                    CK(e == 0 && a1 >= 8 && g && (a1 == want || a1 == a2),
                       "MODE SENSE(10) page $3F, allocation 1023: actual %lu, 1024: %lu, mode data length %lu+2%s", (unsigned long)a1,
                       (unsigned long)a2, (unsigned long)be16(m), a1 != want && a1 == a2 ? " (the TARGET sends less than its header says)" : "");
                }
            }
            gfree(m);
        }
    }

    /* CHECK CONDITION on purpose: READ past the last block, a bad opcode */
    if (U.blocks) {
        mk_read(cdb, 10, U.blocks, 1);
        e = scsi(c, cdb, 10, b, bs > 1024 ? 1024 : bs, SCSIF_READ | SCSIF_AUTOSENSE, 18);
        CK(e == HFERR_BadStatus && ST(c) == 2 && SACT(c) >= 8 && SACT(c) <= 18 && SKEY(c) == 5 && ASC(c) == 0x21,
           "READ(10) past the end, AUTOSENSE: io_Error %ld, status %d, sense %u bytes, key %d ASC $%02x/$%02x (want 5, $21)",
           (long)e, ST(c), SACT(c), SKEY(c), ASC(c), ASCQ(c));
        e = scsi(c, cdb, 10, b, bs > 1024 ? 1024 : bs, SCSIF_READ, 18);
        CK(e == HFERR_BadStatus && ST(c) == 2 && SACT(c) == 0,
           "READ(10) past the end, no AUTOSENSE: io_Error %ld, status %d, sense actual %u (want 0)", (long)e, ST(c), SACT(c));
        {
            UBYTE rs[6] = { 0x03, 0, 0, 0, 18, 0 };
            memset(b, 0, 32);
            e = scsi(c, rs, 6, b, 18, SCSIF_READ, 0);
            CK(e == 0 && ACT(c) >= 8 && (b[2] & 15) == 5 && b[12] == 0x21,
               "  ... and REQUEST SENSE by hand then: key %d ASC $%02x", b[2] & 15, b[12]);
        }
        e = scsi(c, cdb, 10, b, bs > 1024 ? 1024 : bs, SCSIF_READ | SCSIF_AUTOSENSE, 255);
        CK(e == HFERR_BadStatus && SACT(c) >= 8 && SACT(c) <= 255 && SKEY(c) == 5 && guard_ok(c->sense, 256),
           "READ past the end, AUTOSENSE into 255 bytes: sense %u bytes, sense buffer guard %s", SACT(c),
           guard_ok(c->sense, 256) ? "ok" : "OVERRUN");
        /* AUTOSENSE with no sense buffer: a driver that writes the sense anyway
         * writes to address 0 and up - ExecBase's pointer at 4 - and the machine
         * dies (OS 3.2's scsi.device in WinUAE did). On another driver: DANGER only */
        if (g_df || g_danger) {
            scsi_setup(c, cdb, 10, b, bs > 1024 ? 1024 : bs, SCSIF_READ | SCSIF_AUTOSENSE, 18);
            c->sc->scsi_SenseData = NULL;
            e = bio(c, HD_SCSICMD, c->sc, sizeof(struct SCSICmd), 0, 0);
            CK(e == HFERR_BadStatus && SACT(c) == 0, "READ past the end, AUTOSENSE but no sense buffer: io_Error %ld, sense actual %u",
               (long)e, SACT(c));
        } else
            res(R_SKIP, grp, "READ past the end, AUTOSENSE with no sense buffer: a driver that writes the sense anyway overwrites low memory - needs DANGER");
        unit_ready(c);
    }
    memset(cdb, 0, 16); cdb[0] = 0xff;
    e = scsi(c, cdb, 6, NULL, 0, SCSIF_READ | SCSIF_AUTOSENSE, 18);
    CK(e == HFERR_BadStatus && ST(c) == 2 && SACT(c) >= 8 && SKEY(c) == 5 && (ASC(c) == 0x20 || ASC(c) == 0x24),
       "bad opcode $FF, AUTOSENSE: status %d, key %d ASC $%02x (want 5, $20%s)", ST(c), SKEY(c), ASC(c),
       ASC(c) == 0x24 ? "; this target answers $24 INVALID FIELD IN CDB for unknown opcodes" : "");
    e = scsi(c, cdb, 6, NULL, 0, SCSIF_READ, 0);
    CK(e == HFERR_BadStatus && ST(c) == 2 && SACT(c) == 0, "bad opcode $FF, no AUTOSENSE: status %d, sense actual %u", ST(c), SACT(c));
    unit_ready(c);

    /* the request itself malformed */
    memset(cdb, 0, 16);
    e = scsi(c, cdb, 17, NULL, 0, SCSIF_READ, 0);
    CK(e == IOERR_BADLENGTH, "HD_SCSICMD with CmdLength 17: io_Error %ld (want %d)", (long)e, IOERR_BADLENGTH);
    scsi_setup(c, cdb, 6, NULL, 0, SCSIF_READ, 0);
    e = bio(c, HD_SCSICMD, c->sc, sizeof(struct SCSICmd) - 4, 0, 0);
    CK(e == IOERR_BADLENGTH, "HD_SCSICMD with io_Length < sizeof(struct SCSICmd): io_Error %ld", (long)e);
    unit_ready(c);

    /* the commands that work before they answer (the driver's long time
     * budgets, audit H1): VERIFY(10) of 16 blocks (no data), SYNCHRONIZE
     * CACHE, SEND DIAGNOSTIC self-test. Each must come back GOOD, or
     * ILLEGAL REQUEST from a target without it (SKIP) - never a timeout
     * error - and the unit must answer afterwards. On an emulator they are
     * instant; a real disk's self-test takes its time (bounded by TIMEOUT). */
    {
        static const UBYTE lc[3][10] = { { 0x2f, 0, 0, 0, 0, 0, 0, 0, 16, 0 }, { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                                         { 0x1d, 0x04, 0, 0, 0, 0, 0, 0, 0, 0 } };
        static const char *const ln[3] = { "VERIFY(10) 16 blocks", "SYNCHRONIZE CACHE", "SEND DIAGNOSTIC self-test" };
        static const int ll[3] = { 10, 10, 6 };
        for (k = 0; k < 3; k++) {
            if (k == 0 && U.blocks < 16) continue;
            e = scsi(c, lc[k], ll[k], NULL, 0, SCSIF_READ | SCSIF_AUTOSENSE, 18);
            if (HUNG()) break;
            if (e == HFERR_BadStatus && unsupported(c))
                res(R_SKIP, grp, "%s: not supported by the target (ILLEGAL REQUEST)", ln[k]);
            else
                CK(e == 0 && ST(c) == 0, "%s (long budget): io_Error %ld %s, status %d, %lu ms", ln[k], (long)e, tdname(e), ST(c),
                   (unsigned long)c->lastms);
            unit_ready(c);
        }
    }

    /* absent ids, a LUN > 0, invalid unit numbers */
    t_conf_absent();

    /* a vendor CDB passes byte 1 unchanged: the Toolbox's GET */
    if (U.toolbox) {
        UBYTE *list = gbuf(100 * TB_ENT, MEMF_ANY);
        if (list) {
            memset(list, 0, 100 * TB_ENT);
            e = tb_list(c, list, 100 * TB_ENT);
            if (!HUNG()) tb_byte1(grp, c, list, e ? 0 : ACT(c) / TB_ENT);
            gfree(list);
        }
    } else
        res(R_SKIP, grp, "vendor CDB byte 1: not a BlueSCSI/ZuluSCSI Toolbox unit (TOOLBOX forces the probe)");
out:
    gfree(b); gfree(ref); gfree(chip); gfree(fast);
}

/* ---- group 2: big requests ---------------------------------------------- */

static void cmp_paths(struct Ctx *c, ULONG lba, ULONG n, const UBYTE *ref, UBYTE *buf, int paths)
{
    char line[160];
    int p, ok = 1, len = 0, any = 0;
    ULONG bytes = n << c->sh;
    for (p = 0; p < 4; p++) {
        ULONG t0;
        LONG e;
        if (!(paths & (1 << p))) continue;
        memset(buf, 0xee, bytes);
        guard_set(buf, bytes);
        t0 = now_ms();
        e = rd(c, p, lba, n, buf);
        if (e == E_HANG) { c->hungnow = 0; return; }
        if (e == E_RANGE) continue;
        any = 1;
        if (e || memcmp(buf, ref, bytes) || !guard_ok(buf, bytes)) {
            char et[24];
            errtxt(et, e);
            len += sprintf(line + len, " %s:%s", pname[p], e ? et : !guard_ok(buf, bytes) ? "OVERRUN" : "DIFFERENT");
            ok = 0;
        } else
            len += sprintf(line + len, " %lu", (unsigned long)(now_ms() - t0));
        if (len > 120) break;
    }
    if (any)
        res(ok ? R_PASS : R_FAIL, grp, "%lu blocks (%lu KB) at block %lu, CMD/TD64/NSD/SCSI ms:%s", (unsigned long)n,
            (unsigned long)(bytes >> 10), (unsigned long)lba, line);
}

/* one block at a time with path p into ref; 0 = ok */
static int ref_read(struct Ctx *c, int p, ULONG lba, ULONG n, UBYTE *ref)
{
    ULONG k;
    for (k = 0; k < n; k++) {
        LONG e = rd(c, p, lba + k, 1, ref + (k << c->sh));
        if (e) {
            char et[24];
            if (e == E_HANG) { c->hungnow = 0; return -1; }
            errtxt(et, e);
            res(R_FAIL, grp, "reference read of block %lu (%s): %s", (unsigned long)(lba + k), pname[p], et);
            return -1;
        }
        if (brk()) return -1;
    }
    return 0;
}

static void t_big_far(ULONG lba, ULONG n, const char *what, int paths)
{
    UBYTE *ref = gbuf(n << U.sh, MEMF_ANY), *buf = gbuf(n << U.sh, MEMF_ANY);
    if (!ref || !buf) { res(R_SKIP, grp, "%s: no memory", what); goto out; }
    say("    %s: %lu blocks at %lu, reference one block at a time with SCSI READ(10)", what, (unsigned long)n, (unsigned long)lba);
    if (ref_read(M, P_SCSI, lba, n, ref) == 0) {
        if (U.pattern) CK(pattern_ok(ref, lba, n), "%s: the reference holds the pattern", what);
        cmp_paths(M, lba, n, ref, buf, paths);
    }
out:
    gfree(ref); gfree(buf);
}

static void t_big(void)
{
    static const UWORD nb[10] = { 1, 3, 126, 127, 128, 129, 130, 256, 2047, 2048 };
    ULONG R = U.blocks, lbaE, i;
    UBYTE *ref0 = NULL, *ref1 = NULL, *buf = NULL;
    unsigned long long total = (unsigned long long)U.blocks << U.sh;

    grp = "big";
    if (!U.blocks) { res(R_SKIP, grp, "no medium"); return; }
    if (R > (1048576UL >> U.sh)) R = 1048576UL >> U.sh;          /* 1 MB */
    while (R && !((ref0 = gbuf(R << U.sh, MEMF_ANY)) && (ref1 = gbuf(R << U.sh, MEMF_ANY)) && (buf = gbuf(R << U.sh, MEMF_ANY)))) {
        gfree(ref0); gfree(ref1); gfree(buf); ref0 = ref1 = buf = NULL;
        R /= 2;
    }
    if (!R) { res(R_SKIP, grp, "no memory"); return; }
    lbaE = U.blocks - R;
    say("    reference: %lu blocks at 0 and at %lu, one block at a time with CMD_READ/TD_READ64", (unsigned long)R, (unsigned long)lbaE);
    if (ref_read(M, total > 0xffffffffULL ? P_TD64 : P_CMD, 0, R, ref0) ||
        ref_read(M, total > 0xffffffffULL ? P_TD64 : P_CMD, lbaE, R, ref1))
        goto out;
    if (U.pattern)
        CK(pattern_ok(ref0, 0, R) && pattern_ok(ref1, lbaE, R), "the single-block reads hold the pattern (%lu + %lu blocks)",
           (unsigned long)R, (unsigned long)R);
    for (i = 0; i < 10 && !brk() && !g_stuck; i++) {
        ULONG n = nb[i];
        if (n > R) continue;
        cmp_paths(M, 0, n, ref0, buf, 15);
        cmp_paths(M, U.blocks - n, n, ref1 + ((R - n) << U.sh), buf, 15);
    }
    /* a block in the middle (byte offsets of a size the 32-bit commands still reach) */
    if (total > 0x80000000ULL && !g_stuck)
        t_big_far((ULONG)(0x80000000ULL >> U.sh) - 32, 64, "across 2 GB", 15);
    else
        res(R_SKIP, grp, "offsets above 2 GB: the unit is %lu MB", (unsigned long)(total >> 20));
    if (total > 0x100000000ULL + (64ULL << U.sh) && !g_stuck) {
        t_big_far((ULONG)(0x100000000ULL >> U.sh) - 32, 64, "across 4 GB", 14);
        t_big_far((ULONG)(0x100000000ULL >> U.sh) + 4096, 128, "above 4 GB", 14);
    } else
        res(R_SKIP, grp, "offsets above 4 GB: the unit is %lu MB", (unsigned long)(total >> 20));
out:
    gfree(ref0); gfree(ref1); gfree(buf);
}

/* ---- group 3: edges ----------------------------------------------------- */

static void j_openloop(struct Job *j)
{
    struct Ctx *c = ctx_new("openloop");
    int i;
    if (!c) { j->err = -98; return; }
    for (i = 0; i < 100 && !g_break; i++) {
        ULONG t0 = now_ms(), dt;
        beat(j, "OpenDevice %s unit %ld, #%d of 100", g_dev, (long)j->unit, i + 1);
        if (OpenDevice((STRPTR)g_dev, j->unit, (struct IORequest *)c->io, 0)) { j->n1++; j->err = c->io->io_Error; }
        else {
            beat(j, "CloseDevice unit %ld, #%d", (long)j->unit, i + 1);
            CloseDevice((struct IORequest *)c->io);
            j->n2++;
        }
        dt = now_ms() - t0;
        j->ms += dt;
        if (dt > j->maxms) j->maxms = dt;
    }
    ctx_free(c);
}

/* did a read that had to fail come back with block 0 (the address wrapped)? A block 0 that is
 * one byte repeated (a blank disk: all zeros) proves nothing: a driver or an emulator that
 * clears the buffer of a refused read gives the same bytes. Then only the error counts. */
static int wrapped(const UBYTE *p, const UBYTE *blk0, ULONG bs)
{
    ULONG i;
    for (i = 1; i < bs && blk0[i] == blk0[0]; i++) ;
    if (i == bs) return 0;
    return !memcmp(p, blk0, bs);
}

static void t_edge(void)
{
    struct Ctx *c = M;
    ULONG bs = U.bs, N = U.blocks, lo, hi, k;
    UBYTE *ref = gbuf(16 * 4096, MEMF_ANY), *b = gbuf(140 * 4096, MEMF_ANY), *blk0 = gbuf(4096, MEMF_ANY);
    LONG e;
    int p;

    grp = "edge";
    if (!ref || !b || !blk0) { res(R_SKIP, grp, "no memory"); goto out; }
    if (N >= 16 && bs <= 4096) {
        /* requests ending exactly at the end */
        if (ref_read(c, P_TD64, N - 8, 8, ref)) goto out;
        if (ref_read(c, P_TD64, 0, 1, blk0)) goto out;
        for (p = 0; p < 3; p++) {
            memset(b, 0xee, 8 * bs); guard_set(b, 8 * bs);
            e = rd(c, p, N - 8, 8, b);
            if (HUNG()) goto out;
            if (e == E_RANGE) continue;
            CK(e == 0 && !memcmp(b, ref, 8 * bs) && guard_ok(b, 8 * bs), "%s of the last 8 blocks (ending exactly at the end): io_Error %ld",
               pname[p], (long)e);
            e = rd(c, p, N - 1, 1, b);
            CK(e == 0 && !memcmp(b, ref + 7 * bs, bs), "%s of the last block: io_Error %ld", pname[p], (long)e);
        }
        /* one block past: must fail, and not wrap to block 0 */
        for (p = 0; p < 4; p++) {
            memset(b, 0xee, 2 * bs);
            e = rd(c, p, N - 1, 2, b);
            if (HUNG()) goto out;
            if (e == E_RANGE) continue;
            CK(e != 0 && !wrapped(b + bs, blk0, bs), "%s of the last block + 1 past the end fails (%ld), no wrap", pname[p], (long)e);
            /* (CMD_READ refused here, unsent: those are past 4 GB, not a driver answer) */
            if ((e = rd(c, p, N, 1, b)) != E_RANGE)
                CK(e != 0, "%s of block %lu (= the block count) fails: %ld", pname[p], (unsigned long)N, (long)e);
            if ((e = rd(c, p, N + 1000, 1, b)) != E_RANGE)
                CK(e != 0, "%s 1000 blocks past the end fails: %ld", pname[p], (long)e);
        }
        if (N > 140 && low_path(c, N - 129, 130) == P_CMD) {   /* the whole request below 4 GB */
            memset(b, 0xee, 130 * bs);
            e = rd(c, P_CMD, N - 129, 130, b);
            CK(e != 0, "CMD_READ of 130 blocks (2 driver chunks) ending 1 past the end fails: %ld (io_Actual %lu)", (long)e,
               (unsigned long)c->io->io_Actual);
        } else if (N > 140)
            res(R_SKIP, grp, "CMD_READ ending past the end: the end of the unit is above 4 GB, which CMD_READ can't reach");
        /* far beyond: the top of the 32-bit block range, and 2^32 blocks */
        off64(c, 0xffffffffUL, &lo, &hi);
        memset(b, 0xee, 2 * bs);
        e = bio(c, TD_READ64, b, 2 * bs, lo, hi);
        CK(e != 0 && !wrapped(b + bs, blk0, bs) && !wrapped(b, blk0, bs), "TD_READ64 2 blocks at block $FFFFFFFF fails (%ld), no wrap to 0",
           (long)e);
        lo = 0; hi = 1UL << U.sh;                       /* 2^32 blocks = 2^(32+sh) bytes */

        memset(b, 0xee, bs);
        e = bio(c, TD_READ64, b, bs, lo, hi);
        CK(e != 0 && !wrapped(b, blk0, bs), "TD_READ64 at block 2^32 (byte offset $%lx:00000000) fails (%ld), no wrap to block 0",
           (unsigned long)hi, (long)e);
        e = bio(c, NSCMD_TD_READ64, b, bs, lo, hi);
        CK(e != 0 && !wrapped(b, blk0, bs), "NSCMD_TD_READ64 at block 2^32 fails (%ld)", (long)e);
        /* lengths and offsets that are not whole blocks: dataflyer.device rejects them with
         * IOERR_BADLENGTH (rw); another driver passes with any error, nothing transferred */
#define BADLEN(e) ((e) == IOERR_BADLENGTH || (!g_df && (e) != 0 && (e) != E_HANG))
        {
            static const ULONG badlen[3] = { 100, 1, 0 };
            static const char *const bn[3] = { "100 bytes", "a block + 1", "a block - 1" };
            for (k = 0; k < 3; k++) {
                ULONG len = k == 0 ? 100 : k == 1 ? bs + 1 : bs - 1;
                (void)badlen;
                memset(b, 0x5a, 2 * bs + GUARD);
                e = bio(c, CMD_READ, b, len, 0, 0);
                for (lo = 0; lo < 2 * bs + GUARD && b[lo] == 0x5a; lo++) ;
                CK(BADLEN(e) && c->io->io_Actual == 0 && lo == 2 * bs + GUARD,
                   "CMD_READ of %s: io_Error %ld (want %d), io_Actual %lu, buffer %s", bn[k], (long)e, IOERR_BADLENGTH,
                   (unsigned long)c->io->io_Actual, lo == 2 * bs + GUARD ? "untouched" : "WRITTEN");
            }
            memset(b, 0x5a, 2 * bs + GUARD);
            e = bio(c, CMD_READ, b, bs, 100, 0);
            CK(BADLEN(e), "CMD_READ at byte offset 100: io_Error %ld (want %d)", (long)e, IOERR_BADLENGTH);
            e = bio(c, TD_READ64, b, 100, bs, 0);
            CK(BADLEN(e), "TD_READ64 of 100 bytes: io_Error %ld", (long)e);
            e = bio(c, CMD_READ, b, 0, 0, 0);
            CK(e == 0 && c->io->io_Actual == 0, "CMD_READ of 0 bytes: io_Error %ld, io_Actual %lu (nothing to do)", (long)e,
               (unsigned long)c->io->io_Actual);
        }
    } else
        res(R_SKIP, grp, "end-of-unit checks: %lu blocks of %lu bytes", (unsigned long)N, (unsigned long)bs);

    /* the trackdisk queries */
    e = bio(c, TD_CHANGESTATE, NULL, 0, 0, 0);
    CK(e == 0 && c->io->io_Actual == (N ? 0UL : 1UL), "TD_CHANGESTATE: io_Actual %lu (%s)", (unsigned long)c->io->io_Actual,
       c->io->io_Actual ? "no disk" : "disk in");
    /* TD_PROTSTATUS against the MODE SENSE header's WP bit (audit L4): a
     * CD-ROM is always protected; a target without MODE SENSE says nothing */
    {
        UBYTE hdr[4], cdb[6] = { 0x1a, 0, 0x3f, 0, 4, 0 };
        LONG ms;
        int want = -1;
        memset(hdr, 0, 4);
        ms = scsi(c, cdb, 6, hdr, 4, SCSIF_READ | SCSIF_AUTOSENSE, 18);
        if (ms == 0 && ST(c) == 0 && ACT(c) >= 3) want = (hdr[2] >> 7) & 1;
        if (U.devtype == 5) want = 1;
        e = bio(c, TD_PROTSTATUS, NULL, 0, 0, 0);
        if (want < 0)
            CK(e == 0, "TD_PROTSTATUS: io_Error %ld, io_Actual %lu (no MODE SENSE here to compare with)", (long)e,
               (unsigned long)c->io->io_Actual);
        else
            CK(e == 0 && (c->io->io_Actual != 0) == (want != 0), "TD_PROTSTATUS: io_Error %ld, io_Actual %lu (%s says %s)", (long)e,
               (unsigned long)c->io->io_Actual, U.devtype == 5 ? "a CD-ROM" : "MODE SENSE's WP bit", want ? "protected" : "writable");
        unit_ready(c);
    }
    e = bio(c, TD_CHANGENUM, NULL, 0, 0, 0);
    CK(e == 0, "TD_CHANGENUM: %lu", (unsigned long)c->io->io_Actual);
    {
        struct DriveGeometry *dg = (struct DriveGeometry *)b;
        memset(dg, 0, sizeof(*dg) + 8);
        e = bio(c, TD_GETGEOMETRY, dg, sizeof(*dg), 0, 0);
        CK(e == 0 && dg->dg_SectorSize == bs && dg->dg_TotalSectors == N && dg->dg_CylSectors &&
           dg->dg_CylSectors == dg->dg_Heads * dg->dg_TrackSectors && dg->dg_Cylinders * dg->dg_CylSectors <= N,
           "TD_GETGEOMETRY: %lu x %lu bytes, %lu cyl x %lu heads x %lu sectors, type %d", (unsigned long)dg->dg_TotalSectors,
           (unsigned long)dg->dg_SectorSize, (unsigned long)dg->dg_Cylinders, (unsigned long)dg->dg_Heads,
           (unsigned long)dg->dg_TrackSectors, dg->dg_DeviceType);
        e = bio(c, TD_GETGEOMETRY, dg, sizeof(*dg) - 4, 0, 0);
        CK(e == IOERR_BADLENGTH, "TD_GETGEOMETRY with a short io_Length: io_Error %ld", (long)e);
    }
    {
        struct NSDeviceQueryResult *q = (struct NSDeviceQueryResult *)b;
        int n = 0, rd64 = 0, scsi_ = 0, r32 = 0;
        memset(q, 0, sizeof(*q));
        e = bio(c, NSCMD_DEVICEQUERY, q, sizeof(*q), 0, 0);
        if (!e && q->nsdqr_SupportedCommands) {
            UWORD *s = q->nsdqr_SupportedCommands;
            while (n < 200 && s[n]) {
                if (s[n] == NSCMD_TD_READ64) rd64 = 1;
                if (s[n] == HD_SCSICMD) scsi_ = 1;
                if (s[n] == CMD_READ) r32 = 1;
                n++;
            }
        }
        CK(e == 0 && q->nsdqr_DeviceType == NSDEVTYPE_TRACKDISK && q->nsdqr_SizeAvailable >= 16 && c->io->io_Actual >= 16 &&
           n < 200 && rd64 && scsi_ && r32, "NSCMD_DEVICEQUERY: type %u, %lu bytes, %d commands (CMD_READ %s, NSCMD_TD_READ64 %s, HD_SCSICMD %s)",
           q->nsdqr_DeviceType, (unsigned long)q->nsdqr_SizeAvailable, n, r32 ? "yes" : "NO", rd64 ? "yes" : "NO", scsi_ ? "yes" : "NO");
        e = bio(c, NSCMD_DEVICEQUERY, q, 8, 0, 0);
        CK(e == IOERR_BADLENGTH, "NSCMD_DEVICEQUERY with an 8-byte buffer: io_Error %ld", (long)e);
    }
    e = bio(c, 0x5ff0, NULL, 0, 0, 0);
    CK(e == IOERR_NOCMD, "an unknown command ($5FF0): io_Error %ld (want %d)", (long)e, IOERR_NOCMD);
    /* CMD_UPDATE must really send the flush (audit M6: it skipped every
     * "removable" unit, CF cards among them): on dataflyer.device the
     * command counter goes up by one */
    {
        ULONG before, after;
        int t;
        for (t = 0; t < 3; t++) {        /* another task's commands land in the same counter: three tries */
            before = df_cmds();
            e = bio(c, CMD_UPDATE, NULL, 0, 0, 0);
            after = df_cmds();
            if (e || after <= 1) break;
            say("    CMD_UPDATE: %lu commands counted (another task on the bus?), once more", (unsigned long)after);
        }
        if (g_df && before != 0xffffffffUL && after != 0xffffffffUL)
            CK(e == 0 && after == 1, "CMD_UPDATE (SYNCHRONIZE CACHE / FLUSH CACHE, no data written): io_Error %ld, %lu command(s) sent (want 1)",
               (long)e, (unsigned long)after);
        else
            CK(e == 0, "CMD_UPDATE (SYNCHRONIZE CACHE, no data written): io_Error %ld", (long)e);
    }
    /* CMD_RESET (audit H1: a no-op before): on dataflyer.device a SCSI bus
     * reset; the unit must read the same block 0 right after it (the UNIT
     * ATTENTION every target raises is retried inside the driver) */
    if (N >= 16 && !g_stuck) {
        e = bio(c, CMD_RESET, NULL, 0, 0, 0);
        CK(e == 0, "CMD_RESET: io_Error %ld, %lu ms", (long)e, (unsigned long)c->lastms);
        memset(b, 0xee, bs);
        e = rd(c, P_CMD, 0, 1, b);
        CK(e == 0 && !memcmp(b, blk0, bs), "CMD_READ of block 0 right after CMD_RESET: io_Error %ld, %s", (long)e,
           e ? "-" : memcmp(b, blk0, bs) ? "DIFFERENT" : "the same");
        {
            struct DriveGeometry *dg = (struct DriveGeometry *)b;
            memset(dg, 0, sizeof(*dg));
            e = bio(c, TD_GETGEOMETRY, dg, sizeof(*dg), 0, 0);
            CK(e == 0 && dg->dg_SectorSize == bs && dg->dg_TotalSectors == N, "TD_GETGEOMETRY after CMD_RESET: %lu x %lu",
               (unsigned long)dg->dg_TotalSectors, (unsigned long)dg->dg_SectorSize);
        }
        unit_ready(c);
    }

    /* Open/Close 100 times */
    {
        struct Job *j = job_new();
        if (j) {
            j->unit = g_unit;
            if (job_start(j, j_openloop) && wait_jobs(&j, 1, g_tmo))
                CK(j->n1 == 0 && j->n2 == 100, "OpenDevice/CloseDevice 100 times: %lu failed (last error %ld), max %lu ms, total %lu ms",
                   (unsigned long)j->n1, (long)j->err, (unsigned long)j->maxms, (unsigned long)j->ms);
        }
    }
    /* two opens at once, both requests in flight together */
    if (N >= 64 && !g_stuck) {
        struct Ctx *c2 = ctx_new("open2");
        UBYTE *r2 = gbuf(32 * bs, MEMF_ANY), *b2 = gbuf(32 * bs, MEMF_ANY);
        if (c2 && r2 && b2 && open_bounded(c2, g_unit, 0, NULL) == 0 && 32 * bs <= 140 * 4096) {
            LONG e1, e2;
            ctx_bs(c2, bs);
            if (ref_read(c, P_CMD, 0, 16, ref) == 0 && ref_read(c, low_path(c, N - 32, 32), N - 32, 32, r2) == 0) {
                memset(b, 0, 16 * bs); memset(b2, 0, 32 * bs);
                off64(c, N - 32, &lo, &hi);
                bio_start(c, CMD_READ, b, 16 * bs, 0, 0);
                bio_start(c2, TD_READ64, b2, 32 * bs, lo, hi);
                e1 = bio_end(c);
                e2 = bio_end(c2);
                if (e1 != E_HANG && e2 != E_HANG)
                    CK(e1 == 0 && e2 == 0 && !memcmp(b, ref, 16 * bs) && !memcmp(b2, r2, 32 * bs),
                       "two opens of unit %ld, a request on each at once: %ld/%ld, data %s", (long)g_unit, (long)e1, (long)e2,
                       (!memcmp(b, ref, 16 * bs) && !memcmp(b2, r2, 32 * bs)) ? "right" : "WRONG");
                c->hungnow = 0;
            }
        } else
            res(R_FAIL, grp, "a second open of unit %ld at the same time failed", (long)g_unit);
        if (c2 && !c2->stuck) { gfree(r2); gfree(b2); ctx_free(c2); }
    }
out:
    gfree(ref); gfree(b); gfree(blk0);
}

/* ---- group 4: concurrency ----------------------------------------------- */

/* a worker: random reads, checked against the pattern or a second read
 * through another path; kind 1 = AbortIO on its own requests */
static void j_worker(struct Job *j)
{
    struct Ctx *c;
    char tag[24];
    ULONG bs = 512, blocks, end, s = g_seed + 77 * (j->idx + 1), maxn;
    UBYTE *a = NULL, *b = NULL;
    int pattern = 0;
    sprintf(tag, "worker%d", j->idx);
    c = ctx_new(tag);
    if (!c) { res(R_FAIL, "conc", "%s: no memory", tag); return; }
    beat(j, "%s: OpenDevice unit %ld", tag, (long)j->unit);
    if (OpenDevice((STRPTR)g_dev, j->unit, (struct IORequest *)c->io, 0)) {
        res(R_FAIL, "conc", "%s: can't open unit %ld (error %d)", tag, (long)j->unit, c->io->io_Error);
        ctx_free(c);
        return;
    }
    c->opened = 1;
    beat(j, "%s: TD_GETGEOMETRY", tag);
    blocks = ctx_geom(c, &bs);
    maxn = umin(128, blocks);
    if (!blocks || !(a = gbuf(maxn * bs, MEMF_ANY)) || !(b = gbuf(maxn * bs, MEMF_ANY))) {
        res(R_SKIP, "conc", "%s unit %ld: no medium or no memory", tag, (long)j->unit);
        goto out;
    }
    if (bs == 512 && rd(c, P_CMD, 0, 1, a) == 0 && pattern_ok(a, 0, 1) && rd(c, P_TD64, blocks - 1, 1, a) == 0 &&
        pattern_ok(a, blocks - 1, 1))
        pattern = 1;
    end = now_ms() + j->n5 * 1000;
    while (!g_break && !g_stuck && !c->stuck && now_ms() < end) {
        ULONG n = 1 + rnd(&s) % maxn, lba = rnd(&s) % (blocks - n + 1);
        int p = reach(c, rnd(&s) % 4, lba, n);
        LONG e;
        beat(j, "%s: %s %lu blocks at %lu, unit %ld", tag, pname[p], (unsigned long)n, (unsigned long)lba, (long)j->unit);
        if (j->kind == 1) {
            ULONG lo, hi;
            off64(c, lba, &lo, &hi);
            bio_start(c, TD_READ64, a, n * bs, lo, hi);
            if (rnd(&s) & 1) Delay(0);
            AbortIO((struct IORequest *)c->io);
            e = bio_end(c);
            if (e == E_HANG) break;
            if (e == IOERR_ABORTED) { j->n3++; continue; }
            if (!e && c->io->io_Actual != n * bs) e = E_SHORT;
        } else
            e = rd(c, p, lba, n, a);
        if (e == E_HANG) break;
        j->n1++;
        j->n4 += n * bs;
        if (e) {
            if (!j->n2++) res(R_FAIL, "conc", "%s: %s %lu blocks at %lu: error %ld", tag, pname[p], (unsigned long)n, (unsigned long)lba, (long)e);
            continue;
        }
        if (pattern) {
            if (!pattern_ok(a, lba, n) && !j->err++)
                res(R_FAIL, "conc", "%s: %lu blocks at %lu (%s) do not hold the pattern", tag, (unsigned long)n, (unsigned long)lba, pname[p]);
        } else {
            int q = reach(c, (p + 1 + rnd(&s) % 3) % 4, lba, n);
            e = rd(c, q, lba, n, b);
            if (e == E_HANG) break;
            if (!e && memcmp(a, b, n * bs) && !j->err++)
                res(R_FAIL, "conc", "%s: %lu blocks at %lu differ between %s and %s", tag, (unsigned long)n, (unsigned long)lba,
                    pname[p], pname[q]);
        }
    }
    {
        const char *what = j->kind == 1 ? "AbortIO worker" : "reader";
        int bad = j->n2 || j->err || c->hangs;
        res(bad ? R_FAIL : R_PASS, "conc", "%s %d, unit %ld%s: %lu reads, %lu KB, %lu aborted, %lu errors, %lu wrong, %lu hangs, max %lu ms",
            what, j->idx, (long)j->unit, pattern ? " (pattern)" : "", (unsigned long)j->n1, (unsigned long)(j->n4 >> 10),
            (unsigned long)j->n3, (unsigned long)j->n2, (unsigned long)j->err, (unsigned long)c->hangs, (unsigned long)c->maxms);
    }
out:
    if (c->stuck) return;
    gfree(a); gfree(b);
    ctx_free(c);
}

static void t_conc(void)
{
    struct Job *js[12];
    int n = 0, i, ide = 0;
    ULONG ms;
    grp = "conc";
    if (g_ide >= 0 && g_unit != g_ide && g_unit2 != g_ide) {   /* dataflyer.device only (g_ide = -1 otherwise) */
        struct Ctx *x = ctx_new("ideprobe");
        if (x) {
            LONG e = open_bounded(x, g_ide, 1, &ms);
            if (e == E_HANG) return;
            ide = e == 0;
            say("    IDE unit %ld through the driver: %s", (long)g_ide, ide ? "present, a worker reads it too" : "not there");
            ctx_free(x);
        }
    }
    for (i = 0; i < (int)g_workers + 1 + ide && n < 12; i++) {
        struct Job *j = job_new();
        if (!j) break;
        j->idx = i;
        j->n5 = g_secs;
        j->kind = i == (int)g_workers ? 1 : 0;
        j->unit = i > (int)g_workers ? g_ide : (g_unit2 >= 0 && (i & 1)) ? g_unit2 : g_unit;
        if (!job_start(j, j_worker)) break;
        js[n++] = j;
    }
    say("    %d workers for %lu s: %lu readers on unit %ld%s%s, an AbortIO worker%s", n, (unsigned long)g_secs,
        (unsigned long)g_workers, (long)g_unit, g_unit2 >= 0 ? " and UNIT2 " : "", "", ide ? ", one on the IDE unit" : "");
    if (!wait_jobs(js, n, g_tmo + 15000)) return;
    if (!g_stuck) {
        UBYTE *a = gbuf(U.bs, MEMF_ANY);
        if (a && U.blocks) {
            LONG e = rd(M, P_CMD, 0, 1, a);
            CK(e == 0 && (!U.pattern || pattern_ok(a, 0, 1)), "the unit answers normally after the workers: %ld", (long)e);
        }
        gfree(a);
    }
}

/* ---- latency: does the rest of the system keep running? ---------------- */

/* A child at priority 21 (above input.device) asks timer.device for 10 ms sleeps,
 * over and over, and notes how late each one comes back. Nothing below priority
 * 21 can delay it; a driver that holds Forbid() or Disable() through a transfer,
 * or busy-polls in a task above 21, can - and that is what a user sees as a
 * frozen pointer and a stalled network. j->idx: 0 = the idle phase, 1 = under
 * load, 2 = stop. Results per phase: samples, worst lateness (us), samples over 20 ms. */
static void j_sampler(struct Job *j)
{
    struct MsgPort *mp = CreateMsgPort();
    struct timerequest *tr = mp ? (struct timerequest *)CreateIORequest(mp, sizeof(struct timerequest)) : NULL;
    if (!tr || OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)tr, 0)) { j->err = 1; goto out; }
    while (j->idx < 2) {
        struct timeval a, b;
        LONG dt, late;
        int ph = j->idx;
        GetSysTime(&a);
        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs = 0;
        tr->tr_time.tv_micro = 10000;
        DoIO((struct IORequest *)tr);
        GetSysTime(&b);
        dt = (LONG)(b.tv_secs - a.tv_secs) * 1000000L + ((LONG)b.tv_micro - (LONG)a.tv_micro);
        late = dt > 10000 ? dt - 10000 : 0;
        if (ph == 0) { j->n1++; if ((ULONG)late > j->n2) j->n2 = late; if (late > 20000) j->n3++; }
        else         { j->n4++; if ((ULONG)late > j->maxms) j->maxms = late; if (late > 20000) j->ms++; }
        j->beat = now_ms();
    }
    CloseDevice((struct IORequest *)tr);
out:
    if (tr) DeleteIORequest((struct IORequest *)tr);
    if (mp) DeleteMsgPort(mp);
}

static void t_latency(void)
{
    struct Job *j;
    UBYTE *b = NULL;
    ULONG n, lba = 0, reads = 0, kb = 0, t0, ms, idle, load, top;
    int scsi_ok = 1, path, ok;
    LONG e = 0;
    grp = "sys";
    if (!U.blocks) { res(R_SKIP, grp, "LATENCY: no medium"); return; }
    n = umin(128, U.blocks);
    if ((n << U.sh) > 65536) n = 65536 >> U.sh;
    if (!n) n = 1;
    if (!(b = gbuf(n << U.sh, MEMF_ANY)) || !(j = job_new())) { res(R_SKIP, grp, "LATENCY: no memory"); gfree(b); return; }
    g_jobpri = 21;
    ok = job_start(j, j_sampler);
    g_jobpri = 0;
    if (!ok) { res(R_SKIP, grp, "LATENCY: can't start the sampler"); gfree(b); return; }
    t0 = now_ms();
    while (now_ms() - t0 < 3000 && !g_break) tick(250);      /* idle: what this machine does anyway */
    drain();
    if (j->err) { res(R_SKIP, grp, "LATENCY: the sampler can't open timer.device"); gfree(b); return; }
    /* CMD_READ reaches the first 4 GB only */
    top = U.blocks;
    if (U.sh && top > (0xffffffffUL >> U.sh)) top = 0xffffffffUL >> U.sh;
    j->idx = 1;
    t0 = now_ms();
    while (now_ms() - t0 < 6000 && !g_stuck && !g_break) {   /* load: reads through the driver, back to back */
        path = (scsi_ok && (reads & 1)) ? P_SCSI : P_CMD;      /* CMD_READ and HD_SCSICMD READ(10) in turn */
        e = rd(M, path, lba, n, b);
        if (HUNG()) break;
        if (e && path == P_SCSI) { scsi_ok = 0; e = 0; continue; }   /* no READ(10) through HD_SCSICMD: CMD_READ only */
        if (e) break;
        reads++; kb += (n << U.sh) >> 10;
        lba += n;
        if (lba + n > top) lba = 0;
    }
    ms = now_ms() - t0;
    j->idx = 2;
    if (!wait_jobs(&j, 1, g_tmo + 5000)) { gfree(b); return; }
    idle = j->n2; load = j->maxms;
    if (e && !reads) { res(R_FAIL, grp, "LATENCY: the load reads failed at once: io_Error %ld", (long)e); gfree(b); return; }
    res(load > idle + 50000 ? R_FAIL : R_PASS, grp,
        "LATENCY: a priority-21 task's 10 ms sleeps: idle worst %lu ms (%lu samples), during %lu reads of %lu KB (%lu KB/s) worst %lu ms, %lu of %lu over 20 ms",
        (unsigned long)(idle / 1000), (unsigned long)j->n1, (unsigned long)reads, (unsigned long)((n << U.sh) >> 10),
        (unsigned long)(ms ? kb * 1000 / ms : 0), (unsigned long)(load / 1000), (unsigned long)j->ms, (unsigned long)j->n4);
    if (load > idle + 50000)
        say("    the driver kept the CPU from a priority-21 task for up to %lu ms: Forbid()/Disable() around a transfer, or a busy loop in a task above 21",
            (unsigned long)(load / 1000));
    else if (idle > 50000)
        say("    already %lu ms late with no disk traffic: something else on this machine holds the CPU, so this says little about the driver",
            (unsigned long)(idle / 1000));
    gfree(b);
}

/* ---- dma16: buffers above the 16 MB line -------------------------------- */

/* A Zorro II DMA controller (A2091, GVP Series II, ...) reaches only the first
 * 16 MB of the address space; its driver must copy through a buffer it can reach
 * when the caller's buffer is higher (accelerator RAM on an A2000/A3000/A4000).
 * The buffer here comes from a memory header above $01000000 (exec Allocate()),
 * and every read is compared with the same blocks read into chip RAM. */
static struct MemHeader *hi_mh;
static UBYTE *hi_alloc(ULONG len)
{
    struct MemHeader *mh;
    UBYTE *p = NULL;
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head; mh->mh_Node.ln_Succ; mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
        if ((ULONG)mh->mh_Lower >= 0x01000000UL && (p = Allocate(mh, len)) != NULL) { hi_mh = mh; break; }
    Permit();
    return p;
}

static void hi_free(UBYTE *p, ULONG len)
{
    if (!p || !hi_mh || g_stuck) return;
    Forbid();
    Deallocate(hi_mh, p, len);
    Permit();
}

static void t_dma16(void)
{
    static const int offs[2] = { 0, 2 };
    UBYTE *hi = NULL, *ref = NULL, *ref2 = NULL, *save = NULL;
    ULONG nmax, len, lba, sizes[3], hilen;
    int p, s, o, k;
    LONG e;
    grp = "dma16";
    if (!U.blocks) { res(R_SKIP, grp, "no medium"); return; }
    nmax = umin(128, U.blocks);
    if ((nmax << U.sh) > 65536) nmax = 65536 >> U.sh;
    if (!nmax) nmax = 1;
    len = nmax << U.sh;
    hilen = len + 8 + GUARD;
    if (!(hi = hi_alloc(hilen))) {
        res(R_SKIP, grp, "no free RAM above 16 MB on this machine (chip, Zorro II or 24-bit RAM only): nothing to test");
        return;
    }
    ref = gbuf(len, MEMF_CHIP); ref2 = gbuf(len, MEMF_CHIP);
    if (!ref) ref = gbuf(len, MEMF_ANY);
    if (!ref2) ref2 = gbuf(len, MEMF_ANY);
    if (!ref || !ref2) { res(R_SKIP, grp, "no memory for the reference buffers"); goto out; }
    say("    buffer at $%08lx (in \"%s\", $%08lx-$%08lx); reference in %s RAM", (unsigned long)hi,
        hi_mh->mh_Node.ln_Name ? hi_mh->mh_Node.ln_Name : "?", (unsigned long)hi_mh->mh_Lower, (unsigned long)hi_mh->mh_Upper,
        (TypeOfMem(ref) & MEMF_CHIP) ? "chip" : "other");
    /* a third of the way in: away from block 0 and from an FFS root block in the middle */
    lba = U.blocks / 3;
    if (lba + nmax > U.blocks) lba = 0;
    sizes[0] = 1; sizes[1] = umin(16, nmax); sizes[2] = nmax;
    for (p = 0; p < 4 && !g_stuck && !g_break; p++) {
        if (p == P_CMD && (((unsigned long long)(lba + nmax) << U.sh) >> 32)) continue;
        e = rd(M, p, lba, 1, ref);
        if (HUNG()) goto out;
        if (e) { res(R_SKIP, grp, "%s: not available here (io_Error %ld)", pname[p], (long)e); continue; }
        for (s = 0; s < 3; s++)
            for (o = 0; o < 2; o++) {
                ULONG n = sizes[s], bl = n << U.sh;
                UBYTE *b = hi + offs[o];
                int tries;
                if (s == 1 && sizes[1] == sizes[0]) continue;
                if (s == 2 && sizes[2] == sizes[1]) continue;
                for (tries = 0; tries < 2; tries++) {
                    int same;
                    e = rd(M, p, lba, n, ref);
                    if (HUNG()) goto out;
                    memset(b, 0xee, bl); guard_set(b, bl);
                    e = e ? e : rd(M, p, lba, n, b);
                    if (HUNG()) goto out;
                    same = !e && !memcmp(b, ref, bl) && guard_ok(b, bl);
                    if (same || e) {
                        CK(same, "%s %lu block%s into RAM above 16 MB ($%08lx, %s): %s", pname[p], (unsigned long)n, n == 1 ? "" : "s",
                           (unsigned long)b, o ? "2 bytes off" : "aligned",
                           e ? "error" : same ? "same as in chip RAM" : !guard_ok(b, bl) ? "OVERRUN past the buffer" : "DIFFERENT");
                        break;
                    }
                    /* different: did the blocks themselves change (a file system writing there)? */
                    if (rd(M, p, lba, n, ref2) == 0 && memcmp(ref, ref2, bl)) { if (!tries) continue; }
                    for (k = 0; k < (int)bl && b[k] == ref[k]; k++) ;
                    CK(0, "%s %lu block%s into RAM above 16 MB ($%08lx, %s): DIFFERENT from byte %ld (got $%02x, chip RAM has $%02x)",
                       pname[p], (unsigned long)n, n == 1 ? "" : "s", (unsigned long)b, o ? "2 bytes off" : "aligned",
                       (long)k, k < (int)bl ? b[k] : 0, k < (int)bl ? ref[k] : 0);
                    break;
                }
            }
    }
    /* writes from that RAM: only inside RAWWRITE, saved first and put back */
    if (!g_rwcount) { res(R_SKIP, grp, "writes from RAM above 16 MB: need RAWWRITE (the blocks are saved, used and restored)"); goto out; }
    {
        ULONG n = umin(16, umin(g_rwcount, nmax)), bl = n << U.sh, i;
        int wp = (((unsigned long long)(g_rwfirst + n) << U.sh) >> 32) ? P_TD64 : P_CMD;   /* CMD_WRITE below 4 GB */
        if (!(save = gbuf(bl, MEMF_ANY))) { res(R_SKIP, grp, "writes: no memory to save the range"); goto out; }
        if (rd(M, wp, g_rwfirst, n, save)) { res(R_FAIL, grp, "writes: can't read blocks %lu-%lu to save them", (unsigned long)g_rwfirst, (unsigned long)(g_rwfirst + n - 1)); goto out; }
        for (o = 0; o < 2 && !g_stuck; o++) {
            UBYTE *b = hi + offs[o];
            for (i = 0; i < bl; i++) b[i] = (UBYTE)(i * 7 + 3 + o * 101);
            e = wr(M, wp, g_rwfirst, n, b);
            if (HUNG()) break;
            memset(ref, 0, bl);
            if (!e) e = rd(M, wp, g_rwfirst, n, ref);
            CK(!e && !memcmp(ref, b, bl), "%s of %lu blocks from RAM above 16 MB ($%08lx, %s), read back into chip RAM: %s", wname[wp],
               (unsigned long)n, (unsigned long)b, o ? "2 bytes off" : "aligned", e ? "error" : !memcmp(ref, b, bl) ? "same" : "DIFFERENT");
        }
        if (!g_stuck) {
            e = wr(M, wp, g_rwfirst, n, save);
            if (!e) e = rd(M, wp, g_rwfirst, n, ref);
            CK(!e && !memcmp(ref, save, bl), "writes: blocks %lu-%lu written back as they were", (unsigned long)g_rwfirst, (unsigned long)(g_rwfirst + n - 1));
        }
    }
out:
    hi_free(hi, hilen);
    gfree(ref); gfree(ref2); gfree(save);
}

/* ---- group 5: soak ------------------------------------------------------ */

static ULONG file_verify(struct Job *j, struct Ctx *c, const char *path, ULONG size, ULONG seed, UBYTE *buf, UBYTE *gen,
                         ULONG *bad)
{
    BPTR fh;
    ULONG off = 0, st = seed;
    beat(j, "Open %s to read", path);
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) { res(R_FAIL, "soak", "can't open %s again (error %ld)", path, (long)IoErr()); (*bad)++; return 0; }
    while (off < size) {
        ULONG n = umin(65536, size - off), t0, dt;
        LONG got;
        beat(j, "Read %s, %lu bytes at %lu", path, (unsigned long)n, (unsigned long)off);
        t0 = now_ms();
        got = Read(fh, buf, n);
        dt = now_ms() - t0;
        if (dt > OUTLIER_MS) { char b[160]; Forbid(); n_outl++; Permit(); sprintf(b, "OUTLIER soak    Read(%s, %lu at %lu) took %lu ms\n", path, (unsigned long)n, (unsigned long)off, (unsigned long)dt); emit(1, b); }
        if (dt > j->maxms) j->maxms = dt;
        genfill(gen, n, &st);
        if (got != (LONG)n || memcmp(buf, gen, n)) {
            ULONG k = 0;
            if (got > 0) while (k < (ULONG)got && buf[k] == gen[k]) k++;
            res(R_FAIL, "soak", "%s: %s at byte %lu (read %ld of %lu)", path, got != (LONG)n ? "short read" : "DIFFERENT",
                (unsigned long)(off + k), (long)got, (unsigned long)n);
            (*bad)++;
            break;
        }
        off += n;
    }
    beat(j, "Close %s", path);
    Close(fh);
    return off;
}

static void j_soak(struct Job *j)
{
    struct Ctx *c = ctx_new("soak");
    ULONG bs = 512, blocks, end, s = g_seed ^ 0x50a4c0deUL, nextrep, t_start = now_ms();
    UBYTE *a = NULL, *b = NULL, *save = NULL, *nb = NULL;
    ULONG raw_w = 0, raw_bad = 0, raw_err = 0, ro_n = 0, ro_bad = 0, ro_err = 0, f_n = 0, f_bytes = 0, f_bad = 0, iter = 0;
    int pattern = 0, files = 0, made_dir = 0, nbs = 0;
    char prev[256], path[256];
    ULONG prev_size = 0, prev_seed = 0, fmax = 0;
    BPTR dlock = 0;

    prev[0] = 0;
    if (!c) { res(R_FAIL, "soak", "no memory"); return; }
    beat(j, "soak: OpenDevice unit %ld", (long)g_unit);
    if (OpenDevice((STRPTR)g_dev, g_unit, (struct IORequest *)c->io, 0)) { res(R_FAIL, "soak", "can't open the unit"); ctx_free(c); return; }
    c->opened = 1;
    blocks = ctx_geom(c, &bs);
    if (!blocks || !(a = gbuf(256 * bs, MEMF_ANY)) || !(b = gbuf(256 * bs, MEMF_ANY)) || !(nb = gbuf(2 * bs, MEMF_ANY))) {
        res(R_SKIP, "soak", "no medium or no memory");
        goto out;
    }
    if (bs == 512 && rd(c, P_CMD, 0, 1, a) == 0 && pattern_ok(a, 0, 1)) pattern = 1;

    /* raw: save the range and its two neighbours */
    if (g_rwcount) {
        ULONG k;
        if (!(save = gbuf(g_rwcount * bs, MEMF_ANY))) { res(R_FAIL, "soak", "RAWWRITE: no memory to save %lu blocks", (unsigned long)g_rwcount); goto out; }
        for (k = 0; k < g_rwcount; k += 128) {
            ULONG n = umin(128, g_rwcount - k);
            beat(j, "soak: saving the RAWWRITE range, block %lu", (unsigned long)(g_rwfirst + k));
            if (rd(c, P_TD64, g_rwfirst + k, n, save + k * bs)) { res(R_FAIL, "soak", "RAWWRITE: can't read the range to save it (block %lu)", (unsigned long)(g_rwfirst + k)); goto out; }
        }
        if (g_rwfirst > 0 && rd(c, P_TD64, g_rwfirst - 1, 1, nb) == 0) nbs |= 1;
        if (g_rwfirst + g_rwcount < blocks && rd(c, P_TD64, g_rwfirst + g_rwcount, 1, nb + bs) == 0) nbs |= 2;
        say("    soak: RAWWRITE blocks %lu-%lu saved (%lu KB); written with all four write paths, read back with all four read paths",
            (unsigned long)g_rwfirst, (unsigned long)(g_rwfirst + g_rwcount - 1), (unsigned long)((g_rwcount * bs) >> 10));
    }
    /* files */
    if (g_wfile) {
        struct InfoData *id = AllocMem(sizeof(struct InfoData), MEMF_PUBLIC | MEMF_CLEAR);
        beat(j, "soak: Lock %s", g_wfile);
        dlock = Lock((STRPTR)g_wfile, ACCESS_READ);
        if (!dlock) {
            beat(j, "soak: CreateDir %s", g_wfile);
            dlock = CreateDir((STRPTR)g_wfile);
            if (dlock) { made_dir = 1; UnLock(dlock); dlock = Lock((STRPTR)g_wfile, ACCESS_READ); }
        }
        beat(j, "soak: Info %s", g_wfile);
        if (!dlock || !id || !Info(dlock, id)) res(R_FAIL, "soak", "WRITEFILE %s: can't use it (error %ld)", g_wfile, (long)IoErr());
        else if (id->id_DiskState != ID_VALIDATED) res(R_FAIL, "soak", "WRITEFILE %s: the volume is not writable (state %ld)", g_wfile, (long)id->id_DiskState);
        else {
            unsigned long long fr = (unsigned long long)(id->id_NumBlocks - id->id_NumBlocksUsed) * id->id_BytesPerBlock;
            fmax = fr / 3 > 20UL * 1048576 ? 20UL * 1048576 : (ULONG)(fr / 3);
            if (fmax < 65536) res(R_SKIP, "soak", "WRITEFILE %s: only %lu KB free", g_wfile, (unsigned long)(fr >> 10));
            else {
                files = 1;
                say("    soak: files in %s%s, 1 KB - %lu KB (%lu KB free)", g_wfile, made_dir ? " (made)" : "",
                    (unsigned long)(fmax >> 10), (unsigned long)(fr >> 10));
            }
        }
        if (id) FreeMem(id, sizeof(struct InfoData));
        if (dlock) UnLock(dlock);
    }
    if (!g_rwcount && !g_wfile)
        say("    soak: read-only (no WRITEFILE, no RAWWRITE): random reads, each read twice through two paths%s",
            pattern ? ", and the pattern" : "");

    end = now_ms() + g_minutes * 60000UL;
    nextrep = now_ms() + 60000UL;
    while (!g_break && !g_stuck && !c->stuck && now_ms() < end) {
        iter++;
        if (g_rwcount) {                                       /* one raw write + read back */
            ULONG n = 1 + rnd(&s) % umin(256, g_rwcount), lba = g_rwfirst + rnd(&s) % (g_rwcount - n + 1), st = s ^ iter;
            int wp = iter % 4, rp = reach(c, (iter / 4) % 4, lba, n);
            LONG e;
            genfill(a, n * bs, &st);
            beat(j, "soak: %s %lu blocks at %lu", wname[wp], (unsigned long)n, (unsigned long)lba);
            e = wr(c, wp, lba, n, a);
            if (e == E_HANG || e == E_REFUSE) break;
            raw_w++;
            if (e) { if (!raw_err++) res(R_FAIL, "soak", "%s %lu blocks at %lu: error %ld", wname[wp], (unsigned long)n, (unsigned long)lba, (long)e); continue; }
            memset(b, 0, n * bs);
            beat(j, "soak: %s %lu blocks at %lu (read back)", pname[rp], (unsigned long)n, (unsigned long)lba);
            e = rd(c, rp, lba, n, b);
            if (e == E_HANG) break;
            if (e) { if (!raw_err++) res(R_FAIL, "soak", "read back %s %lu blocks at %lu: error %ld", pname[rp], (unsigned long)n, (unsigned long)lba, (long)e); }
            else if (memcmp(a, b, n * bs)) {
                ULONG k = 0;
                while (a[k] == b[k]) k++;
                if (!raw_bad++) res(R_FAIL, "soak", "raw: wrote %s / read %s %lu blocks at %lu: different from byte %lu", wname[wp], pname[rp],
                                    (unsigned long)n, (unsigned long)lba, (unsigned long)k);
            }
        }
        if (files) {                                           /* one file: write, read back, re-check the last one, delete it */
            ULONG r = rnd(&s) % 100, size, seed = rnd(&s), off = 0, st = seed;
            BPTR fh;
            if (r < 50) size = 1024 + rnd(&s) % (63 * 1024);
            else if (r < 80) size = 65536 + rnd(&s) % (960 * 1024);
            else size = 1048576 + rnd(&s) % (19 * 1048576);
            if (size > fmax) size = 1024 + rnd(&s) % (fmax - 1024);
            strcpy(path, g_wfile);
            {
                char nm[24];
                sprintf(nm, "flysuite.%03lu", (unsigned long)(f_n % 1000));
                AddPart((STRPTR)path, (STRPTR)nm, sizeof(path));
            }
            beat(j, "soak: Open %s (new)", path);
            fh = Open((STRPTR)path, MODE_NEWFILE);
            if (!fh) { res(R_FAIL, "soak", "can't create %s (error %ld)", path, (long)IoErr()); files = 0; continue; }
            while (off < size) {
                ULONG n = umin(65536, size - off), t0, dt;
                LONG put;
                genfill(a, n, &st);
                beat(j, "soak: Write %s, %lu bytes at %lu", path, (unsigned long)n, (unsigned long)off);
                t0 = now_ms();
                put = Write(fh, a, n);
                dt = now_ms() - t0;
                if (dt > OUTLIER_MS) { char bb[160]; Forbid(); n_outl++; Permit(); sprintf(bb, "OUTLIER soak    Write(%s, %lu at %lu) took %lu ms\n", path, (unsigned long)n, (unsigned long)off, (unsigned long)dt); emit(1, bb); }
                if (dt > j->maxms) j->maxms = dt;
                if (put != (LONG)n) { res(R_FAIL, "soak", "Write %s at %lu: %ld of %lu (error %ld)", path, (unsigned long)off, (long)put, (unsigned long)n, (long)IoErr()); f_bad++; break; }
                off += n;
            }
            beat(j, "soak: Close %s", path);
            Close(fh);
            if (off == size) {
                file_verify(j, c, path, size, seed, b, a, &f_bad);
                f_n++;
                f_bytes += size;
            }
            if (prev[0]) {
                file_verify(j, c, prev, prev_size, prev_seed, b, a, &f_bad);
                beat(j, "soak: DeleteFile %s", prev);
                DeleteFile((STRPTR)prev);
            }
            strcpy(prev, path); prev_size = size; prev_seed = seed;
            if (f_bad) files = 0;
        }
        if (!g_rwcount && !files) {                            /* read-only */
            ULONG n = 1 + rnd(&s) % umin(256, blocks), lba = rnd(&s) % (blocks - n + 1);
            int p = rnd(&s) % 4, q = (p + 1 + rnd(&s) % 3) % 4;
            LONG e;
            p = reach(c, p, lba, n);
            q = reach(c, q, lba, n);
            beat(j, "soak: %s %lu blocks at %lu", pname[p], (unsigned long)n, (unsigned long)lba);
            e = rd(c, p, lba, n, a);
            if (e == E_HANG) break;
            ro_n++;
            if (!e) {
                beat(j, "soak: %s %lu blocks at %lu", pname[q], (unsigned long)n, (unsigned long)lba);
                e = rd(c, q, lba, n, b);
                if (e == E_HANG) break;
            }
            if (e) { if (!ro_err++) res(R_FAIL, "soak", "read %lu blocks at %lu: error %ld", (unsigned long)n, (unsigned long)lba, (long)e); }
            else if (memcmp(a, b, n * bs) || (pattern && !pattern_ok(a, lba, n))) {
                if (!ro_bad++) res(R_FAIL, "soak", "%lu blocks at %lu: %s vs %s %s", (unsigned long)n, (unsigned long)lba, pname[p], pname[q],
                                   memcmp(a, b, n * bs) ? "differ" : "agree but not the pattern");
            }
        }
        if (now_ms() > nextrep) {
            ULONG el = (now_ms() - t_start) / 1000;
            nextrep += 60000UL;
            say("    soak %lu:%02lu - raw %lu (%lu bad), files %lu (%lu KB, %lu bad), reads %lu (%lu bad), max %lu ms",
                (unsigned long)(el / 60), (unsigned long)(el % 60), (unsigned long)raw_w, (unsigned long)(raw_bad + raw_err),
                (unsigned long)f_n, (unsigned long)(f_bytes >> 10), (unsigned long)f_bad, (unsigned long)ro_n,
                (unsigned long)(ro_bad + ro_err), (unsigned long)umin(c->maxms > j->maxms ? c->maxms : j->maxms, 0xffffffffUL));
        }
    }
    if (prev[0]) {
        if (!c->stuck && files) file_verify(j, c, prev, prev_size, prev_seed, b, a, &f_bad);
        beat(j, "soak: DeleteFile %s", prev);
        DeleteFile((STRPTR)prev);
    }
    if (made_dir) { beat(j, "soak: DeleteFile %s", g_wfile); DeleteFile((STRPTR)g_wfile); }
    if (g_wfile && (f_n || f_bad))
        res(f_bad ? R_FAIL : R_PASS, "soak", "files: %lu written + read back + read again later, %lu KB, %lu bad, max call %lu ms",
            (unsigned long)f_n, (unsigned long)(f_bytes >> 10), (unsigned long)f_bad, (unsigned long)j->maxms);
    if (g_rwcount) {
        ULONG k, rbad = 0;
        LONG e = 0;
        res(raw_bad || raw_err ? R_FAIL : R_PASS, "soak", "raw: %lu writes + reads in blocks %lu-%lu, %lu different, %lu errors",
            (unsigned long)raw_w, (unsigned long)g_rwfirst, (unsigned long)(g_rwfirst + g_rwcount - 1), (unsigned long)raw_bad,
            (unsigned long)raw_err);
        if (!c->stuck) {
            for (k = 0; k < g_rwcount && !c->stuck; k += 128) {
                ULONG n = umin(128, g_rwcount - k);
                beat(j, "soak: restoring block %lu", (unsigned long)(g_rwfirst + k));
                if ((e = wr(c, P_TD64, g_rwfirst + k, n, save + k * bs)) != 0) break;
                if ((e = rd(c, P_TD64, g_rwfirst + k, n, b)) != 0) break;
                if (memcmp(b, save + k * bs, n * bs)) rbad++;
            }
            if (nbs & 1) { rd(c, P_TD64, g_rwfirst - 1, 1, a); if (memcmp(a, nb, bs)) rbad++; }
            if (nbs & 2) { rd(c, P_TD64, g_rwfirst + g_rwcount, 1, a); if (memcmp(a, nb + bs, bs)) rbad++; }
            res(e || rbad ? R_FAIL : R_PASS, "soak", "raw: the range written back as it was and read back%s (error %ld, %lu bad), neighbours unchanged",
                e || rbad ? " - NOT RESTORED" : "", (long)e, (unsigned long)rbad);
        } else
            res(R_FAIL, "soak", "raw: the unit got stuck: blocks %lu-%lu are NOT restored", (unsigned long)g_rwfirst,
                (unsigned long)(g_rwfirst + g_rwcount - 1));
    }
    if (!g_rwcount && !files && ro_n)
        res(ro_bad || ro_err ? R_FAIL : R_PASS, "soak", "read-only: %lu random reads, each twice, %lu different, %lu errors, max %lu ms",
            (unsigned long)ro_n, (unsigned long)ro_bad, (unsigned long)ro_err, (unsigned long)c->maxms);
    if (c->outliers) say("    soak: %lu requests over 2 s", (unsigned long)c->outliers);
out:
    if (c->stuck) return;
    gfree(a); gfree(b); gfree(save); gfree(nb);
    ctx_free(c);
}

static void t_soak(void)
{
    struct Job *j = job_new();
    grp = "soak";
    if (!j) return;
    say("    soak for %lu minute%s", (unsigned long)g_minutes, g_minutes == 1 ? "" : "s");
    if (job_start(j, j_soak)) wait_jobs(&j, 1, g_tmo + 15000);
}

/* ---- group 6: the toolbox ----------------------------------------------- */

static void t_toolbox(void)
{
    struct Ctx *c = M;
    UBYTE *list, *a, *b;
    LONG e;
    int count = 0, nent, i, k;
    grp = "toolbox";
    if (!U.toolbox) { res(R_SKIP, grp, "not a BlueSCSI/ZuluSCSI Toolbox unit (\"%s\" \"%s\"; TOOLBOX forces the probe)", U.vendor, U.product); return; }
    list = gbuf(100 * TB_ENT, MEMF_ANY); a = gbuf(4096, MEMF_ANY); b = gbuf(4096, MEMF_ANY);
    if (!list || !a || !b) { res(R_SKIP, grp, "no memory"); goto out; }
    e = tb_count(c, a, &count);
    CK(e == 0, "COUNT ($D2): %d files", count);
    memset(list, 0, 100 * TB_ENT); guard_set(list, 100 * TB_ENT);
    e = tb_list(c, list, 100 * TB_ENT);
    nent = e ? 0 : ACT(c) / TB_ENT;
    CK(e == 0 && ACT(c) % TB_ENT == 0 && nent == (count > 100 ? 100 : count) && guard_ok(list, 100 * TB_ENT),
       "LIST ($D0): %lu bytes = %d entries of %d (COUNT said %d)", (unsigned long)ACT(c), nent, TB_ENT, count);
    for (i = 0, k = 0; i < nent; i++) if (list[i * TB_ENT] != i) k++;
    CK(k == 0, "LIST entries numbered 0..%d in order (%d out of place)", nent - 1, k);
    for (i = 0; i < nent && i < 6; i++)
        say("    %2d %s %-32.32s %lu", list[i * TB_ENT], list[i * TB_ENT + 1] ? "dir " : "file", (char *)list + i * TB_ENT + 2,
            (unsigned long)tb_size(list + i * TB_ENT));
    if (nent > 32) tb_byte1(grp, c, list, nent);
    {
        int idx[5], ni = 0;
        idx[ni++] = 0; if (nent > 1) idx[ni++] = 1; if (nent > 31) idx[ni++] = 31; if (nent > 33) idx[ni++] = 33; if (nent > 2) idx[ni++] = nent - 1;
        for (k = 0; k < ni; k++) {
            ULONG g1, g2, want;
            i = idx[k];
            if (list[i * TB_ENT + 1]) continue;
            want = umin(tb_size(list + i * TB_ENT), 4096);
            memset(a, 0, 4096); guard_set(a, 4096);
            e = tb_get(c, i, 0, a); g1 = ACT(c);
            if (HUNG()) goto out;
            memset(b, 0, 4096);
            tb_get(c, i, 0, b); g2 = ACT(c);
            CK(e == 0 && g1 == want && g2 == g1 && !memcmp(a, b, g1) && guard_ok(a, 4096),
               "GET ($D1) file %d \"%.32s\" twice: %lu and %lu bytes (want %lu), %s", i, (char *)list + i * TB_ENT + 2,
               (unsigned long)g1, (unsigned long)g2, (unsigned long)want, memcmp(a, b, g1) ? "DIFFERENT" : "same");
        }
    }
out:
    gfree(list); gfree(a); gfree(b);
}

/* ---- group 7: the fuzzer ------------------------------------------------ */

/* never sent: writes, format, erase, mode/log select, reserve/release,
 * start/stop and prevent/allow, diagnostics, cache locks, copies, ATA
 * pass-through, security out, every vendor-specific opcode */
static const UBYTE fz_never[] = {
    0x02, 0x04, 0x06, 0x07, 0x09, 0x0a, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1b, 0x1d, 0x1e, 0x20, 0x21, 0x22, 0x24, 0x26, 0x27, 0x29, 0x2a, 0x2c, 0x2e, 0x36, 0x39, 0x3a,
    0x3b, 0x3d, 0x3f, 0x40, 0x41, 0x42, 0x48, 0x4c, 0x50, 0x51, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x5b, 0x5c,
    0x5d, 0x5f, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x85, 0x87, 0x89, 0x8a, 0x8b, 0x8d, 0x8e, 0x92, 0x93, 0x94, 0x9a,
    0x9b, 0x9c, 0x9d, 0x9f, 0xa1, 0xa4, 0xa5, 0xa6, 0xa7, 0xa9, 0xaa, 0xac, 0xae, 0xb5, 0xb6, 0xbb, 0xbf, 0
};
/* the usual read-only commands, picked more often */
static const UBYTE fz_known[] = {
    0x00, 0x01, 0x03, 0x08, 0x0b, 0x12, 0x1a, 0x1c, 0x23, 0x25, 0x28, 0x2b, 0x2f, 0x34, 0x35, 0x37, 0x3c, 0x3e,
    0x43, 0x46, 0x4a, 0x4d, 0x52, 0x5a, 0x5e, 0x88, 0x8f, 0x90, 0x9e, 0xa0, 0xa3, 0xa8, 0xaf, 0xb7, 0xbd, 0xbe, 0
};

static int fz_ok(UBYTE op)
{
    const UBYTE *p;
    if (op >= 0xc0) return 0;
    for (p = fz_never; *p; p++) if (*p == op) return 0;
    return 1;
}

static void t_fuzz(void)
{
    struct Ctx *c = M;
    ULONG s = g_seed ^ 0xf022UL, i, nst[4] = { 0, 0, 0, 0 }, maxms = 0, bad = 0, sent = 0;
    UBYTE *buf = gbuf(8192, MEMF_ANY), *ref = gbuf(U.bs, MEMF_ANY), *after = gbuf(U.bs, MEMF_ANY), cdb[16];
    grp = "fuzz";
    if (!g_fuzz) { res(R_SKIP, grp, "needs the FUZZ switch"); goto out; }
    if (!buf || !ref || !after) { res(R_SKIP, grp, "no memory"); goto out; }
    if (U.blocks && rd(c, P_CMD, 0, 1, ref)) { res(R_FAIL, grp, "can't read block 0 before fuzzing"); goto out; }
    say("    fuzz: %lu random non-write CDBs, seed %lu (SEED=%lu repeats it)", (unsigned long)g_fuzzn, (unsigned long)g_seed,
        (unsigned long)g_seed);
    for (i = 0; i < g_fuzzn && !brk() && !g_stuck; i++) {
        UBYTE op, flags;
        int len, k;
        ULONG dl, r = rnd(&s) % 100;
        LONG e;
        do {
            op = r < 60 ? fz_known[rnd(&s) % (sizeof(fz_known) - 1)] : (UBYTE)rnd(&s);
        } while (!fz_ok(op));
        for (k = 0; k < 16; k++) cdb[k] = (UBYTE)rnd(&s);
        cdb[0] = op;
        switch (op >> 5) {
        case 0: len = 6; cdb[4] %= 17; if (op == 0x08 && !cdb[4]) cdb[4] = 1; break;
        case 1: case 2: len = 10; cdb[6] = 0; cdb[7] = 0; cdb[8] %= 65; break;
        case 4: len = 16; cdb[10] = cdb[11] = cdb[12] = 0; cdb[13] %= 65; break;
        case 5: len = 12; cdb[6] = cdb[7] = cdb[8] = 0; cdb[9] %= 65; break;
        default: len = 6 + rnd(&s) % 11; break;
        }
        if (U.blocks && (rnd(&s) & 1)) {                        /* half of them at a real LBA */
            ULONG lba = rnd(&s) % U.blocks;
            if (len == 6) { cdb[1] = (cdb[1] & 0xe0) | (UBYTE)((lba >> 16) & 0x1f); cdb[2] = (UBYTE)(lba >> 8); cdb[3] = (UBYTE)lba; }
            else if (len == 16) { put32(cdb + 2, 0); put32(cdb + 6, lba); }
            else put32(cdb + 2, lba);
        }
        cdb[1] &= 0x1f;                                         /* LUN bits: the driver's */
        if (rnd(&s) % 10 == 0) len = rnd(&s) % 17;              /* a CmdLength that does not fit the group */
        k = rnd(&s) % 10;
        dl = k == 0 ? 0 : k == 1 ? 1 : k == 2 ? 36 : k == 3 ? 255 : k == 4 ? 512 : k == 5 ? 4096 : rnd(&s) % 8192;
        flags = (rnd(&s) % 5) ? SCSIF_READ : SCSIF_WRITE;
        if (rnd(&s) & 1) flags |= SCSIF_AUTOSENSE;
        genfill(buf, dl, &s);
        guard_set(buf, dl);
        scsi_setup(c, cdb, len, dl ? buf : NULL, dl, flags, (flags & SCSIF_AUTOSENSE) ? (rnd(&s) & 1 ? 18 : 255) : 0);
        e = bio(c, HD_SCSICMD, c->sc, sizeof(struct SCSICmd), 0, 0);
        sent++;
        if (e == E_HANG) break;
        if (c->lastms > maxms) maxms = c->lastms;
        nst[e == 0 ? 0 : e == HFERR_BadStatus ? 1 : e == HFERR_SelTimeout ? 2 : 3]++;
        if (ACT(c) > dl || !guard_ok(buf, dl) || SACT(c) > c->sc->scsi_SenseLength || !guard_ok(c->sense, 256)) {
            char d[120];
            desc(c, d);
            if (bad++ < 5) res(R_FAIL, grp, "%s: actual %lu, sense %u of %u, guards %s", d, (unsigned long)ACT(c), SACT(c),
                               c->sc->scsi_SenseLength, (guard_ok(buf, dl) && guard_ok(c->sense, 256)) ? "ok" : "OVERRUN");
        }
        if (e == HFERR_SelTimeout && nst[2] == 1) {
            char d[120];
            desc(c, d);
            say("    first selection failure: %s", d);
        }
        if ((i + 1) % 100 == 0) say("    fuzz %lu: %lu good, %lu CHECK CONDITION/bad status, %lu no target, %lu other",
                                    (unsigned long)(i + 1), (unsigned long)nst[0], (unsigned long)nst[1], (unsigned long)nst[2],
                                    (unsigned long)nst[3]);
    }
    if (c->hangs || c->stuck) { res(R_FAIL, grp, "%lu of %lu CDBs sent before a HANG", (unsigned long)sent, (unsigned long)g_fuzzn); goto out; }
    res(bad ? R_FAIL : R_PASS, grp, "%lu random CDBs, every one back in time (max %lu ms): %lu good, %lu bad status, %lu no target, %lu other; %lu overruns",
        (unsigned long)sent, (unsigned long)maxms, (unsigned long)nst[0], (unsigned long)nst[1], (unsigned long)nst[2],
        (unsigned long)nst[3], (unsigned long)bad);
    {
        int st = unit_ready(c);
        LONG e = U.blocks ? rd(c, P_CMD, 0, 1, after) : 0;
        CK(st == 0 && e == 0 && (!U.blocks || !memcmp(ref, after, U.bs)), "after the fuzz: TEST UNIT READY %d, block 0 %s", st,
           e ? "unreadable" : U.blocks && memcmp(ref, after, U.bs) ? "CHANGED" : "the same");
    }
out:
    gfree(buf); gfree(ref); gfree(after);
}

/* ---- group 9: mix - SCSI and IDE, reads and writes, raw and files, at once --
 * Phases of SECONDS each: every combination of SCSI read/write with IDE
 * read/write, two SCSI units writing, then file copies through the file
 * systems (FILES=) on top. A unit writes only inside its own range
 * (RAWWRITE / RAWWRITE2 / RAWIDE, at most 1 MB each), saved before the first
 * phase and written back + verified after the last one, also after an error
 * or a break. Every worker verifies what it reads; a writer keeps a shadow
 * of its range and checks the whole range at the end of each phase. */

struct MixUnit {
    LONG unit;
    ULONG first, count, blocks, bs;
    UBYTE *save, *shadow;
    int present, pattern;
    ULONG xlo, xhi;                   /* this phase: a file worker writes in these blocks (a volume): */
    int xon;                          /* raw reads there are not compared (the file system moves them) */
    struct Ctx *c;                    /* the main task's, for the save and the restore */
    const char *role;
};
static struct MixUnit MU[3];
static ULONG g_rw2first, g_rw2count, g_rwifirst, g_rwicount;
static const char *g_files;

/* block b of a mix unit as it should read */
static int mix_block_ok(struct MixUnit *mu, ULONG lba, const UBYTE *p)
{
    if (mu->count && lba >= mu->first && lba < mu->first + mu->count)
        return !memcmp(p, mu->shadow + (lba - mu->first) * mu->bs, mu->bs);
    if (mu->pattern) return pattern_ok(p, lba, 1);
    return -1;                        /* nothing known */
}

static void j_mix(struct Job *j)
{
    struct MixUnit *mu = &MU[j->idx];
    struct Ctx *c;
    char tag[24];
    ULONG bs, s = g_seed + 991 * (j->idx + 1) + j->n4, end, maxn, wn = 0, rn = 0, bad = 0, errs = 0, kb = 0, k;
    UBYTE *a = NULL, *b = NULL;
    sprintf(tag, "mix-%ld", (long)mu->unit);
    c = ctx_new(tag);
    if (!c) { j->err = 1; return; }
    beat(j, "%s: OpenDevice", tag);
    if (OpenDevice((STRPTR)g_dev, mu->unit, (struct IORequest *)c->io, 0)) {
        res(R_FAIL, "mix", "%s: can't open unit %ld", tag, (long)mu->unit);
        j->err = 1;
        ctx_free(c);
        return;
    }
    c->opened = 1;
    bs = mu->bs;
    ctx_bs(c, bs);
    maxn = umin(128, mu->blocks);
    if (!(a = gbuf(maxn * bs, MEMF_ANY)) || !(b = gbuf(maxn * bs, MEMF_ANY))) { j->err = 1; goto out; }
    end = now_ms() + j->n5 * 1000;
    while (!g_break && !g_stuck && !c->stuck && now_ms() < end) {
        int p = rnd(&s) % 4, q = (p + 1 + rnd(&s) % 3) % 4;
        LONG e;
        if (j->kind == 2 && (rnd(&s) & 1)) {                  /* a write in the range, read back */
            ULONG n = 1 + rnd(&s) % umin(maxn, mu->count), lba = mu->first + rnd(&s) % (mu->count - n + 1), st = rnd(&s);
            genfill(a, n * bs, &st);
            beat(j, "%s: %s %lu blocks at %lu", tag, wname[p], (unsigned long)n, (unsigned long)lba);
            e = wr_r(c, p, lba, n, a, mu->first, mu->count);
            if (e == E_HANG || e == E_REFUSE) break;
            wn++;
            kb += n * bs >> 10;
            if (e) {
                if (!errs++) res(R_FAIL, "mix", "%s: %s %lu blocks at %lu: error %ld", tag, wname[p], (unsigned long)n, (unsigned long)lba, (long)e);
                if (rd(c, P_TD64, lba, n, b) == 0) memcpy(mu->shadow + (lba - mu->first) * bs, b, n * bs);   /* what is there now */
                continue;
            }
            memcpy(mu->shadow + (lba - mu->first) * bs, a, n * bs);
            q = reach(c, q, lba, n);
            beat(j, "%s: %s %lu blocks at %lu (read back)", tag, pname[q], (unsigned long)n, (unsigned long)lba);
            e = rd(c, q, lba, n, b);
            if (e == E_HANG) break;
            if (e) { if (!errs++) res(R_FAIL, "mix", "%s: read back %s at %lu: error %ld", tag, pname[q], (unsigned long)lba, (long)e); }
            else if (memcmp(a, b, n * bs)) {
                for (k = 0; k < n * bs && a[k] == b[k]; k++) ;
                if (bad++ < 3) res(R_FAIL, "mix", "%s: wrote %s / read %s %lu blocks at %lu: DIFFERENT from byte %lu (block %lu)", tag,
                                   wname[p], pname[q], (unsigned long)n, (unsigned long)lba, (unsigned long)k, (unsigned long)(lba + k / bs));
            }
        } else {                                               /* a read anywhere, twice, checked */
            ULONG n = 1 + rnd(&s) % maxn, lba = rnd(&s) % (mu->blocks - n + 1);
            int okb = 1;
            p = reach(c, p, lba, n);
            q = reach(c, q, lba, n);
            beat(j, "%s: %s %lu blocks at %lu", tag, pname[p], (unsigned long)n, (unsigned long)lba);
            e = rd(c, p, lba, n, a);
            if (e == E_HANG) break;
            rn++;
            kb += n * bs >> 10;
            if (!e) {
                beat(j, "%s: %s %lu blocks at %lu", tag, pname[q], (unsigned long)n, (unsigned long)lba);
                e = rd(c, q, lba, n, b);
                if (e == E_HANG) break;
            }
            if (e) { if (!errs++) res(R_FAIL, "mix", "%s: %s/%s %lu blocks at %lu: error %ld", tag, pname[p], pname[q], (unsigned long)n, (unsigned long)lba, (long)e); continue; }
            if (mu->xon && lba <= mu->xhi && lba + n - 1 >= mu->xlo) { j->n4 += 0; continue; }   /* a live volume: read for load only */
            for (k = 0; k < n; k++) if (mix_block_ok(mu, lba + k, a + k * bs) == 0) { okb = 0; break; }
            if (memcmp(a, b, n * bs) || !okb) {

                if (bad++ < 3) res(R_FAIL, "mix", "%s: %lu blocks at %lu: %s", tag, (unsigned long)n, (unsigned long)lba,
                                   memcmp(a, b, n * bs) ? "two reads DIFFER" : "not what the block should hold");
            }
        }
    }
    if (j->kind == 2 && !c->stuck && !g_stuck) {              /* the whole range against the shadow */
        ULONG off, rbad = 0;
        for (off = 0; off < mu->count && !c->stuck; off += maxn) {
            ULONG n = umin(maxn, mu->count - off);
            beat(j, "%s: checking the range, block %lu", tag, (unsigned long)(mu->first + off));
            if (rd(c, P_TD64, mu->first + off, n, a) || memcmp(a, mu->shadow + off * bs, n * bs)) rbad++;
        }
        if (rbad) { bad += rbad; res(R_FAIL, "mix", "%s: the range does not hold what was written (%lu chunks differ)", tag, (unsigned long)rbad); }
    }
    j->n1 = wn; j->n2 = rn; j->n3 = kb;
    j->err = bad || errs || c->hangs;
    res(j->err ? R_FAIL : R_PASS, "mix", "%s unit %ld %s: %lu writes, %lu reads, %lu KB, %lu wrong, %lu errors, %lu hangs, %lu outliers, max %lu ms",
        mu->role, (long)mu->unit, j->kind == 2 ? "read+write" : "read", (unsigned long)wn, (unsigned long)rn, (unsigned long)kb,
        (unsigned long)bad, (unsigned long)errs, (unsigned long)c->hangs, (unsigned long)c->outliers, (unsigned long)c->maxms);
out:
    if (c->stuck) return;
    gfree(a); gfree(b);
    ctx_free(c);
}

/* file copies in a drawer of its own under dir: write, read back, read the previous again, delete */
static void j_mixfile(struct Job *j)
{
    char dir[200], path[256], prev[256], nm[32];
    ULONG s = g_seed + 7777 + j->n4 + j->idx, end, nfile = 0, kbytes = 0, bad = 0, prev_size = 0, prev_seed = 0, fmax = 2UL * 1048576;
    UBYTE *a = gbuf(65536, MEMF_ANY), *b = gbuf(65536, MEMF_ANY);
    BPTR l;
    struct InfoData *id = AllocMem(sizeof(struct InfoData), MEMF_PUBLIC | MEMF_CLEAR);
    prev[0] = 0;
    strcpy(dir, (char *)j->c);                                 /* the dir name rides in j->c */
    sprintf(nm, "flysuite.mix%d", j->idx);
    AddPart((STRPTR)dir, (STRPTR)nm, sizeof(dir));
    if (!a || !b || !id) { j->err = 1; goto out; }
    beat(j, "files: CreateDir %s", dir);
    l = CreateDir((STRPTR)dir);
    if (!l) { res(R_FAIL, "mix", "files: can't make %s (error %ld)", dir, (long)IoErr()); j->err = 1; goto out; }
    beat(j, "files: Info %s", dir);
    if (Info(l, id)) {
        unsigned long long fr = (unsigned long long)(id->id_NumBlocks - id->id_NumBlocksUsed) * id->id_BytesPerBlock;
        if (fr / 4 < fmax) fmax = (ULONG)(fr / 4);
    }
    UnLock(l);
    if (fmax < 16384) { res(R_SKIP, "mix", "files: %s has too little room", dir); goto deldir; }
    end = now_ms() + j->n5 * 1000;
    while (!g_break && !g_stuck && now_ms() < end && !bad) {
        ULONG size = 1024 + rnd(&s) % (fmax - 1024), seed = rnd(&s), st = seed, off = 0;
        BPTR fh;
        if (rnd(&s) & 1) size = 1024 + rnd(&s) % 65536;        /* half of them small */
        strcpy(path, dir);
        sprintf(nm, "f%03lu", (unsigned long)(nfile % 1000));
        AddPart((STRPTR)path, (STRPTR)nm, sizeof(path));
        beat(j, "files: Open %s", path);
        if (!(fh = Open((STRPTR)path, MODE_NEWFILE))) { res(R_FAIL, "mix", "files: can't create %s (%ld)", path, (long)IoErr()); bad++; break; }
        while (off < size) {
            ULONG n = umin(32768, size - off), t0, dt;
            LONG put;
            genfill(a, n, &st);
            beat(j, "files: Write %s, %lu at %lu", path, (unsigned long)n, (unsigned long)off);
            t0 = now_ms();
            put = Write(fh, a, n);
            dt = now_ms() - t0;
            if (dt > j->maxms) j->maxms = dt;
            if (dt > OUTLIER_MS) { char ob[160]; Forbid(); n_outl++; Permit(); sprintf(ob, "OUTLIER mix     Write(%s, %lu at %lu) took %lu ms\n", path, (unsigned long)n, (unsigned long)off, (unsigned long)dt); emit(1, ob); }
            if (put != (LONG)n) { res(R_FAIL, "mix", "files: Write %s at %lu: %ld (error %ld)", path, (unsigned long)off, (long)put, (long)IoErr()); bad++; break; }
            off += n;
        }
        beat(j, "files: Close %s", path);
        Close(fh);
        if (bad) break;
        file_verify(j, NULL, path, size, seed, b, a, &bad);
        if (prev[0]) {
            file_verify(j, NULL, prev, prev_size, prev_seed, b, a, &bad);
            beat(j, "files: DeleteFile %s", prev);
            DeleteFile((STRPTR)prev);
        }
        strcpy(prev, path); prev_size = size; prev_seed = seed;
        nfile++;
        kbytes += size >> 10;
    }
    if (prev[0]) { beat(j, "files: DeleteFile %s", prev); DeleteFile((STRPTR)prev); }
deldir:
    beat(j, "files: DeleteFile %s", dir);
    if (!DeleteFile((STRPTR)dir)) res(R_FAIL, "mix", "files: can't delete %s (%ld)", dir, (long)IoErr());
    j->err = bad != 0;
    res(bad ? R_FAIL : R_PASS, "mix", "files in %s: %lu files, %lu KB written + read back + re-read, %lu bad, max call %lu ms",
        dir, (unsigned long)nfile, (unsigned long)kbytes, (unsigned long)bad, (unsigned long)j->maxms);
out:
    gfree(a); gfree(b);
    if (id) FreeMem(id, sizeof(struct InfoData));
}

static int rawwrite_check_u(struct Ctx *cc, LONG unit, ULONG blocks, ULONG bsz, ULONG first, ULONG count);
static int bstr_eq(BSTR bs, const char *s);
static unsigned long long env_lo(ULONG *env, ULONG bs);
static unsigned long long env_hi(ULONG *env, ULONG bs);


/* the unit of DEVICE a drawer's volume lives on, or -1; on dataflyer.device
 * also a volume on scsi.device unit n, which is its unit n + 100 */
static LONG dir_unit(const char *dir, ULONG *lo, ULONG *hi, ULONG bs)
{
    BPTR l = Lock((STRPTR)dir, ACCESS_READ);
    struct MsgPort *task;
    struct DosList *dl;
    LONG unit = -1;
    if (!l) return -1;
    task = ((struct FileLock *)BADDR(l))->fl_Task;
    UnLock(l);
    dl = LockDosList(LDF_DEVICES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_DEVICES))) {
        struct FileSysStartupMsg *fssm = BADDR(dl->dol_misc.dol_handler.dol_Startup);
        if (dl->dol_Task != task || (LONG)dl->dol_misc.dol_handler.dol_Startup <= 64 || !TypeOfMem(fssm) || !fssm->fssm_Device)
            continue;
        if (bstr_eq(fssm->fssm_Device, g_devbase)) unit = fssm->fssm_Unit;
        else if (g_df && bstr_eq(fssm->fssm_Device, "scsi.device")) unit = fssm->fssm_Unit + 100;
        {
            ULONG *env = BADDR(fssm->fssm_Environ);
            if (env && TypeOfMem(env) && env[DE_TABLESIZE] >= DE_UPPERCYL) { *lo = (ULONG)env_lo(env, bs); *hi = (ULONG)env_hi(env, bs); }
            else { *lo = 0; *hi = 0xffffffffUL; }
        }
        break;
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    return unit;
}

static void mix_restore(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        struct MixUnit *mu = &MU[i];
        ULONG off, rbad = 0;
        LONG e = 0;
        if (!mu->present || !mu->count || !mu->save) continue;
        if (!mu->c || mu->c->stuck) {
            res(R_FAIL, "mix", "unit %ld: the request got stuck - blocks %lu-%lu are NOT restored", (long)mu->unit,
                (unsigned long)mu->first, (unsigned long)(mu->first + mu->count - 1));
            continue;
        }
        for (off = 0; off < mu->count; off += 128) {
            ULONG n = umin(128, mu->count - off);
            if ((e = wr_r(mu->c, P_TD64, mu->first + off, n, mu->save + off * mu->bs, mu->first, mu->count)) != 0) break;
            if ((e = rd(mu->c, P_TD64, mu->first + off, n, mu->shadow + off * mu->bs)) != 0) break;
            if (memcmp(mu->shadow + off * mu->bs, mu->save + off * mu->bs, n * mu->bs)) rbad++;
        }
        res(e || rbad ? R_FAIL : R_PASS, "mix", "unit %ld: blocks %lu-%lu written back as they were and verified%s (error %ld, %lu bad)",
            (long)mu->unit, (unsigned long)mu->first, (unsigned long)(mu->first + mu->count - 1), e || rbad ? " - NOT RESTORED" : "",
            (long)e, (unsigned long)rbad);
        mu->count = 0;                                    /* never twice */
    }
    for (i = 0; i < 3; i++) {
        struct MixUnit *mu = &MU[i];
        if (!mu->c || mu->c->stuck) continue;
        gfree(mu->save); gfree(mu->shadow);
        mu->save = mu->shadow = NULL;
        ctx_free(mu->c);
        mu->c = NULL;
    }
}

static void t_mix(void)
{
    static const struct { const char *name; UBYTE s1, s2, ide, files; } ph[8] = {
        { "SCSI read + IDE read", 1, 0, 1, 0 },
        { "SCSI write + IDE read", 2, 0, 1, 0 },
        { "SCSI read + IDE write", 1, 0, 2, 0 },
        { "SCSI write + IDE write", 2, 0, 2, 0 },
        { "two SCSI units write + IDE read", 2, 2, 1, 0 },
        { "two SCSI units write + IDE write", 2, 2, 2, 0 },
        { "all raw writes + file copies", 2, 2, 2, 1 },
        { "raw reads + file copies", 1, 1, 1, 1 },
    };
    const char *fdir[4];
    LONG funit[4];
    ULONG flo[4], fhi[4];
    char fbuf[256];
    int nf = 0, i, p, bad = 0;
    grp = "mix";
    memset(MU, 0, sizeof(MU));
    MU[0].unit = g_unit; MU[0].first = g_rwfirst; MU[0].count = g_rwcount; MU[0].role = "SCSI";
    MU[1].unit = g_unit2; MU[1].first = g_rw2first; MU[1].count = g_rw2count; MU[1].role = "SCSI2";
    MU[2].unit = g_ide; MU[2].first = g_rwifirst; MU[2].count = g_rwicount; MU[2].role = "IDE";
    if (g_df && g_unit >= 100) { MU[0].role = "IDE"; MU[2].unit = -1; }
    if (g_files) {
        char *q;
        strncpy(fbuf, g_files, sizeof(fbuf) - 1);
        fbuf[sizeof(fbuf) - 1] = 0;
        for (q = fbuf; *q && nf < 4; ) {
            fdir[nf++] = q;
            while (*q && *q != ',') q++;
            if (*q) *q++ = 0;
        }
    }
    for (i = 0; i < nf; i++) {
        funit[i] = dir_unit(fdir[i], &flo[i], &fhi[i], 512);
        say("    mix: FILES %s is on %s unit %ld", fdir[i], g_dev, (long)funit[i]);
    }
    /* open, size, check and save every unit */
    for (i = 0; i < 3; i++) {
        struct MixUnit *mu = &MU[i];
        ULONG bs = 512, off;
        UBYTE *b;
        if (mu->unit < 0 || (i > 0 && mu->unit == g_unit) || (i == 2 && mu->unit == g_unit2)) continue;
        if (!(mu->c = ctx_new("mixmain"))) continue;
        if (open_bounded(mu->c, mu->unit, 0, NULL) != 0) {
            if (i == 2) say("    mix: no IDE unit %ld", (long)mu->unit);
            else res(R_FAIL, grp, "mix: unit %ld does not open", (long)mu->unit);
            if (!mu->c->stuck) ctx_free(mu->c);
            mu->c = NULL;
            if (g_stuck) return;
            continue;
        }
        mu->blocks = ctx_geom(mu->c, &bs);
        mu->bs = bs;
        if (!mu->blocks || bs > 4096) { res(R_SKIP, grp, "mix: unit %ld has no medium", (long)mu->unit); continue; }
        if ((b = gbuf(bs, MEMF_ANY))) {
            mu->pattern = bs == 512 && rd(mu->c, P_CMD, 0, 1, b) == 0 && pattern_ok(b, 0, 1) && rd(mu->c, P_TD64, mu->blocks - 1, 1, b) == 0 &&
                          pattern_ok(b, mu->blocks - 1, 1);
            gfree(b);
        }
        mu->present = 1;
        if (mu->count) {
            if ((ULONG)mu->count * bs > 1048576UL) { res(R_FAIL, grp, "mix: unit %ld's range is more than 1 MB - refused", (long)mu->unit); mu->count = 0; bad = 1; continue; }
            int rc_ = rawwrite_check_u(mu->c, mu->unit, mu->blocks, bs, mu->first, mu->count);
            if (rc_ && ((rc_ & 2) || !g_iknow)) {
                res(R_FAIL, grp, "mix: unit %ld blocks %lu-%lu refused (see above): no writes on it", (long)mu->unit, (unsigned long)mu->first,
                    (unsigned long)(mu->first + mu->count - 1));
                mu->count = 0; bad = 1; continue;
            }
            mu->save = gbuf(mu->count * bs, MEMF_ANY);
            mu->shadow = gbuf(mu->count * bs, MEMF_ANY);
            if (!mu->save || !mu->shadow) { res(R_FAIL, grp, "mix: no memory to save unit %ld's range", (long)mu->unit); mu->count = 0; bad = 1; continue; }
            for (off = 0; off < mu->count; off += 128)
                if (rd(mu->c, P_TD64, mu->first + off, umin(128, mu->count - off), mu->save + off * bs)) break;
            if (off < mu->count) { res(R_FAIL, grp, "mix: can't save unit %ld's range", (long)mu->unit); gfree(mu->save); mu->save = NULL; mu->count = 0; bad = 1; continue; }
            memcpy(mu->shadow, mu->save, mu->count * bs);
        }
        say("    mix: %s unit %ld, %lu blocks%s; %s", mu->role, (long)mu->unit, (unsigned long)mu->blocks, mu->pattern ? " (pattern)" : "",
            mu->count ? "writes in a saved range" : "read only");
        if (mu->count) say("         range %lu-%lu (%lu KB) saved", (unsigned long)mu->first, (unsigned long)(mu->first + mu->count - 1),
                           (unsigned long)((mu->count * bs) >> 10));
    }
    if (bad) { mix_restore(); return; }
    for (p = 0; p < 8 && !g_break && !g_stuck; p++) {
        struct Job *js[8];
        int n = 0, need = 0, why = 0, k;
        int mode[3], fw[4];
        mode[0] = ph[p].s1; mode[1] = ph[p].s2; mode[2] = ph[p].ide;
        for (k = 0; k < nf; k++) fw[k] = ph[p].files;
        for (i = 0; i < 3; i++) {
            if (!mode[i]) continue;
            if (!MU[i].present) { if (i == 1 && mode[i] == 1) { mode[i] = 0; continue; } why = i + 1; break; }
            if (mode[i] == 2 && !MU[i].count) {
                /* no raw range: the writes come from file copies on a volume of that unit */
                for (k = 0; k < nf; k++) if (funit[k] == MU[i].unit) break;
                if (k == nf) { why = 10 + i; break; }
                fw[k] = 1;
                mode[i] = 1;
            }
        }
        if (ph[p].files && !nf) why = 20;
        if (why) {
            res(R_SKIP, grp, "phase %d \"%s\": %s", p + 1, ph[p].name,
                why == 20 ? "needs FILES=" : why >= 10 ? (why == 10 ? "needs RAWWRITE or a FILES dir on UNIT" : why == 11 ? "needs RAWWRITE2 or a FILES dir on UNIT2" : "needs RAWIDE or a FILES dir on the IDE unit")
                : why == 2 ? "needs UNIT2" : why == 3 ? "no IDE unit" : "no unit");
            continue;
        }
        say("    mix phase %d: %s, %lu s", p + 1, ph[p].name, (unsigned long)g_secs);
        for (i = 0; i < 3; i++) {             /* raw reads inside a volume being written are not compared */
            MU[i].xon = 0;
            for (k = 0; k < nf; k++)
                if (fw[k] && funit[k] == MU[i].unit) { MU[i].xon = 1; MU[i].xlo = flo[k] * 512 / (MU[i].bs ? MU[i].bs : 512); MU[i].xhi = fhi[k] * 512 / (MU[i].bs ? MU[i].bs : 512); }
        }
        for (i = 0; i < 3; i++) {
            struct Job *j;
            if (!mode[i] || !(j = job_new())) continue;
            j->idx = i; j->kind = mode[i]; j->n5 = g_secs; j->n4 = p;
            if (job_start(j, j_mix)) js[n++] = j;
            need++;
        }
        for (i = 0; i < nf; i++) {
            struct Job *j;
            if (!fw[i]) continue;
            if (!(j = job_new())) break;

            j->idx = i; j->n5 = g_secs; j->n4 = p; j->c = (struct Ctx *)fdir[i];
            if (job_start(j, j_mixfile)) js[n++] = j;
            need++;
        }
        if (n < need) res(R_FAIL, grp, "phase %d: only %d of %d workers started", p + 1, n, need);
        (void)0;
        if (!wait_jobs(js, n, g_tmo + 20000)) break;
        for (i = 0, need = 0; i < n; i++) need += js[i]->err ? 1 : 0;
        res(need ? R_FAIL : R_PASS, grp, "phase %d \"%s\": %d workers, %d with problems", p + 1, ph[p].name, n, need);
    }
    mix_restore();
}

/* ---- group 8: the 2026-10-31 audit's findings (docs/audit-2026-10-31.md) --
 * errmap, cdbpass: read-only, in "all". dirmis: its writes only inside
 * RAWWRITE on an emulator. cdlate, raceopen: need a set-up (a late disc, a
 * fresh boot) and SKIP otherwise. slowcmd: START STOP UNIT, named only.
 * atapidir, runend: may wedge or freeze the machine - DANGER, named only,
 * and run last. */


static const char *tdname(LONG e)
{
    switch (e) {
    case 0: return "ok";
    case TDERR_NotSpecified: return "TDERR_NotSpecified";
    case TDERR_BadSecSum: return "TDERR_BadSecSum";
    case TDERR_WriteProt: return "TDERR_WriteProt";
    case TDERR_DiskChanged: return "TDERR_DiskChanged";
    case TDERR_SeekError: return "TDERR_SeekError";
    case TDERR_BadUnitNum: return "TDERR_BadUnitNum";
    case IOERR_BADLENGTH: return "IOERR_BADLENGTH";
    case IOERR_BADADDRESS: return "IOERR_BADADDRESS";
    case HFERR_BadStatus: return "HFERR_BadStatus";
    case HFERR_SelTimeout: return "HFERR_SelTimeout";
    }
    return "?";
}

/* ERRMAP (H5): which io_Error comes back for an address error, NOT READY, a bad opcode */
static void t_errmap(void)
{
    struct Ctx *c = M;
    UBYTE *b = gbuf(4096, MEMF_ANY);
    LONG e;
    int p;
    grp = "audit";
    if (!b || U.bs > 4096) { res(R_SKIP, grp, "ERRMAP: no memory"); goto out; }
    if (U.blocks) {
        for (p = 0; p < 3; p++) {
            e = rd(c, p, U.blocks, 1, b);
            if (HUNG()) goto out;
            if (e == E_RANGE) continue;
            CK(e != 0 && e != TDERR_WriteProt && e != TDERR_BadSecSum,
               "ERRMAP (H5) %s 1 block past the end: io_Error %ld %s (want SeekError/NotSpecified, not a bad-sector or write-protect code)",
               pname[p], (long)e, tdname(e));
        }
        /* the write half: past the end, so the target refuses it; only on a scratch unit on an emulator */
        if (g_rwcount) {
            ULONG lo, hi;
            memset(b, 0, U.bs);
            off64(c, U.blocks, &lo, &hi);
            e = hi ? E_RANGE : bio(c, CMD_WRITE, b, U.bs, lo, 0);
            if (!HUNG() && e != E_RANGE)
                CK(e != 0 && e != TDERR_WriteProt, "ERRMAP (H5) CMD_WRITE 1 block past the end: io_Error %ld %s (want SeekError/NotSpecified, not WriteProt)",
                   (long)e, tdname(e));
            e = bio(c, TD_WRITE64, b, U.bs, lo, hi);
            if (!HUNG())
                CK(e != 0 && e != TDERR_WriteProt, "ERRMAP (H5) TD_WRITE64 1 block past the end: io_Error %ld %s", (long)e, tdname(e));
            e = rd(c, low_path(c, U.blocks - 1, 1), U.blocks - 1, 1, b);
            CK(e == 0, "ERRMAP: the last block reads normally after the refused writes: %ld", (long)e);
        } else
            res(R_SKIP, grp, "ERRMAP write half (writes past the end): needs RAWWRITE (the unit is then scratch)");
    }
    if (U.removable && !U.blocks) {
        e = bio(c, CMD_READ, b, 512, 0, 0);
        CK(e == TDERR_DiskChanged, "ERRMAP (H5) CMD_READ with no medium (NOT READY): io_Error %ld %s (want DiskChanged / no disk)",
           (long)e, tdname(e));
    } else
        res(R_SKIP, grp, "ERRMAP NOT READY: needs a removable unit with no medium (this one: %s)", U.removable ? "medium in" : "fixed");
    {
        UBYTE cdb[6] = { 0xff, 0, 0, 0, 0, 0 };
        e = scsi(c, cdb, 6, NULL, 0, SCSIF_READ | SCSIF_AUTOSENSE, 18);
        CK(e == HFERR_BadStatus && ST(c) == 2, "ERRMAP HD_SCSICMD bad opcode: io_Error %ld %s, status %d", (long)e, tdname(e), ST(c));
        unit_ready(c);
    }
out:
    gfree(b);
}

/* CDBPASS (M1): byte 1's top bits of a standard CDB. A target that has no
 * LUN 1 answers INQUIRY with byte 1 = $20 with qualifier 3 / type $1F; LUN 0's
 * own answer means the driver cleared the bits. */
static void t_cdbpass(void)
{
    struct Ctx *c = M, *x;
    UBYTE b[64], ref[36], cdb[16];
    LONG e, lun1;
    ULONG ms;
    grp = "audit";
    if (g_unit >= 10) { res(R_SKIP, grp, "CDBPASS: a LUN > 0 unit (the LUN bits are the driver's there)"); return; }
    x = ctx_new("open");
    if (!x) return;
    lun1 = open_bounded(x, g_unit + 10, 1, &ms);
    ctx_free(x);
    if (lun1 == E_HANG) return;
    memset(ref, 0, 36);
    e = inquiry(c, ref, 36, 36);
    memset(cdb, 0, 16); cdb[0] = 0x12; cdb[1] = 0x20; cdb[4] = 36;
    memset(b, 0, 64);
    e = scsi(c, cdb, 6, b, 36, SCSIF_READ | SCSIF_AUTOSENSE, 18);
    if (HUNG()) return;
    if (lun1 == 0)
        res(R_SKIP, grp, "CDBPASS (M1): the target answers LUN 1 itself, so INQUIRY byte 1 = $20 can't show what arrived (byte 0 back: $%02x)",
            b[0]);
    else if (e == 0 && ST(c) == 0 && (b[0] & 0xe0) == 0 && !memcmp(b, ref, 36))
        res(R_FAIL, grp, "CDBPASS (M1) reproduced: INQUIRY with byte 1 = $20 got LUN 0's answer (qualifier 0) though the target has no LUN 1: the driver cleared byte 1's top bits");
    else
        res(R_PASS, grp, "CDBPASS (M1): INQUIRY byte 1 = $20 reached the target as LUN 1 (status %d, byte 0 $%02x)", ST(c), b[0]);
    memset(cdb, 0, 16); cdb[1] = 0xe0;
    e = scsi(c, cdb, 6, NULL, 0, SCSIF_READ | SCSIF_AUTOSENSE, 18);
    say("    CDBPASS: TEST UNIT READY with byte 1 = $E0: io_Error %ld, status %d, key %d (no LUN 7 here: GOOD means the bits were cleared)",
        (long)e, ST(c), ST(c) == 2 ? SKEY(c) : 0);
    if (U.blocks && U.bs <= 4096) {
        UBYTE *d = gbuf(U.bs, MEMF_ANY);
        if (d) {
            mk_read(cdb, 10, 0, 1);
            cdb[1] = 0xe0;                                 /* RDPROTECT = 7: reserved, a protection-aware target refuses it */
            e = scsi(c, cdb, 10, d, U.bs, SCSIF_READ | SCSIF_AUTOSENSE, 18);
            say("    CDBPASS: READ(10) with RDPROTECT = 7: io_Error %ld, status %d%s", (long)e, ST(c),
                e == 0 ? " (GOOD: cleared by the driver, or a target that ignores the field)" : "");
            gfree(d);
        }
    }
    unit_ready(c);
}

/* DIRMIS (H6): wrong-direction and over-long data phases */
static void t_dirmis(void)
{
    struct Ctx *c = M;
    ULONG bs = U.bs, blk = g_rwfirst, k;
    UBYTE *save = NULL, *aa = NULL, *rb = NULL, *b55 = NULL, cdb[16];
    LONG e;
    int zeros;
    grp = "audit";
    {   /* read-only: INQUIRY sent with SCSIF_WRITE */
        UBYTE ib[64];
        memset(ib, 0xee, 64);
        memset(cdb, 0, 16); cdb[0] = 0x12; cdb[4] = 36;
        e = scsi(c, cdb, 6, ib, 36, SCSIF_WRITE | SCSIF_AUTOSENSE, 18);
        if (HUNG()) return;
        for (k = 0; k < 36 && ib[k] == 0xee; k++) ;
        if (e == 0 && ACT(c) == 36 && k == 36)
            res(R_FAIL, grp, "DIRMIS (H6) reproduced: INQUIRY sent with SCSIF_WRITE: GOOD, scsi_Actual 36, the buffer untouched (nothing moved)");
        else
            res(R_PASS, grp, "DIRMIS (H6): INQUIRY with SCSIF_WRITE: io_Error %ld, scsi_Actual %lu", (long)e, (unsigned long)ACT(c));
    }
    if (!g_rwcount || g_rwcount < 2 || bs > 4096) {
        res(R_SKIP, grp, "DIRMIS writes: need RAWWRITE (>= 2 blocks; the first two are saved, used and restored)");

        return;
    }
    save = gbuf(2 * bs, MEMF_ANY); aa = gbuf(2 * bs, MEMF_ANY); rb = gbuf(2 * bs, MEMF_ANY); b55 = gbuf(2 * bs, MEMF_ANY);
    if (!save || !aa || !rb || !b55) { res(R_SKIP, grp, "DIRMIS: no memory"); goto out; }
    if (rd(c, P_TD64, blk, 2, save)) { res(R_FAIL, grp, "DIRMIS: can't save blocks %lu-%lu", (unsigned long)blk, (unsigned long)blk + 1); goto out; }
    memset(aa, 0xaa, 2 * bs);
    memset(b55, 0x55, 2 * bs);
    /* 1: WRITE(10) of 1 block sent with SCSIF_READ */
    if (wr(c, P_TD64, blk, 2, aa)) goto restore;
    memset(cdb, 0, 16); cdb[0] = 0x2a; put32(cdb + 2, blk); cdb[8] = 1;
    e = scsi(c, cdb, 10, b55, bs, SCSIF_READ | SCSIF_AUTOSENSE, 18);
    if (HUNG()) goto restore;
    rd(c, P_TD64, blk, 1, rb);
    for (k = 0, zeros = 1; k < bs; k++) if (rb[k]) { zeros = 0; break; }
    if (zeros)
        res(R_FAIL, grp, "DIRMIS (H6) reproduced: WRITE(10) sent with SCSIF_READ (io_Error %ld, status %d, actual %lu) wrote a block of ZEROS",
            (long)e, ST(c), (unsigned long)ACT(c));
    else
        res(rb[0] == 0xaa ? R_PASS : R_FAIL, grp, "DIRMIS (H6): WRITE(10) with SCSIF_READ: io_Error %ld; the block holds $%02x", (long)e, rb[0]);
    /* 2: WRITE(10) of 2 blocks with scsi_Length of 1 block */
    if (wr(c, P_TD64, blk, 2, aa)) goto restore;
    cdb[8] = 2;
    e = scsi(c, cdb, 10, b55, bs, SCSIF_WRITE | SCSIF_AUTOSENSE, 18);
    if (HUNG()) goto restore;
    rd(c, P_TD64, blk, 2, rb);
    for (k = 0, zeros = 1; k < bs; k++) if (rb[bs + k]) { zeros = 0; break; }
    if (zeros)
        res(R_FAIL, grp, "DIRMIS (H6) reproduced: WRITE(10) of 2 blocks with scsi_Length %lu (io_Error %ld, status %d, actual %lu): the 2nd block became ZEROS",
            (unsigned long)bs, (long)e, ST(c), (unsigned long)ACT(c));
    else
        res(R_PASS, grp, "DIRMIS (H6): over-long WRITE(10): io_Error %ld, block 2 holds $%02x", (long)e, rb[bs]);
restore:
    e = wr(c, P_TD64, blk, 2, save);
    if (!e) e = rd(c, P_TD64, blk, 2, rb);
    CK(e == 0 && !memcmp(rb, save, 2 * bs), "DIRMIS: blocks %lu-%lu written back as they were", (unsigned long)blk, (unsigned long)blk + 1);
out:
    gfree(save); gfree(aa); gfree(rb); gfree(b55);
}

/* SLOWCMD (H1): START STOP UNIT start with IMMED=0; then the other id must still answer */
static void t_slowcmd(void)
{
    struct Ctx *c = M;
    UBYTE stop[6] = { 0x1b, 0, 0, 0, 0, 0 }, start[6] = { 0x1b, 0, 0, 0, 1, 0 };
    ULONG t0, dt;
    LONG e;
    grp = "audit";
    if (g_df && g_unit >= 100) { res(R_SKIP, grp, "SLOWCMD: a SCSI unit, not IDE"); return; }
    e = scsi(c, stop, 6, NULL, 0, SCSIF_READ | SCSIF_AUTOSENSE, 18);
    if (HUNG()) return;
    say("    SLOWCMD: START STOP UNIT stop: io_Error %ld, status %d", (long)e, ST(c));
    t0 = now_ms();
    e = scsi(c, start, 6, NULL, 0, SCSIF_READ | SCSIF_AUTOSENSE, 18);
    dt = now_ms() - t0;
    if (HUNG()) return;
    if (e == 0 && dt < 9000)
        res(R_SKIP, grp, "SLOWCMD (H1) not reproduced: the start (IMMED=0) took %lu ms, under the driver's 10 s - needs a slow mechanical disk", (unsigned long)dt);
    else
        res(e ? R_FAIL : R_PASS, grp, "SLOWCMD (H1): the start took %lu ms: io_Error %ld %s", (unsigned long)dt, (long)e, tdname(e));
    e = unit_ready(c);
    CK(e == 0, "SLOWCMD: TEST UNIT READY on the unit afterwards: status %ld", (long)e);
    if (g_unit2 >= 0) {
        struct Ctx *x = ctx_new("unit2");
        if (x && open_bounded(x, g_unit2, 0, NULL) == 0) {
            int st = unit_ready(x);
            res(st == 0 ? R_PASS : R_FAIL, grp, "SLOWCMD: TEST UNIT READY on UNIT2 %ld afterwards: status %d", (long)g_unit2, st);
        }
        if (x && !x->stuck) ctx_free(x);
    } else
        res(R_SKIP, grp, "SLOWCMD: the other-id check needs UNIT2");
}

/* CDLATE (H4): a disc that comes after the open */
static void t_cdlate(void)
{
    struct Ctx *c = M;
    struct DriveGeometry dg;
    UBYTE *b;
    ULONG end, bs0;
    LONG e = -1;
    grp = "audit";
    if (!U.removable || U.blocks) {
        res(R_SKIP, grp, "CDLATE (H4) not testable here: needs a removable unit opened with no disc in (this one: %s) - manual test, see docs/flysuite.md",
            !U.removable ? "fixed" : "a disc is in");
        return;
    }
    if (!(b = gbuf(2048, MEMF_ANY))) return;
    memset(&dg, 0, sizeof(dg));
    bio(c, TD_GETGEOMETRY, &dg, sizeof(dg), 0, 0);
    bs0 = dg.dg_SectorSize;
    say("    CDLATE: no disc; the driver says %lu-byte sectors. Insert a disc now - waiting up to %lu s", (unsigned long)bs0, (unsigned long)g_secs);
    end = now_ms() + g_secs * 1000;
    while (now_ms() < end && !brk() && !g_stuck) {
        e = bio(c, CMD_READ, b, 2048, 16 * 2048, 0);
        if (e == 0 || e == E_HANG) break;
        tick(2000);
    }
    if (e == 0) {
        memset(&dg, 0, sizeof(dg));
        bio(c, TD_GETGEOMETRY, &dg, sizeof(dg), 0, 0);
        res(memcmp(b + 1, "CD001", 5) ? R_FAIL : R_PASS, grp, "CDLATE (H4): CMD_READ 2048 at 16*2048: %s; sectors %lu before, %lu after",
            memcmp(b + 1, "CD001", 5) ? "GOOD status but no CD001 (wrong sector: reproduced)" : "CD001 found", (unsigned long)bs0,
            (unsigned long)dg.dg_SectorSize);
    } else if (e != E_HANG)
        res(R_SKIP, grp, "CDLATE: no disc came within %lu s (io_Error %ld)", (unsigned long)g_secs, (long)e);
    gfree(b);
}

/* ATAPIDIR (H2): INQUIRY / MODE SENSE to an IDE ATAPI unit with the direction flag wrong */
static void t_atapidir(void)
{
    struct Ctx *c = M;
    UBYTE cdb[16], *b = gbuf(256, MEMF_ANY);
    ULONG tmo = g_tmo;
    LONG e, oldpri;
    grp = "audit";
    if (!g_danger) { res(R_SKIP, grp, "ATAPIDIR: may wedge the machine - needs DANGER (run it last, in its own boot)"); goto out0; }
    /* on dataflyer.device the IDE ATAPI path (its SCSI side is DIRMIS's); on another driver any CD-ROM unit */
    if ((g_df && g_unit < 100) || U.devtype != 5) {
        res(R_SKIP, grp, "ATAPIDIR: needs %s; this is unit %ld type %d", g_df ? "an IDE ATAPI CD unit (100/101)" : "a CD-ROM unit (type 5)",
            (long)g_unit, U.devtype);
        goto out0;
    }
    if (!b) goto out0;
    oldpri = SetTaskPri(g_main, 5);            /* above the driver task (1): our timer wait still runs if it spins */
    g_tmo = 15000;
    say("    ATAPIDIR: sending INQUIRY 36 with scsi_Flags = 0 (SCSIF_WRITE) to unit %ld; 15 s limit", (long)g_unit);
    memset(cdb, 0, 16); cdb[0] = 0x12; cdb[4] = 36;
    e = scsi(c, cdb, 6, b, 36, 0, 0);
    if (HUNG()) res(R_FAIL, grp, "ATAPIDIR (H2) reproduced: INQUIRY with the direction wrong never came back (see the HANG above)");
    else {
        res(R_PASS, grp, "ATAPIDIR (H2) not reproduced: INQUIRY with SCSIF_WRITE came back in %lu ms: io_Error %ld, status %d, actual %lu",
            (unsigned long)c->lastms, (long)e, ST(c), (unsigned long)ACT(c));
        say("    ATAPIDIR: MODE SENSE(6) page $3F with scsi_Flags = 0");
        memset(cdb, 0, 16); cdb[0] = 0x1a; cdb[2] = 0x3f; cdb[4] = 255;
        e = scsi(c, cdb, 6, b, 255, 0, 0);
        if (HUNG()) res(R_FAIL, grp, "ATAPIDIR (H2) reproduced: MODE SENSE with the direction wrong never came back");
        else res(R_PASS, grp, "ATAPIDIR (H2): MODE SENSE with SCSIF_WRITE came back in %lu ms: io_Error %ld, actual %lu",
                 (unsigned long)c->lastms, (long)e, (unsigned long)ACT(c));
    }
    g_tmo = tmo;
    SetTaskPri(g_main, oldpri);
out0:
    gfree(b);
}

/* RUNEND (H3): a READ whose data is not a multiple of 256 into a longer buffer */
static void t_runend(void)
{
    struct Ctx *c = M;
    UBYTE cdb[16], *b;
    ULONG bs = U.bs, len;
    LONG e;
    grp = "audit";
    if (!g_danger || !U.emu) { res(R_SKIP, grp, "RUNEND: may freeze the machine - needs DANGER and an emulator's unit (run it last, in its own boot)"); return; }
    if (!U.blocks || !(b = gbuf(8192, MEMF_ANY))) return;
    mk_read(cdb, 10, 0, 1);
    len = (bs + 511) & ~511UL;
    if (len == bs) len += 512;
    e = scsi(c, cdb, 10, b, len, SCSIF_READ | SCSIF_AUTOSENSE, 18);
    CK(e == 0 && ACT(c) == bs, "RUNEND (H3) safe case: READ(10) 1 block of %lu into %lu bytes: actual %lu", (unsigned long)bs,
       (unsigned long)len, (unsigned long)ACT(c));
    if (bs % 256 == 0) {
        res(R_SKIP, grp, "RUNEND (H3) not testable here: the block size %lu is a multiple of 256 (needs 520/2352-byte blocks)", (unsigned long)bs);
    } else {
        say("    RUNEND: READ(10) 3 blocks of %lu into %lu bytes - a freeze right here is H3", (unsigned long)bs, (unsigned long)((3 * bs + 511) & ~511UL));
        mk_read(cdb, 10, 0, 3);
        len = (3 * bs + 511) & ~511UL;
        e = scsi(c, cdb, 10, b, len, SCSIF_READ | SCSIF_AUTOSENSE, 18);
        CK(e == 0 && ACT(c) == 3 * bs, "RUNEND (H3): READ(10) 3 blocks of %lu into %lu bytes: actual %lu, no freeze", (unsigned long)bs,
           (unsigned long)len, (unsigned long)ACT(c));
    }
    gfree(b);
}

/* RACEOPEN (M3): four priority-10 processes do the very first OpenDevice at once */
static volatile UBYTE race_ready[4], race_opened[4];
static struct Task *race_task[4];

static void j_race(struct Job *j)
{
    struct Ctx *c = ctx_new("race");
    UBYTE *b = gbuf(4096, MEMF_ANY);
    ULONG t0;
    SetSignal(0, SIGBREAKF_CTRL_F);
    race_ready[j->idx] = 1;
    beat(j, "race %d: waiting for the start", j->idx);
    Wait(SIGBREAKF_CTRL_F);
    if (!c || !b) {                            /* still wait for both signals: the parent sends them */
        j->err = -98;
        race_opened[j->idx] = 1;
        Wait(SIGBREAKF_CTRL_F);
        if (b) gfree(b);
        if (c) ctx_free(c);
        return;
    }
    beat(j, "race %d: OpenDevice %s unit %ld", j->idx, g_dev, (long)j->unit);
    t0 = now_ms();
    j->err = OpenDevice((STRPTR)g_dev, j->unit, (struct IORequest *)c->io, 0) ? c->io->io_Error : 0;
    j->ms = now_ms() - t0;
    if (!j->err) {
        c->opened = 1;
        j->n1 = (ULONG)c->io->io_Unit;
        beat(j, "race %d: CMD_READ block 0", j->idx);
        j->n2 = (ULONG)bio(c, CMD_READ, b, 512, 0, 0);
        j->n3 = c->io->io_Unit->unit_OpenCnt;
    }
    race_opened[j->idx] = 1;
    beat(j, "race %d: waiting to close", j->idx);
    Wait(SIGBREAKF_CTRL_F);
    if (!c->stuck) gfree(b);
    ctx_free(c);
}

static void t_raceopen(void)
{
    struct Job *js[4];
    struct Node *n;
    int i, k, ok = 1, same = 1;
    ULONG t0;
    grp = "audit";
    Forbid();
    n = FindName(&SysBase->DeviceList, (STRPTR)g_devbase);
    Permit();
    if (n) { res(R_SKIP, grp, "RACEOPEN (M3): %s is already loaded in this boot - needs a fresh boot (TESTS=raceopen as the first command)", g_dev); return; }
    g_jobpri = 10;
    for (i = 0; i < 4; i++) {
        race_ready[i] = race_opened[i] = 0;
        js[i] = job_new();
        if (!js[i]) { g_jobpri = 0; return; }
        js[i]->idx = i; js[i]->unit = g_unit;
        if (!job_start(js[i], j_race)) { g_jobpri = 0; res(R_FAIL, grp, "RACEOPEN: can't start the processes"); return; }
        race_task[i] = g_lastjob;
    }
    g_jobpri = 0;
    t0 = now_ms();
    for (;;) {                                  /* all four waiting? */
        for (k = 0, i = 0; i < 4; i++) k += race_ready[i];
        if (k == 4 || now_ms() - t0 > 5000) break;
        tick(50);
    }
    say("    RACEOPEN: 4 processes at priority 10, the first OpenDevice of %s unit %ld at once", g_dev, (long)g_unit);
    Forbid();
    for (i = 0; i < 4; i++) Signal(race_task[i], SIGBREAKF_CTRL_F);
    Permit();
    t0 = now_ms();
    for (;;) {
        for (k = 0, i = 0; i < 4; i++) k += race_opened[i];
        if (k == 4) break;
        if (now_ms() - t0 > g_tmo) {
            g_stuck = 1;
            for (i = 0; i < 4; i++) if (!race_opened[i]) js[i]->hung = 1;
            res(R_HANG, grp, "RACEOPEN (M3): %d of 4 openers did not get through OpenDevice + a read in %lu s", 4 - k, (unsigned long)(g_tmo / 1000));
            for (i = 0; i < 4; i++) if (race_opened[i]) Signal(race_task[i], SIGBREAKF_CTRL_F);
            return;
        }
        drain();
        tick(100);
    }
    for (i = 0; i < 4; i++) {
        say("    opener %d: OpenDevice %ld in %lu ms, unit $%08lx, CMD_READ %ld, unit_OpenCnt %lu", i, (long)js[i]->err,
            (unsigned long)js[i]->ms, (unsigned long)js[i]->n1, (long)js[i]->n2, (unsigned long)js[i]->n3);
        if (js[i]->err || js[i]->n2) ok = 0;
        if (js[i]->n1 != js[0]->n1) same = 0;
    }
    if (ok && same)
        res(R_PASS, grp, "RACEOPEN (M3) not reproduced: 4 simultaneous first opens, one unit, all reads ok");
    else
        res(R_FAIL, grp, "RACEOPEN (M3) reproduced: %s%s", ok ? "" : "an open or a read failed; ", same ? "" : "the openers got DIFFERENT unit structures (an orphaned unit)");
    for (i = 0; i < 4; i++) Signal(race_task[i], SIGBREAKF_CTRL_F);
    wait_jobs(js, 4, g_tmo);
}

/* ---- RAWWRITE safety: no RDB, no partition, no mounted volume in range -- */

static int overlap(ULONG a0, ULONG n, unsigned long long b0, unsigned long long b1)
{
    return (unsigned long long)a0 <= b1 && (unsigned long long)a0 + n - 1 >= b0;
}

static int bstr_eq(BSTR bs, const char *s)
{
    const UBYTE *p = BADDR(bs);
    int i, n = strlen(s);
    /* mount tools often count a trailing NUL in the BSTR's length */
    if (!p || !(p[0] == n
 || (p[0] == n + 1 && p[n + 1] == 0))) return 0;

    for (i = 0; i < n; i++) {
        char a = p[1 + i], b = s[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static unsigned long long env_lo(ULONG *env, ULONG bs)
{
    return (unsigned long long)env[DE_LOWCYL] * env[DE_NUMHEADS] * env[DE_BLKSPERTRACK] * (env[DE_SIZEBLOCK] * 4) / bs;
}
static unsigned long long env_hi(ULONG *env, ULONG bs)
{
    return (unsigned long long)(env[DE_UPPERCYL] + 1) * env[DE_NUMHEADS] * env[DE_BLKSPERTRACK] * (env[DE_SIZEBLOCK] * 4) / bs - 1;
}

/* 0 = the range is free of everything we know about; prints why not. Bit 0: the RDB or an RDB
 * partition (IKNOW overrides it); bit 1: outside the unit or on a MOUNTED volume (never) */
static int rawwrite_check_u(struct Ctx *cc, LONG unit, ULONG blocks, ULONG bsz, ULONG first, ULONG count)
{
    UBYTE *blk = gbuf(bsz > 512 ? bsz : 512, MEMF_ANY);
    ULONG k, rdb = 0xffffffffUL, next;
    int bad = 0, n;
    struct DosList *dl;
    if (!blk) return 2;
    if (first >= blocks || count > blocks - first) {
        con("flysuite: RAWWRITE=%lu,%lu is outside the unit (%lu blocks)\n", (unsigned long)first, (unsigned long)count,
               (unsigned long)blocks);
        bad = 2;               /* never overridable */
        goto out;
    }
    for (k = 0; k < 16 && k < blocks; k++)
        if (rd(cc, P_CMD, k, 1, blk) == 0 && be32(blk) == IDNAME_RIGIDDISK) { rdb = k; break; }
    if (rdb != 0xffffffffUL) {
        struct RigidDiskBlock *r = (struct RigidDiskBlock *)blk;
        ULONG hi = r->rdb_RDBBlocksHi > rdb ? r->rdb_RDBBlocksHi : rdb + 1;
        if (overlap(first, count, 0, hi)) {
            con("flysuite: RAWWRITE overlaps the RDB (blocks 0-%lu)\n", (unsigned long)hi);
            bad |= 1;
        }
        next = r->rdb_PartitionList;
        for (n = 0; n < 64 && next != 0xffffffffUL && next < blocks; n++) {
            struct PartitionBlock *pb = (struct PartitionBlock *)blk;
            if (rd(cc, P_CMD, next, 1, blk) || pb->pb_ID != IDNAME_PARTITION) break;
            {
                int ov = overlap(first, count, env_lo(pb->pb_Environment, bsz), env_hi(pb->pb_Environment, bsz));
                con("  unit %ld RDB partition %.*s: blocks %lu-%lu%s\n", (long)unit, pb->pb_DriveName[0], (char *)pb->pb_DriveName + 1,
                       (unsigned long)env_lo(pb->pb_Environment, bsz), (unsigned long)env_hi(pb->pb_Environment, bsz),
                       ov ? "  <- the range overlaps it: refused" : "");
                if (ov) bad |= 1;
            }
            next = pb->pb_Next;
        }
    }
    dl = LockDosList(LDF_DEVICES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_DEVICES))) {
        struct FileSysStartupMsg *fssm = BADDR(dl->dol_misc.dol_handler.dol_Startup);
        ULONG *env;
        if ((LONG)dl->dol_misc.dol_handler.dol_Startup <= 64 || !TypeOfMem(fssm) || !fssm->fssm_Device ||
            (!((LONG)fssm->fssm_Unit == unit && bstr_eq(fssm->fssm_Device, g_devbase)) &&
             !(g_df && unit >= 100 && (LONG)fssm->fssm_Unit == unit - 100 && bstr_eq(fssm->fssm_Device, "scsi.device"))))
            continue;
        env = BADDR(fssm->fssm_Environ);
        if (!env || !TypeOfMem(env) || env[DE_TABLESIZE] < DE_UPPERCYL) continue;
        if (overlap(first, count, env_lo(env, bsz), env_hi(env, bsz))) {
            const UBYTE *nm = BADDR(dl->dol_Name);
            con("flysuite: RAWWRITE overlaps the mounted %.*s: (blocks %lu-%lu)\n", nm ? nm[0] : 0, nm ? (const char *)nm + 1 : "",
                   (unsigned long)env_lo(env, bsz), (unsigned long)env_hi(env, bsz));
            bad |= 2;               /* a live volume: never overridable */
        }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
out:
    gfree(blk);
    return bad;
}

static int rawwrite_check(void)
{
    return rawwrite_check_u(M, g_unit, U.blocks, U.bs, g_rwfirst, g_rwcount);
}

/* "first,count" */
static int parse_range(const char *p, ULONG *first, ULONG *count)
{
    char *q;
    *first = strtoul(p, &q, 0);
    *count = (*q == ',') ? strtoul(q + 1, &q, 0) : 0;
    return q != p && !*q && *count && *count <= 65536;
}

/* ---- main ---------------------------------------------------------------- */


#define G_CONF 1
#define G_BIG 2
#define G_EDGE 4
#define G_CONC 8
#define G_SOAK 16
#define G_TOOLBOX 32
#define G_FUZZ 64
#define G_ERRMAP 128
#define G_CDBPASS 256
#define G_DIRMIS 512
#define G_SLOW 1024
#define G_CDLATE 2048
#define G_RACE 4096
#define G_ATAPIDIR 8192
#define G_RUNEND 16384
#define G_MIX 32768
#define G_LATENCY 65536
#define G_DMA16 131072
#define G_ALL (0x7f | G_ERRMAP | G_CDBPASS | G_LATENCY | G_DMA16)
#define G_AUDIT (G_ERRMAP | G_CDBPASS | G_DIRMIS | G_CDLATE | G_RACE)

static int parse_tests(const char *s)
{
    static const char *const nm[18] = { "conf", "big", "edge", "conc", "soak", "toolbox", "fuzz", "errmap", "cdbpass",
                                        "dirmis", "slowcmd", "cdlate", "raceopen", "atapidir", "runend", "mix",
                                        "latency", "dma16" };
    int m = 0;
    while (*s) {
        char w[16];
        int n = 0, i;
        while (*s == ',' || *s == ' ') s++;
        while (*s && *s != ',' && n < 15) w[n++] = *s++;
        w[n] = 0;
        if (!n) break;
        if (!strcmp(w, "all")) { m |= G_ALL; continue; }
        if (!strcmp(w, "audit")) { m |= G_AUDIT; continue; }
        for (i = 0; i < 18; i++) if (!strcmp(w, nm[i])) break;
        if (i == 18) return -1;
        m |= 1 << i;
    }
    return m;
}

/* dataflyer.device's own counters (DF_STATS, read and clear) */
static void df_stats(int show)
{
    ULONG st[24];
    memset(st, 0, sizeof(st));
    if (!g_df || g_stuck) return;
    if (bio(M, 0x7ff0, st, sizeof(st), 0, 0) == 0 && M->io->io_Actual >= 23 * 4 && show)
        say("    driver counters: %lu reads (%lu KB), %lu writes (%lu KB), %lu SCSI commands, %lu bus timeouts, %lu timer sleeps",
            (unsigned long)st[0], (unsigned long)(st[2] >> 10), (unsigned long)st[1], (unsigned long)(st[3] >> 10),
            (unsigned long)st[4], (unsigned long)st[21], (unsigned long)st[22]);
}

int main(void)
{
    LONG args[23];
    struct RDArgs *rda;
    int tests = G_ALL & ~G_FUZZ, rc = 0, i;
    LONG e;
    ULONG ms = 0;
    (void)version;

    memset(args, 0, sizeof(args));
    rda = ReadArgs("DEVICE/K,UNIT/A/K/N,UNIT2/K/N,TESTS/K,MINUTES/K/N,SECONDS/K/N,WRITEFILE/K,RAWWRITE/K,FUZZ/S,FUZZN/K/N,"
                   "IKNOW/S,TOOLBOX/S,WORKERS/K/N,TIMEOUT/K/N,SEED/K/N,LOG/K,QUIET/S,DANGER/S,SKIPIDS/K,RAWWRITE2/K,RAWIDE/K,IDE/K/N,FILES/K", args, NULL);
    if (!rda) {
        con("flysuite UNIT=<n> [DEVICE=<name>, default dataflyer.device] [UNIT2=<n>] [TESTS=all|audit|conf,big,edge,conc,soak,toolbox,fuzz,\n"
               "         errmap,cdbpass,dirmis,slowcmd,cdlate,raceopen,atapidir,runend,latency,dma16] [MINUTES=<n>] [SECONDS=<n>]\n"
               "         [WRITEFILE=<vol:dir>] [RAWWRITE=<first>,<count>] [FUZZ] [FUZZN=<n>] [IKNOW] [DANGER]\n"
               "         [TOOLBOX] [WORKERS=<n>] [TIMEOUT=<s>] [SEED=<n>] [LOG=<file>] [QUIET] [SKIPIDS=<id,id>]\n"
               "         mix: [RAWWRITE2=<first>,<count>] [FILES=<dir>,<dir>,...]\n"
               "         dataflyer.device only: [IDE=<unit>] [RAWIDE=<first>,<count>]\n");
        return 20;
    }
    g_danger = args[17] != 0;
    if ((args[19] && !parse_range((const char *)args[19], &g_rw2first, &g_rw2count)) ||
        (args[20] && !parse_range((const char *)args[20], &g_rwifirst, &g_rwicount))) {
        con("flysuite: RAWWRITE2 / RAWIDE = <first block>,<count>\n");
        FreeArgs(rda);
        return 20;
    }
    if (args[21]) g_ide = *(LONG *)args[21];
    g_files = (const char *)args[22];
    if (args[18]) {
        const char *p = (const char *)args[18];
        while (*p) { if (*p >= '0' && *p <= '6') g_skipids |= 1UL << (*p - '0'); p++; }
    }
    if (args[0]) g_dev = (const char *)args[0];
    {   /* the name without a path (mount entries name it so); is it ours? */
        const char *p;
        for (p = g_devbase = g_dev; *p; p++) if (*p == ':' || *p == '/') g_devbase = p + 1;
        g_df = streqi(g_devbase, "dataflyer.device");
    }
    if (!g_df) {
        if (args[21] || args[20])
            con("flysuite: IDE= and RAWIDE= are dataflyer.device's (its IDE port): ignored for %s\n", g_devbase);
        g_ide = -1;
        g_rwicount = 0;
    }
    g_unit = *(LONG *)args[1];
    if (args[2]) g_unit2 = *(LONG *)args[2];
    /* (after UNIT/UNIT2 are read: this check used to look at the defaults) */
    if (((g_unit < 100 || !g_df) && (g_skipids & (1UL << (g_unit % 10)))) ||
        (g_unit2 >= 0 && (g_unit2 < 100 || !g_df) && (g_skipids & (1UL << (g_unit2 % 10))))) {
        con("flysuite: UNIT/UNIT2 is on an id in SKIPIDS\n");
        FreeArgs(rda);
        return 20;
    }
    if (args[3] && (tests = parse_tests((const char *)args[3])) < 0) { con("flysuite: TESTS: conf,big,edge,conc,soak,toolbox,fuzz,latency,dma16, an audit test or all\n"); FreeArgs(rda); return 20; }
    if (args[4]) g_minutes = *(LONG *)args[4];
    if (args[5]) g_secs = *(LONG *)args[5];
    g_wfile = (const char *)args[6];
    if (args[7]) {
        const char *p = (const char *)args[7];
        char *q;
        g_rwfirst = strtoul(p, &q, 0);
        g_rwcount = (*q == ',') ? strtoul(q + 1, &q, 0) : 0;
        if (q == p || *q || !g_rwcount || g_rwcount > 65536) {
            con("flysuite: RAWWRITE=<first block>,<count> (at most 1 MB of blocks)\n");
            FreeArgs(rda);
            return 20;
        }
    }
    g_fuzz = args[8] != 0;
    if (args[9]) g_fuzzn = *(LONG *)args[9];
    g_iknow = args[10] != 0;
    g_tbforce = args[11] != 0;
    if (args[12]) g_workers = *(LONG *)args[12];
    if (g_workers > 9) g_workers = 9;
    if (args[13]) g_tmo = *(LONG *)args[13] * 1000UL;
    if (g_tmo < 2000) g_tmo = 2000;
    g_quiet = args[16] != 0;
    if (g_fuzz && !args[3]) tests |= G_FUZZ;
    if (!g_fuzz) tests &= ~G_FUZZ | (args[3] ? G_FUZZ : 0);

    g_main = FindTask(NULL);
    g_jobsig = AllocSignal(-1);
    M = ctx_new("main");
    if (!M || g_jobsig < 0) { con("flysuite: no memory / no timer.device\n"); FreeArgs(rda); return 20; }
    if (args[14]) g_seed = *(LONG *)args[14];
    else { struct timeval tv; GetSysTime(&tv); g_seed = (tv.tv_micro * 2654435761UL) ^ tv.tv_secs; g_seed %= 1000000; }
    if (args[15]) {
        if (!(g_log = Open((STRPTR)args[15], MODE_NEWFILE))) con("flysuite: can't open LOG %s\n", (char *)args[15]);
        else { Close(g_log); g_logname = (const char *)args[15]; }   /* g_log stays set as "there is a log" */
    }

    if (tests & G_RACE) {                      /* before our own first open */
        t_raceopen();
        drain();
        if (g_stuck) goto done;
    }
    e = open_bounded(M, g_unit, 0, &ms);
    if (e) {
        if (e != E_HANG) res(R_FAIL, "open", "%s unit %ld: OpenDevice error %ld (in %lu ms)", g_dev, (long)g_unit, (long)e, (unsigned long)ms);
        rc = 10;
        goto done;
    }
    df_stats(0);
    {   /* the unit */
        UBYTE *b = gbuf(512, MEMF_ANY);
        ULONG bs = 512;
        U.blocks = ctx_geom(M, &bs);
        U.bs = bs; ctx_bs(M, bs); U.sh = M->sh;
        if (b && inquiry(M, b, 36, 36) == 0 && ST(M) == 0) {
            memcpy(U.inq, b, 36);
            memcpy(U.vendor, b + 8, 8); memcpy(U.product, b + 16, 16); memcpy(U.rev, b + 32, 4);
            U.devtype = b[0] & 0x1f;
            U.removable = (b[1] & 0x80) != 0;
        }
        if (b && U.blocks && U.bs == 512 && rd(M, P_CMD, 0, 1, b) == 0 && pattern_ok(b, 0, 1) &&
            rd(M, P_TD64, U.blocks - 1, 1, b) == 0 && pattern_ok(b, U.blocks - 1, 1))
            U.pattern = 1;
        U.emu = has(U.vendor, "UAE") || has(U.product, "UAE") || has(U.vendor, "QEMU");
        if (b && (g_tbforce || has(U.vendor, "BLUESCSI") || has(U.product, "BLUESCSI") || has(U.vendor, "ZULU") || has(U.product, "ZULU"))) {
            int n;
            if (tb_count(M, b, &n) == 0) { U.toolbox = 1; U.tbcount = n; }
            unit_ready(M);
        }
        gfree(b);
    }
    con("flysuite 1.1.3: %s unit %ld: \"%s\" \"%s\" \"%s\", type %d, %lu blocks of %lu bytes (%lu MB)%s%s%s\n", g_dev, (long)g_unit,
           U.vendor, U.product, U.rev, U.devtype, (unsigned long)U.blocks, (unsigned long)U.bs,
           (unsigned long)(((unsigned long long)U.blocks * U.bs) >> 20), U.pattern ? ", pattern disk" : "",
           U.emu ? ", emulator" : "", U.toolbox ? ", Toolbox" : "");
    con("  tests:%s%s%s%s%s%s%s%s%s%s%s  seed %lu  timeout %lu s  opened in %lu ms\n", tests & G_CONF ? " conf" : "", tests & G_BIG ? " big" : "",
           tests & G_EDGE ? " edge" : "", tests & G_TOOLBOX ? " toolbox" : "", tests & G_CONC ? " conc" : "",
           tests & G_SOAK ? " soak" : "", tests & G_FUZZ ? " fuzz" : "", tests & G_MIX ? " mix" : "",
           tests & G_LATENCY ? " latency" : "", tests & G_DMA16 ? " dma16" : "",
           tests & ~(0x7f | G_MIX | G_LATENCY | G_DMA16) ? " audit" : "", (unsigned long)g_seed,
           (unsigned long)(g_tmo / 1000), (unsigned long)ms);
    if (g_log) {
        char b[200];
        sprintf(b, "flysuite 1.1.3: %s unit %ld \"%s\" \"%s\", %lu x %lu, seed %lu\n", g_dev, (long)g_unit, U.vendor, U.product,
                (unsigned long)U.blocks, (unsigned long)U.bs, (unsigned long)g_seed);
        out_line(0, b);
    }
    {
        char b[200];
        if (g_df)
            sprintf(b, "  mode: dataflyer.device - units 100/101 = the IDE port through the driver (IDE unit %ld), its counters on\n",
                    (long)g_ide);
        else
            sprintf(b, "  mode: another driver (%s) - every unit is a plain SCSI/disk unit; no IDE unit, no driver counters\n", g_devbase);
        out_line(1, b);
    }

    /* refusals, before anything runs */
    if (g_rwcount * U.bs > 1048576UL) {
        con("flysuite: RAWWRITE refused: %lu blocks of %lu bytes is more than 1 MB\n", (unsigned long)g_rwcount, (unsigned long)U.bs);
        rc = 20;
        goto done;
    }
    if (g_unit2 >= 0) {                        /* UNIT2 must be storage too */
        struct Ctx *x = ctx_new("unit2");
        UBYTE ib[40];
        int bad = 0;
        if (x && open_bounded(x, g_unit2, 0, NULL) == 0) {
            memset(ib, 0, sizeof(ib));
            if (inquiry(x, ib, 36, 36) == 0 && ((ib[0] & 0x1f) == 3 || (ib[0] & 0x1f) == 9 || has((char *)ib + 8, "DAYNA")))
                bad = 1;
        } else
            bad = 2;
        if (x && !x->stuck) ctx_free(x);
        if (bad) {
            con("flysuite: UNIT2 %ld refused: %s\n", (long)g_unit2, bad == 1 ? "a network/processor device" : "it does not open");
            rc = 20;
            goto done;
        }
    }
    if (U.devtype == 3 || U.devtype == 9 || has(U.vendor, "DAYNA") || has(U.product, "DAYNA")) {

        con("flysuite: refused: unit %ld is a network/processor device (type %d \"%s\"), not storage - never tested\n",
               (long)g_unit, U.devtype, U.product);
        rc = 20;
        goto done;
    }
    if ((tests & G_FUZZ) && g_fuzz && !U.emu && !g_iknow) {
        con("flysuite: FUZZ refused: \"%s\" \"%s\" is not an emulator's unit (add IKNOW to fuzz a real target)\n", U.vendor, U.product);
        rc = 20;
        goto done;
    }
    if (g_rwcount && (i = rawwrite_check()) != 0 && ((i & 2) || !g_iknow)) {
        con("flysuite: RAWWRITE refused (IKNOW overrides only the RDB/partition check - never a mounted volume or the unit's end)\n");
        rc = 20;
        goto done;
    }

    if ((tests & G_CONF) && !g_stuck && !g_break) t_conf();
    if ((tests & G_BIG) && !g_stuck && !g_break) t_big();
    if ((tests & G_EDGE) && !g_stuck && !g_break) t_edge();
    if ((tests & G_TOOLBOX) && !g_stuck && !g_break) t_toolbox();
    if ((tests & G_DMA16) && !g_stuck && !g_break) t_dma16();
    if ((tests & G_LATENCY) && !g_stuck && !g_break) t_latency();
    if ((tests & G_CONC) && !g_stuck && !g_break) t_conc();
    if ((tests & G_SOAK) && !g_stuck && !g_break) t_soak();
    if ((tests & G_ERRMAP) && !g_stuck && !g_break) t_errmap();
    if ((tests & G_CDBPASS) && !g_stuck && !g_break) t_cdbpass();
    if ((tests & G_DIRMIS) && !g_stuck && !g_break) t_dirmis();
    if ((tests & G_CDLATE) && !g_stuck && !g_break) t_cdlate();
    if ((tests & G_SLOW) && !g_stuck && !g_break) t_slowcmd();
    if ((tests & G_MIX) && !g_stuck && !g_break) t_mix();

    if ((tests & G_FUZZ) && !g_stuck && !g_break) t_fuzz();
    drain();
    df_stats(1);
    /* the ones that may wedge or freeze the machine: last */
    if ((tests & G_RUNEND) && !g_stuck && !g_break) t_runend();
    if ((tests & G_ATAPIDIR) && !g_stuck && !g_break) t_atapidir();
    drain();

done:
    drain();
    if (g_break) out_line(1, "flysuite: *** break\n");
    {
        char b[200];
        sprintf(b, "flysuite: %ld PASS, %ld FAIL, %ld SKIP, %ld HANG, %ld outliers over 2 s, longest request %lu ms -> %s\n",
                (long)n_res[0], (long)n_res[1], (long)n_res[2], (long)n_res[3], (long)n_outl, (unsigned long)g_maxms,
                n_res[3] || g_stuck ? "HANG" : n_res[1] ? "FAIL" : rc ? "NOT RUN" : "PASS");
        out_line(1, b);
    }
    if (!rc) rc = n_res[3] || g_stuck ? 10 : n_res[1] ? 5 : 0;

    /* children still running: their code is ours, so we can't leave before they do */
    g_break = 1;
    for (i = 0; i < NJOBS; i++)
        if (jobs[i].used && !jobs[i].done) {
            out_line(1, "flysuite: waiting for a child process still inside the driver (CTRL-C does not end this):\n  ");
            out_line(1, jobs[i].what);
            out_line(1, "\n");
            while (!jobs[i].done) { drain(); tick(1000); }
        }
    drain();
    if (!g_stuck) {
        ctx_free(M);
        FreeSignal(g_jobsig);
    }
    FreeArgs(rda);
    return rc;
}
