/*
 * dataflyer.device - the A1200's built-in IDE port, and taking it over from
 * scsi.device. Part of DataFlyerSCSI. GPL-3.0-only, see LICENSE.
 *
 * Once the card has switched its port (to SCSI or back) the built-in IDE
 * interrupt no longer reaches the system, and scsi.device, which waits for
 * that interrupt, would hang. So before the first switch this driver takes
 * the port over:
 *   1. it opens scsi.device units 0 and 1 (master and slave) itself, to know
 *      their unit structures - every DOS device on the port uses those;
 *   2. under Forbid() it points scsi.device's Open, Close, BeginIO and AbortIO
 *      at its own code;
 *   3. it sends one read to each unit through scsi.device's old BeginIO and
 *      waits for it: everything scsi.device had queued before is done then;
 *   4. from then on it drives the port itself, by polling, with the drive
 *      interrupt off (nIEN).
 * scsi.device units 0 and 1 then run on this driver's IDE code (also as
 * dataflyer.device units 100 and 101), scsi.device units 2-6 (+ 10 * LUN) on
 * its SCSI ids 2-6, as the original SCSIPlus graft did. The bus semaphore
 * keeps IDE and SCSI commands apart.
 *
 * ATA disks get the SCSI commands a file system and HDToolBox use, translated
 * (READ/WRITE 6/10, INQUIRY, READ CAPACITY, MODE SENSE 3/4, ...). ATAPI
 * drives take SCSI command blocks as they are (PACKET).
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <proto/exec.h>
#include "dataflyer.h"

/* ---- the task file ------------------------------------------------------ */

#define IDE_BASE    0xDA2000UL
#define IDE_DATA    (*(volatile UWORD *)IDE_BASE)
#define IDE(n)      (*(volatile UBYTE *)(IDE_BASE + (n) * 4 + 2))
#define IDE_CTL     (*(volatile UBYTE *)0xDA301AUL)    /* write: device control, read: alternate status */

#define I_ERR   1
#define I_FEAT  1
#define I_COUNT 2
#define I_LBA0  3
#define I_LBA1  4
#define I_LBA2  5
#define I_DEV   6
#define I_STAT  7
#define I_CMD   7

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DF   0x20
#define ST_DRQ  0x08
#define ST_ERR  0x01
#define CTL_NIEN 0x02

#define T_BUSY  14000000             /* 20 s in CIA reads: a disk spinning up */
#define T_PROBE   700000             /* 1 s: is anything there */

struct DFUnit *ide_units[2];

static void pause400(void)           /* 400 ns after a write to the task file */
{
    (void)IDE_CTL; (void)IDE_CTL; (void)IDE_CTL; (void)IDE_CTL;
}

/* BSY clear: returns the status; 0xff = still busy after t */
static UBYTE wait_ready(ULONG t)
{
    ULONG n = 0, used = 0;
    int k;
    UBYTE s;
    for (k = 0; k < 256; k++)
        if (!((s = IDE(I_STAT)) & ST_BSY)) return s;
    while (used < t) {
        if (!((s = IDE(I_STAT)) & ST_BSY)) { stats.polls[W_IDE] += n; return s; }
        used += df_pause(n);                        /* spins ~1 ms, then sleeps 1 ms at a time */
        n++;
    }
    stats.polls[W_IDE] += n;
    stats.timeouts++;
    return 0xff;
}

static UBYTE select_drive(struct DFUnit *un, UBYTE dev)
{
    IDE_CTL = CTL_NIEN;
    IDE(I_DEV) = (UBYTE)(0xa0 | (un->drive << 4) | dev);
    pause400();
    return wait_ready(T_BUSY);
}

static void set_sense(struct DFUnit *un, UBYTE key, UBYTE asc, UBYTE ascq)
{
    un->skey = key; un->asc = asc; un->ascq = ascq;
}

