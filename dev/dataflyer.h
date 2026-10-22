/*
 * dataflyer.device - what dataflyer.c (the device, the SCSI side) and ide.c
 * (the built-in IDE port and the scsi.device takeover) share.
 * Part of DataFlyerSCSI. GPL-3.0-only, see LICENSE.
 */
#ifndef DATAFLYER_H
#define DATAFLYER_H

#include <exec/types.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/semaphores.h>

#define REG(r, decl) register decl __asm(#r)

#define SEL_SCSI()  ((void)*(volatile UWORD *)0xDA1012UL)
#define SEL_IDE()   ((void)*(volatile UWORD *)0xDA1016UL)
#define CIA_PRA     (*(volatile UBYTE *)0xBFE001UL)   /* a read takes about 1.4 us */

/* unit numbers: SCSI id + 10 * LUN; the built-in IDE port's two drives: */
#define IDE_UNIT0   100              /* master; 101 = slave */

struct DFUnit {
    struct Unit u;
    UBYTE id, lun, removable, devtype;
    ULONG blocks, bsize;             /* from READ CAPACITY (0 = not read yet) */
    ULONG changes;
    struct List chgints;             /* pending TD_ADDCHANGEINT requests (their io_Data = the Interrupt) */
    UBYTE medium;                    /* what the last poll saw: 0 unknown, 1 a medium in, 2 none */
    UBYTE quiet;                     /* polls left in which an attention alone is not a new change */
    /* the built-in IDE port */
    UBYTE ide;                       /* 1 = a drive on the IDE port */
    UBYTE drive;                     /* 0 master, 1 slave */
    UBYTE atapi;                     /* 1 = ATAPI (packet commands = SCSI CDBs) */
    UBYTE lba, lba48;
    UBYTE skey, asc, ascq;           /* sense of the last ATA command */
    UWORD cyl, heads, secs;
    char model[41], fw[9];
};

/* an empty exec list (no NewList() without amiga.lib) */
#define DF_NEWLIST(l) do { (l)->lh_Head = (struct Node *)&(l)->lh_Tail; (l)->lh_Tail = NULL; \
                           (l)->lh_TailPred = (struct Node *)&(l)->lh_Head; } while (0)

extern struct ExecBase *SysBase;
extern struct Device *dfdev;
extern struct SignalSemaphore buslock;

/* request statistics, read with DF_STATS (tools/flystat) */
#define DF_STATS 0x7ff0
struct DFStats {
    ULONG reads, writes, rbytes, wbytes, cmds;
    ULONG size[8];       /* requests by size: <=512, <=2K, <=8K, <=32K, <=64K, <=128K, <=512K, more */
    ULONG polls[8];      /* wait-loop polls by place (each ~1.4 us on real hardware), see W_ */
    ULONG timeouts;
    ULONG sleeps;        /* timer sleeps (250 us) inside waits */
};
extern struct DFStats stats;
#define W_BUSFREE 0
#define W_SELECT  1
#define W_ACK     2
#define W_REQ     3
#define W_BSYEND  4
#define W_DMAIN   5
#define W_DMAOUT  6
#define W_IDE     7

void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);

/* dataflyer.c */
int  df_ours(struct Unit *u);
int  df_pause(ULONG spins);        /* one wait step: a CIA read, or a 250 us sleep after SPIN_POLLS */
void df_open(REG(a1, struct IOStdReq *io), REG(d0, ULONG unitnum), REG(d1, ULONG flags),
             REG(a6, struct Device *dev));
BPTR df_close(REG(a1, struct IOStdReq *io), REG(a6, struct Device *dev));
void df_beginio(REG(a1, struct IOStdReq *io), REG(a6, struct Device *dev));
LONG df_abortio(REG(a1, struct IOStdReq *io), REG(a6, struct Device *dev));

/* ide.c */
extern struct DFUnit *ide_units[2];
int  ide_takeover(void);
ULONG ide_diag(APTR buf, ULONG len);
struct DFUnit *ide_mapped(struct Unit *u);
int  ide_cmd(struct DFUnit *un, const UBYTE *cdb, int cdblen, UBYTE *data, ULONG len, int out,
             ULONG *actual);

/* start.S */
ULONG df_dmain(UBYTE *p, ULONG n);
ULONG df_dmain_byte(UBYTE *p, ULONG n);
ULONG df_dmaout(const UBYTE *p, ULONG n);
ULONG df_dmaout_run(const UBYTE *p, ULONG n);
void  df_idein(APTR p);              /* one 512-byte sector through the IDE data port */
void  df_ideout(const void *p);
ULONG df_callvec(APTR fn, APTR a1, APTR a6, ULONG d0, ULONG d1);

#endif
