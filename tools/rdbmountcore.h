/* rdbmountcore.h - device-agnostic RDB partition mounter (DFMount core).
 * Reads the RigidDiskBlock chain from <device>/<unit>, MakeDosNode()s each
 * partition, gives it its file system and AddBootNode()s it (ADNF_STARTPROC).
 * The file system is the one for the partition's DosType in
 * FileSystem.resource, or the one the RDB carries (FSHD + LSEG blocks) when
 * that is newer or the only one - loaded and added to the resource, as the
 * boot ROM does for boot drives. A DOS\x partition with neither gets the
 * ROM FFS; any other type with neither is not mounted.
 * Include after defining RM_PRINTF (may be empty for ROM use).
 * Caller supplies SysBase/DOSBase/ExpansionBase. Pure stack/heap state.
 */
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <resources/filesysres.h>

#define RDSK_ID 0x5244534BUL   /* 'RDSK' */
#define PART_ID 0x50415254UL   /* 'PART' */
#define FSHD_ID 0x46534844UL   /* 'FSHD' */
#define LSEG_ID 0x4C534547UL   /* 'LSEG' */
#define PBFF_NOMOUNT 2UL
#define RM_MAXFS (1024UL * 1024)   /* a file system bigger than this is refused */

static ULONG rm_long(const UBYTE *p) {
    return ((ULONG)p[0]<<24)|((ULONG)p[1]<<16)|((ULONG)p[2]<<8)|p[3];
}

static LONG rm_read(struct ExecBase *SysBase, struct IOStdReq *io,
                    APTR buf, ULONG block, ULONG blocksize)
{
    io->io_Command = CMD_READ;
    io->io_Data    = buf;
    io->io_Length  = blocksize;
    io->io_Offset  = block * blocksize;
    return DoIO((struct IORequest *)io);
}

/* the unit's block size (TD_GETGEOMETRY), 512 when the driver has none;
 * the RDSK search reads blocks of this size (a 2048-byte MO or CD-ROM
 * refuses 512-byte reads), and the buffer is this big */
static ULONG rm_blocksize(struct ExecBase *SysBase, struct IOStdReq *io)
{
    struct DriveGeometry dg;
    ULONG bs;
    io->io_Command = TD_GETGEOMETRY;
    io->io_Data    = &dg;
    io->io_Length  = sizeof(dg);
    io->io_Offset  = 0;
    if (DoIO((struct IORequest *)io) != 0) return 512;
    bs = dg.dg_SectorSize;
    if (bs < 256 || bs > 4096 || (bs & (bs - 1))) return 512;
    return bs;
}

/* the newest FileSystem.resource entry for dostype, or NULL */
static struct FileSysEntry *rm_fs_find(struct ExecBase *SysBase, ULONG dostype)
{
    struct FileSysResource *fsr = OpenResource((STRPTR)FSRNAME);
    struct FileSysEntry *fse, *best = NULL;

    if (!fsr) return NULL;
    Forbid();
    for (fse = (struct FileSysEntry *)fsr->fsr_FileSysEntries.lh_Head; fse->fse_Node.ln_Succ;
         fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ)
        if (fse->fse_DosType == dostype && (!best || fse->fse_Version > best->fse_Version))
            best = fse;
    Permit();
    return best;
}

/* The RDB's file system for dostype (FSHD list from block fshd): when the
 * resource has none, or an older one, its LSEG blocks are loaded (through a
 * file in T:, for LoadSeg) and added to FileSystem.resource. buf is a free
 * block-sized buffer. Returns the entry to use, or what the resource had. */