/* an ATA command ended with ERR / DF / a timeout: sense for it */
static void ata_fail(struct DFUnit *un, UBYTE s)
{
    UBYTE e;
    if (s == 0xff) { set_sense(un, 0x04, 0x44, 0x00); return; }       /* HARDWARE ERROR: timeout */
    e = IDE(I_ERR);
    if (e & 0x40)      set_sense(un, 0x03, 0x11, 0x00);                /* uncorrectable read error */
    else if (e & 0x10) set_sense(un, 0x05, 0x21, 0x00);                /* LBA out of range */
    else if (e & 0x04) set_sense(un, 0x0b, 0x00, 0x00);                /* aborted command */
    else               set_sense(un, 0x04, 0x00, 0x00);
}

/* ---- ATA: sectors of 512 bytes ------------------------------------------ */

/* returns sectors moved; on a failure the sense is set */
static ULONG ata_xfer(struct DFUnit *un, ULONG lba, ULONG nblk, UBYTE *buf, int out)
{
    ULONG done = 0;
    while (nblk) {
        ULONG n = nblk > 256 ? 256 : nblk, i;
        int ext = un->lba48 && lba + n > 0x0fffffff;
        UBYTE s, cmd;
        if (ext) {
            s = select_drive(un, 0x40);
            if (s == 0xff) { ata_fail(un, s); break; }
            IDE(I_COUNT) = (UBYTE)(n >> 8);
            IDE(I_LBA0) = (UBYTE)(lba >> 24); IDE(I_LBA1) = 0; IDE(I_LBA2) = 0;
            IDE(I_COUNT) = (UBYTE)n;
            IDE(I_LBA0) = (UBYTE)lba; IDE(I_LBA1) = (UBYTE)(lba >> 8); IDE(I_LBA2) = (UBYTE)(lba >> 16);
            cmd = out ? 0x34 : 0x24;
        } else if (un->lba) {
            s = select_drive(un, (UBYTE)(0x40 | ((lba >> 24) & 0x0f)));
            if (s == 0xff) { ata_fail(un, s); break; }
            IDE(I_COUNT) = (UBYTE)n;                    /* 256 -> 0 */
            IDE(I_LBA0) = (UBYTE)lba; IDE(I_LBA1) = (UBYTE)(lba >> 8); IDE(I_LBA2) = (UBYTE)(lba >> 16);
            cmd = out ? 0x30 : 0x20;
        } else {                                        /* an old drive: cylinder/head/sector */
            ULONG hs = (ULONG)un->heads * un->secs, c = lba / hs, r = lba % hs;
            s = select_drive(un, (UBYTE)(r / un->secs));
            if (s == 0xff) { ata_fail(un, s); break; }
            IDE(I_COUNT) = (UBYTE)n;
            IDE(I_LBA0) = (UBYTE)(r % un->secs + 1);
            IDE(I_LBA1) = (UBYTE)c; IDE(I_LBA2) = (UBYTE)(c >> 8);
            cmd = out ? 0x30 : 0x20;
        }
        IDE(I_CMD) = cmd;
        pause400();
        for (i = 0; i < n; i++) {
            s = wait_ready(T_BUSY);
            if (s == 0xff || (s & (ST_ERR | ST_DF)) || !(s & ST_DRQ)) { ata_fail(un, s); return done; }
            if (out) df_ideout(buf); else df_idein(buf);
            buf += 512;
            done++;
        }
        if (out) {
            s = wait_ready(T_BUSY);
            if (s == 0xff || (s & (ST_ERR | ST_DF))) { ata_fail(un, s); return done - n; }
        }
        lba += n;
        nblk -= n;
    }
    return done;
}

static int ata_flush(struct DFUnit *un)
{
    UBYTE s = select_drive(un, 0);
    if (s == 0xff) return 0;
    IDE(I_CMD) = un->lba48 ? 0xea : 0xe7;              /* FLUSH CACHE (EXT) */
    pause400();
    s = wait_ready(T_BUSY);
    return s != 0xff && !(s & ST_ERR);                 /* an old drive without it: ERR, nothing to flush */
}

/* ---- ATA: the SCSI commands a file system / HDToolBox send -------------- */

static void put3(UBYTE *p, ULONG v) { p[0] = (UBYTE)(v >> 16); p[1] = (UBYTE)(v >> 8); p[2] = (UBYTE)v; }
static void put4(UBYTE *p, ULONG v) { p[0] = (UBYTE)(v >> 24); put3(p + 1, v); }

static int ata_scsi(struct DFUnit *un, const UBYTE *c, UBYTE *data, ULONG len, ULONG *actual)
{
    UBYTE tmp[64];                   /* MODE SENSE all pages: header 4 + descriptor 8 + 2 pages of 24 */
    ULONG lba, n, got;
    int i;

    *actual = 0;
    switch (c[0]) {
    case 0x00: case 0x1b: case 0x1e: case 0x2f:     /* TEST UNIT READY, START STOP, PREVENT, VERIFY */
        break;
    case 0x03:                                       /* REQUEST SENSE */
        memset(tmp, 0, 18);
        tmp[0] = 0x70; tmp[2] = un->skey; tmp[7] = 10; tmp[12] = un->asc; tmp[13] = un->ascq;
        n = len < 18 ? len : 18;
        memcpy(data, tmp, n);
        *actual = n;
        set_sense(un, 0, 0, 0);
        return 0;
    case 0x12:                                       /* INQUIRY */
        if (c[1] & 1) goto illegal;                  /* no vital product data pages */
        memset(tmp, ' ', 36);
        tmp[0] = 0; tmp[1] = un->removable ? 0x80 : 0; tmp[2] = 2; tmp[3] = 2; tmp[4] = 31;
        tmp[5] = tmp[6] = tmp[7] = 0;
        memcpy(tmp + 8, "ATA     ", 8);
        for (i = 0; i < 16 && un->model[i]; i++) tmp[16 + i] = un->model[i];
        for (i = 0; i < 4 && un->fw[i]; i++) tmp[32 + i] = un->fw[i];
        n = len < 36 ? len : 36;
        if (c[4] && n > c[4]) n = c[4];
        memcpy(data, tmp, n);
        *actual = n;
        break;
    case 0x25:                                       /* READ CAPACITY */
        put4(tmp, un->blocks - 1);
        put4(tmp + 4, 512);
        n = len < 8 ? len : 8;
        memcpy(data, tmp, n);
        *actual = n;
        break;
    case 0x1a: {                                     /* MODE SENSE (6): geometry pages 3 and 4 */
        int page = c[2] & 0x3f, o = 4;
        memset(tmp, 0, sizeof(tmp));
        if (page != 3 && page != 4 && page != 0x3f) goto illegal;
        if (!(c[1] & 0x08)) {                        /* a block descriptor */
            tmp[3] = 8;
            put3(tmp + o + 1, un->blocks > 0xffffff ? 0xffffff : un->blocks);
            put3(tmp + o + 5, 512);
            o += 8;
        }
        if (page == 3 || page == 0x3f) {
            tmp[o] = 0x03; tmp[o + 1] = 0x16;
            tmp[o + 10] = (UBYTE)(un->secs >> 8); tmp[o + 11] = (UBYTE)un->secs;
            tmp[o + 12] = 0x02; tmp[o + 13] = 0x00;  /* 512 bytes per sector */
            o += 0x18;
        }
        if (page == 4 || page == 0x3f) {
            if (o + 0x18 > (int)sizeof(tmp)) goto done_mode;
            tmp[o] = 0x04; tmp[o + 1] = 0x16;
            put3(tmp + o + 2, un->cyl);
            tmp[o + 5] = (UBYTE)un->heads;
            o += 0x18;
        }
    done_mode:
        tmp[0] = (UBYTE)(o - 1);
        n = len < (ULONG)o ? len : (ULONG)o;
        if (c[4] && n > c[4]) n = c[4];
        memcpy(data, tmp, n);
        *actual = n;
        break;
    }
    case 0x35:                                       /* SYNCHRONIZE CACHE */
        ata_flush(un);
        break;
    case 0x08: case 0x0a:                            /* READ / WRITE (6) */
        lba = ((ULONG)(c[1] & 0x1f) << 16) | ((ULONG)c[2] << 8) | c[3];
        n = c[4] ? c[4] : 256;
        goto rw;
    case 0x28: case 0x2a:                            /* READ / WRITE (10) */
        lba = ((ULONG)c[2] << 24) | ((ULONG)c[3] << 16) | ((ULONG)c[4] << 8) | c[5];
        n = ((ULONG)c[7] << 8) | c[8];
    rw:
        if (lba >= un->blocks || n > un->blocks - lba) { set_sense(un, 0x05, 0x21, 0x00); return 2; }
        if (n * 512 > len) n = len / 512;
        got = ata_xfer(un, lba, n, data, c[0] == 0x0a || c[0] == 0x2a);
        *actual = got * 512;
        if (got < n) return 2;
        break;
    default:
    illegal:
        set_sense(un, 0x05, 0x20, 0x00);             /* ILLEGAL REQUEST: invalid command */
        return 2;
    }
    return 0;
}