static struct FileSysEntry *rm_fs_rdb(struct ExecBase *SysBase, struct DosLibrary *DOSBase,
                                      struct IOStdReq *io, UBYTE *buf, ULONG blocksize,
                                      ULONG fshd, ULONG dostype, struct FileSysEntry *have)
{
    struct FileSysResource *fsr;
    struct FileSysEntry *fse;
    ULONG i, n, lseg, len = 0, k;
    UBYTE *code = NULL, hdr[84];
    BPTR seg, fh;

    for (i = 0; fshd != 0xFFFFFFFF && i < 16; i++, fshd = rm_long(buf + 16)) {
        if (rm_read(SysBase, io, buf, fshd, blocksize) != 0 || rm_long(buf) != FSHD_ID)
            return have;
        if (rm_long(buf + 32) == dostype)
            break;
    }
    if (fshd == 0xFFFFFFFF || i == 16)
        return have;                                    /* the RDB has none for it */
    if (have && have->fse_Version >= rm_long(buf + 36))
        return have;                                    /* the loaded one is as new */
    for (k = 0; k < 84; k++) hdr[k] = buf[k];           /* DosType .. GlobalVec */
    if (!(code = AllocMem(RM_MAXFS, MEMF_PUBLIC)))
        return have;
    for (lseg = rm_long(buf + 72), n = 0; lseg != 0xFFFFFFFF && n < RM_MAXFS / 256; n++) {
        ULONG longs;
        if (rm_read(SysBase, io, buf, lseg, blocksize) != 0 || rm_long(buf) != LSEG_ID) break;
        longs = rm_long(buf + 4);                       /* lsb_SummedLongs: 5 of header */
        if (longs < 5 || longs * 4 > blocksize || len + (longs - 5) * 4 > RM_MAXFS) break;
        for (k = 0; k < (longs - 5) * 4; k++) code[len + k] = buf[20 + k];
        len += (longs - 5) * 4;
        lseg = rm_long(buf + 16);
    }
    if (lseg != 0xFFFFFFFF || !len) {                   /* a broken chain: don't load half */
        RM_PRINTF("  the RDB's file system $%08lx: its LSEG blocks could not be read\n", (unsigned long)dostype);
        FreeMem(code, RM_MAXFS);
        return have;
    }
    seg = 0;
    if ((fh = Open((STRPTR)"T:RDBMount.fs", MODE_NEWFILE))) {
        LONG w = Write(fh, code, len);
        Close(fh);
        if (w == (LONG)len) seg = LoadSeg((STRPTR)"T:RDBMount.fs");
        DeleteFile((STRPTR)"T:RDBMount.fs");
    }
    FreeMem(code, RM_MAXFS);
    if (!seg) {
        RM_PRINTF("  the RDB's file system $%08lx: LoadSeg failed\n", (unsigned long)dostype);
        return have;
    }
    /* the entry outlives this program: its name lives in the same allocation */
    if (!(fsr = OpenResource((STRPTR)FSRNAME)) ||
        !(fse = AllocMem(sizeof(*fse) + 16, MEMF_PUBLIC | MEMF_CLEAR))) {
        UnLoadSeg(seg);
        return have;
    }
    fse->fse_DosType    = rm_long(hdr + 32);
    fse->fse_Version    = rm_long(hdr + 36);
    fse->fse_PatchFlags = rm_long(hdr + 40) | 0x180;   /* SegList + GlobalVec always */
    fse->fse_Type       = rm_long(hdr + 44);
    fse->fse_Task       = (CPTR)rm_long(hdr + 48);
    fse->fse_Lock       = (BPTR)rm_long(hdr + 52);
    fse->fse_Handler    = (BSTR)rm_long(hdr + 56);
    fse->fse_StackSize  = rm_long(hdr + 60);
    fse->fse_Priority   = (LONG)rm_long(hdr + 64);
    fse->fse_Startup    = (BPTR)rm_long(hdr + 68);
    fse->fse_SegList    = seg;
    fse->fse_GlobalVec  = (BPTR)rm_long(hdr + 76);
    {
        static const char nm[] = "RDBMount";
        char *d = (char *)(fse + 1);
        for (k = 0; nm[k]; k++) d[k] = nm[k];
        fse->fse_Node.ln_Name = d;
    }
    Forbid();
    AddHead(&fsr->fsr_FileSysEntries, &fse->fse_Node);
    Permit();
    RM_PRINTF("  loaded the RDB's file system $%08lx, version %lu.%lu (%lu bytes)\n", (unsigned long)fse->fse_DosType,
              (unsigned long)(fse->fse_Version >> 16), (unsigned long)(fse->fse_Version & 0xffff), (unsigned long)len);
    return fse;
}

/* give a new DeviceNode the file system: the fields fse_PatchFlags names,
 * dn_Type (bit 0) .. dn_GlobalVec (bit 8), copied as the boot ROM does */
static void rm_fs_patch(struct DeviceNode *dn, struct FileSysEntry *fse)
{
    ULONG *dst = (ULONG *)&dn->dn_Type, *src = (ULONG *)&fse->fse_Type, b;

    for (b = 0; b < 9; b++)
        if (fse->fse_PatchFlags & (1UL << b))
            dst[b] = src[b];
}

/* returns number of partitions mounted, -1 = no RDSK on unit, -2 = no drive;
 * *nofs (may be NULL) counts the partitions left out for want of a file system */