/* ---- ATAPI: the command block as a packet ------------------------------- */

static int atapi_cmd(struct DFUnit *un, const UBYTE *cdb, int cdblen, UBYTE *data, ULONG len, int out,
                     ULONG *actual)
{
    UBYTE pkt[12];
    ULONG done = 0;
    UBYTE s;
    int i;

    *actual = 0;
    memset(pkt, 0, sizeof(pkt));
    memcpy(pkt, cdb, cdblen > 12 ? 12 : cdblen);
    s = select_drive(un, 0);
    if (s == 0xff) return -1;
    IDE(I_FEAT) = 0;                                 /* PIO */
    IDE(I_LBA1) = 0xfe; IDE(I_LBA2) = 0xff;          /* up to 65534 bytes per DRQ */
    IDE(I_CMD) = 0xa0;                               /* PACKET */
    pause400();
    s = wait_ready(T_PROBE);
    if (s == 0xff || !(s & ST_DRQ)) return -1;
    for (i = 0; i < 12; i += 2)
        IDE_DATA = (UWORD)((pkt[i] << 8) | pkt[i + 1]);
    for (;;) {
        ULONG cnt, k, t;
        pause400();
        /* the end is the status phase (interrupt reason I/O + C/D); BSY can
         * drop a moment before DRQ comes up for the data */
        for (t = T_BUSY; t; t--) {
            s = wait_ready(T_BUSY);
            if (s == 0xff) return -1;
            if ((s & (ST_DRQ | ST_ERR)) || (IDE(I_COUNT) & 3) == 3) break;
            (void)CIA_PRA;
        }
        if (!t) { stats.timeouts++; return -1; }
        if (!(s & ST_DRQ)) break;
        cnt = IDE(I_LBA1) | ((ULONG)IDE(I_LBA2) << 8);
        /* whole sectors straight through; the rest (and what is past our buffer) a word at a time */
        while (cnt >= 512 && done + 512 <= len) {
            if (out) df_ideout(data + done); else df_idein(data + done);
            done += 512;
            cnt -= 512;
        }
        for (k = 0; k < cnt; k += 2) {
            UWORD w = 0;
            if (out) {
                if (done < len) w = (UWORD)(data[done] << 8);
                if (done + 1 < len) w |= data[done + 1];
                IDE_DATA = w;
            } else {
                w = IDE_DATA;
                if (done < len) data[done] = (UBYTE)(w >> 8);
                if (k + 1 < cnt && done + 1 < len) data[done + 1] = (UBYTE)w;
            }
            done += (k + 1 < cnt) ? 2 : 1;
        }
    }
    *actual = done < len ? done : len;
    return (s & ST_ERR) ? 2 : 0;                     /* CHECK CONDITION: REQUEST SENSE for the rest */
}

/* a SCSI command for a drive on the IDE port; the status (0, 2) or -1 */
int ide_cmd(struct DFUnit *un, const UBYTE *cdb, int cdblen, UBYTE *data, ULONG len, int out, ULONG *actual)
{
    int st;
    ObtainSemaphore(&buslock);
    stats.cmds++;
    st = un->atapi ? atapi_cmd(un, cdb, cdblen, data, len, out, actual) : ata_scsi(un, cdb, data, len, actual);
    ReleaseSemaphore(&buslock);
    return st;
}

/* ---- finding the drives ------------------------------------------------- */

/* an IDENTIFY string, from the words as ATA defines them (first character
 * in the high byte); through the port every word arrives byte-swapped */
static void idstr(char *d, const UWORD *w, int words)
{
    int i, n = words * 2;
    for (i = 0; i < words; i++) { d[2 * i] = (char)(w[i] >> 8); d[2 * i + 1] = (char)w[i]; }
    d[n] = 0;
    while (n > 0 && (d[n - 1] == ' ' || d[n - 1] == 0)) d[--n] = 0;
}

static int ide_probe(struct DFUnit *un)
{
    static UWORD id[256];
    UBYTE s;
    int i;

    IDE_CTL = CTL_NIEN;
    IDE(I_DEV) = (UBYTE)(0xa0 | (un->drive << 4));
    pause400();
    s = wait_ready(T_PROBE);
    if (s == 0xff) return 0;                         /* nothing, or floating */
    IDE(I_COUNT) = 0x55; IDE(I_LBA0) = 0xaa;
    if (IDE(I_COUNT) != 0x55 || IDE(I_LBA0) != 0xaa) return 0;
    IDE(I_CMD) = 0xec;                               /* IDENTIFY DEVICE */
    pause400();
    s = wait_ready(T_BUSY);
    if (s == 0xff) return 0;
    if (!(s & ST_DRQ)) {
        if (IDE(I_LBA1) != 0x14 || IDE(I_LBA2) != 0xeb)
            return 0;                                /* no drive answers (the master does not for an absent slave) */
        un->atapi = 1;
        IDE(I_CMD) = 0xa1;                           /* IDENTIFY PACKET DEVICE */
        pause400();
        s = wait_ready(T_BUSY);
        if (s == 0xff || !(s & ST_DRQ)) return 0;
    }
    for (i = 0; i < 256; i++) id[i] = IDE_DATA;
    wait_ready(T_PROBE);
    for (i = 0; i < 256; i++) id[i] = (UWORD)((id[i] << 8) | (id[i] >> 8));
    idstr(un->model, id + 27, 20);
    idstr(un->fw, id + 23, 4);
    un->removable = (id[0] & 0x80) != 0;
    if (un->atapi) {
        un->devtype = (UBYTE)((id[0] >> 8) & 0x1f);
        un->blocks = 0;                              /* READ CAPACITY, from the device */
        un->bsize = 0;
        return 1;
    }
    un->devtype = 0;
    un->bsize = 512;
    un->cyl = id[1]; un->heads = id[3]; un->secs = id[6];
    un->lba = (id[49] & 0x0200) != 0;
    un->lba48 = (id[83] & 0xc400) == 0x4400;
    if (un->lba48)
        un->blocks = (id[102] || id[103]) ? 0xffffffffUL : ((ULONG)id[101] << 16) | id[100];
    else if (un->lba)
        un->blocks = ((ULONG)id[61] << 16) | id[60];
    else
        un->blocks = (ULONG)un->cyl * un->heads * un->secs;
    if (!un->heads || !un->secs) { un->heads = 16; un->secs = 63; }
    if (un->lba) {                                   /* geometry for HDToolBox: from the size */
        ULONG c = un->blocks / ((ULONG)un->heads * un->secs);
        un->cyl = c > 0xffff ? 0xffff : (UWORD)c;
    }
    return un->blocks != 0 && (un->lba || (un->cyl && un->heads && un->secs));
}