static LONG rm_mount_unit(struct ExecBase *SysBase, struct DosLibrary *DOSBase,
                          struct ExpansionBase *ExpansionBase,
                          const char *devname, ULONG unit, BOOL force, LONG *nofs)
{
    struct MsgPort *port;
    struct IOStdReq *io;
    UBYTE *buf;
    LONG found = -1, mounted = 0;
    ULONG blocksize, bufsize, partlist = 0xFFFFFFFF, fshdlist = 0xFFFFFFFF;
    ULONG i;

    port = CreateMsgPort();
    if (!port) return 0;
    io = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
    if (!io) { DeleteMsgPort(port); return 0; }
    if (OpenDevice((STRPTR)devname, unit, (struct IORequest *)io, 0) != 0) {
        DeleteIORequest((struct IORequest *)io); DeleteMsgPort(port);
        return -2;
    }
    /* the buffer holds one block of the unit's size; every read into it is
     * that size or less (a 1024-byte buffer overflowed on 2048- and
     * 4096-byte-block RDBs). The fields read below all lie within 256 bytes. */
    bufsize = blocksize = rm_blocksize(SysBase, io);
    buf = AllocMem(bufsize, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf) {
        /* find RDSK in blocks 0..15 */
        for (i = 0; i < 16; i++) {
            if (rm_read(SysBase, io, buf, i, blocksize) != 0) continue;
            if (rm_long(buf) == RDSK_ID) {
                blocksize = rm_long(buf + 16);
                if (blocksize < 256 || blocksize > bufsize) blocksize = bufsize;
                partlist = rm_long(buf + 28);
                fshdlist = rm_long(buf + 32);   /* rdb_FileSysHeaderList */
                found = 0;
                break;
            }
        }
        /* walk PART chain */
        for (i = 0; found >= 0 && partlist != 0xFFFFFFFF && i < 16; i++) {
            UBYTE namebuf[32];
            ULONG flags, tabsize, j;
            ULONG *packet;

            if (rm_read(SysBase, io, buf, partlist, blocksize) != 0) break;
            if (rm_long(buf) != PART_ID) break;
            flags = rm_long(buf + 32);
            {
                UBYTE len = buf[36];
                if (len > 30) len = 30;
                for (j = 0; j < len; j++) namebuf[j] = buf[37 + j];
                namebuf[len] = 0;
            }
            partlist = rm_long(buf + 16);   /* pb_Next */

            if ((flags & PBFF_NOMOUNT) && !force) {
                RM_PRINTF("  %s: NOMOUNT flag - skipped\n", (char *)namebuf);
                continue;
            }
            /* skip if a DOS device of this name already exists */
            {
                struct DosList *dl = LockDosList(LDF_DEVICES | LDF_READ);
                struct DosList *hit = FindDosEntry(dl, (STRPTR)namebuf, LDF_DEVICES);
                UnLockDosList(LDF_DEVICES | LDF_READ);
                if (hit) {
                    RM_PRINTF("  %s: already mounted - skipped\n", (char *)namebuf);
                    continue;
                }
            }
            tabsize = rm_long(buf + 128);   /* de_TableSize */
            if (tabsize > 20) tabsize = 20;
            packet = AllocMem((tabsize + 1 + 4) * 4, MEMF_PUBLIC | MEMF_CLEAR);
            if (packet) {
                struct DeviceNode *node;
                packet[0] = (ULONG)namebuf;
                packet[1] = (ULONG)devname;
                packet[2] = unit;
                packet[3] = 0;
                for (j = 0; j <= tabsize; j++)
                    packet[4 + j] = rm_long(buf + 128 + j * 4);
                /* DosEnvec [14] = de_Mask: below $00FFFFFE (24-bit DMA) it cannot be a real
                 * memory mask - some partitioning tools wrote MaxTransfer and Mask
                 * the wrong way round (seen on a DataFlyer drive: MaxTransfer
                 * $FFFFFFFC, Mask $0001FE00). With it, FFS can never transfer into
                 * the caller's buffer and reads one block at a time. */
                if (tabsize >= 14 && packet[4 + 14] < 0x00fffffe) {
                    RM_PRINTF("  %s: the partition's Mask ($%08lx) is not a memory mask - using $7FFFFFFE\n",
                              (char *)namebuf, (unsigned long)packet[4 + 14]);
                    packet[4 + 14] = 0x7ffffffe;
                    if (packet[4 + 13] < 0x200 || packet[4 + 13] > 0x7fffffff)
                        packet[4 + 13] = 0x1fe00;       /* MaxTransfer: the usual value */
                }
                /* the file system for the partition's DosType (buf is free:
                 * this PART block has been read out) */
                {
                    ULONG dostype = tabsize >= 16 ? packet[4 + 16] : 0x444f5300;
                    struct FileSysEntry *fse = rm_fs_find(SysBase, dostype);
                    fse = rm_fs_rdb(SysBase, DOSBase, io, buf, blocksize, fshdlist, dostype, fse);
                    node = NULL;
                    if (!fse && (dostype & 0xffffff00) != 0x444f5300) {
                        RM_PRINTF("  %s: no file system for its type $%08lx (not in the RDB, not loaded) - not mounted\n",
                                  (char *)namebuf, (unsigned long)dostype);
                        if (nofs) (*nofs)++;
                    } else if ((node = (struct DeviceNode *)MakeDosNode(packet)) && fse) {
                        rm_fs_patch(node, fse);
                    }
                }
                if (node) {
                    /* DosEnvec: [15] = de_BootPri, [16] = de_DosType */
                    LONG bootpri = (tabsize >= 15) ? (LONG)packet[4 + 15] : 0;
                    if (AddBootNode(bootpri, ADNF_STARTPROC, node, NULL)) {
                        RM_PRINTF("  %s: mounted (unit %ld)\n", (char *)namebuf, (long)unit);
                        mounted++;
                    } else {
                        RM_PRINTF("  %s: AddBootNode failed\n", (char *)namebuf);
                    }
                }
                FreeMem(packet, (tabsize + 1 + 4) * 4);
            }
        }
        FreeMem(buf, bufsize);
    }
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    return found < 0 ? -1 : mounted;
}