/* ---- scsi.device ------------------------------------------------------- */

static struct Device *sdev;
static APTR sd_old_open, sd_old_close, sd_old_beginio, sd_old_abortio;
static struct Unit *sd_unit[2];                       /* its units 0 and 1 */
static struct IOStdReq sd_io[2];                      /* our opens of them, never closed */
static struct IOStdReq *drain_io;                     /* goes to the old BeginIO */
static UBYTE taken;

/* DF_DIAG (0x7ff2): what the takeover found, and the last scsi.device request
 * whose unit matched nothing (tools/flystat DIAG) */
static struct { ULONG sd_unit[2], ide_unit[2], bad_count, bad_unit, bad_cmd, bad_task; } diag;

ULONG ide_diag(APTR buf, ULONG len)
{
    diag.sd_unit[0] = (ULONG)sd_unit[0]; diag.sd_unit[1] = (ULONG)sd_unit[1];
    diag.ide_unit[0] = (ULONG)ide_units[0]; diag.ide_unit[1] = (ULONG)ide_units[1];
    if (len > sizeof(diag)) len = sizeof(diag);
    memcpy(buf, &diag, len);
    return len;
}

/* A request on a scsi.device unit opened before the takeover. While the
 * takeover is still running (draining scsi.device, then identifying the
 * drives) the drive is not known yet: wait for the takeover (it holds the
 * bus semaphore) instead of failing the request - a file system writing
 * just then got "bad unit" errors on the real A1200. */
struct DFUnit *ide_mapped(struct Unit *u)
{
    int k;
    for (k = 0; k < 2; k++) {
        if (!u || u != sd_unit[k]) continue;
        if (!taken) {
            ObtainSemaphore(&buslock);
            ReleaseSemaphore(&buslock);
        }
        return ide_units[k];
    }
    return NULL;
}

static int sd_known(struct Unit *u)
{
    return u && (u == sd_unit[0] || u == sd_unit[1]);
}

static void sd_open(REG(a1, struct IOStdReq *io), REG(d0, ULONG unit), REG(d1, ULONG flags),
                    REG(a6, struct Device *dev))
{
    if (unit < 2) {                                   /* the IDE port: ours now */
        struct DFUnit *un = ide_units[unit];
        if (!un) { io->io_Error = TDERR_BadUnitNum; return; }
        un->u.unit_OpenCnt++;
        dfdev->dd_Library.lib_OpenCnt++;
        io->io_Unit = &un->u;
        io->io_Error = 0;
        return;
    }
    df_open(io, unit, flags, dfdev);                  /* SCSI ids 2-6 */
}

static BPTR sd_close(REG(a1, struct IOStdReq *io), REG(a6, struct Device *dev))
{
    if (df_ours(io->io_Unit))
        return df_close(io, dfdev);
    return (BPTR)df_callvec(sd_old_close, io, dev, 0, 0);
}

static void sd_beginio(REG(a1, struct IOStdReq *io), REG(a6, struct Device *dev))
{
    UWORD c = io->io_Command;
    if (df_ours(io->io_Unit)) {
        df_beginio(io, dfdev);
        return;
    }
    /* opened before the takeover: change interrupts stay where they were added */
    if (io == drain_io || c == TD_ADDCHANGEINT || c == TD_REMCHANGEINT || !sd_known(io->io_Unit)) {
        if (io == drain_io || c == TD_ADDCHANGEINT || c == TD_REMCHANGEINT) {
            df_callvec(sd_old_beginio, io, dev, 0, 0);
            return;
        }
        diag.bad_count++;
        diag.bad_unit = (ULONG)io->io_Unit;
        diag.bad_cmd = c;
        diag.bad_task = (ULONG)FindTask(NULL);
        io->io_Error = TDERR_BadUnitNum;             /* a unit we found no drive for */
        if (!(io->io_Flags & IOF_QUICK)) ReplyMsg(&io->io_Message);
        return;
    }
    df_beginio(io, dfdev);
}

static LONG sd_abortio(REG(a1, struct IOStdReq *io), REG(a6, struct Device *dev))
{
    if (df_ours(io->io_Unit))
        return df_abortio(io, dfdev);
    if (io->io_Command == TD_ADDCHANGEINT)
        return (LONG)df_callvec(sd_old_abortio, io, dev, 0, 0);
    return 0;                                         /* ours finish on their own */
}

/* before the card's port is switched for the first time; with the task up */
int ide_takeover(void)
{
    static UBYTE buf[2048];
    struct MsgPort port;
    int k;

    if (taken) return 1;
    ObtainSemaphore(&buslock);
    if (taken) { ReleaseSemaphore(&buslock); return 1; }
    memset(&port, 0, sizeof(port));
    port.mp_Node.ln_Type = NT_MSGPORT;
    port.mp_Flags = PA_SIGNAL;
    port.mp_SigBit = SIGB_SINGLE;
    port.mp_SigTask = FindTask(NULL);
    port.mp_MsgList.lh_Head = (struct Node *)&port.mp_MsgList.lh_Tail;
    port.mp_MsgList.lh_TailPred = (struct Node *)&port.mp_MsgList.lh_Head;

    Forbid();
    sdev = (struct Device *)FindName(&SysBase->DeviceList, (STRPTR)"scsi.device");
    Permit();
    if (sdev) {
        for (k = 0; k < 2; k++) {
            memset(&sd_io[k], 0, sizeof(sd_io[k]));
            sd_io[k].io_Message.mn_ReplyPort = &port;
            sd_io[k].io_Message.mn_Length = sizeof(sd_io[k]);
            if (OpenDevice((STRPTR)"scsi.device", k, (struct IORequest *)&sd_io[k], 0) == 0)
                sd_unit[k] = sd_io[k].io_Unit;
        }
        Forbid();
        sd_old_open    = SetFunction((struct Library *)sdev, -6,  (APTR)sd_open);
        sd_old_close   = SetFunction((struct Library *)sdev, -12, (APTR)sd_close);
        sd_old_beginio = SetFunction((struct Library *)sdev, -30, (APTR)sd_beginio);
        sd_old_abortio = SetFunction((struct Library *)sdev, -36, (APTR)sd_abortio);
        Permit();
        /* what scsi.device has queued runs to the end before this read */
        for (k = 0; k < 2; k++) {
            if (!sd_unit[k]) continue;
            drain_io = &sd_io[k];
            sd_io[k].io_Command = CMD_READ;
            sd_io[k].io_Data = buf;
            sd_io[k].io_Length = 512;
            sd_io[k].io_Offset = 0;
            SetSignal(0, SIGF_SINGLE);
            DoIO((struct IORequest *)&sd_io[k]);
        }
        drain_io = NULL;
    }
    dfdev->dd_Library.lib_OpenCnt++;                  /* our code is in scsi.device now: no expunge */
    SEL_IDE();
    IDE_CTL = CTL_NIEN;
    for (k = 0; k < 2; k++) {
        struct DFUnit *un = AllocMem(sizeof(*un), MEMF_PUBLIC | MEMF_CLEAR);
        if (!un) continue;
        un->ide = 1;
        un->drive = (UBYTE)k;
        un->u.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        un->u.unit_MsgPort.mp_Flags = PA_IGNORE;
        DF_NEWLIST(&un->chgints);
        if (ide_probe(un))
            ide_units[k] = un;
        else
            FreeMem(un, sizeof(*un));
    }
    taken = 1;
    ReleaseSemaphore(&buslock);
    return 1;
}
