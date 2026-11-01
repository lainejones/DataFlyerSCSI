/* rdbmountcore.h - device-agnostic RDB partition mounter (DFMount core).
 * Reads the RigidDiskBlock chain from <device>/<unit>, MakeDosNode()s each
 * partition and AddBootNode()s it (ADNF_STARTPROC) so the ROM FFS handles
 * it. Include after defining RM_PRINTF (may be empty for ROM use).
 * Caller supplies SysBase/DOSBase/ExpansionBase. Pure stack/heap state.
 */
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>

#define RDSK_ID 0x5244534BUL   /* 'RDSK' */
#define PART_ID 0x50415254UL   /* 'PART' */
#define PBFF_NOMOUNT 2UL

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

/* returns number of partitions mounted, -1 = no RDSK on unit */
static LONG rm_mount_unit(struct ExecBase *SysBase, struct DosLibrary *DOSBase,
                          struct ExpansionBase *ExpansionBase,
                          const char *devname, ULONG unit, BOOL force)
{
    struct MsgPort *port;
    struct IOStdReq *io;
    UBYTE *buf;
    LONG found = -1, mounted = 0;
    ULONG blocksize, bufsize, partlist = 0xFFFFFFFF;
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
                node = (struct DeviceNode *)MakeDosNode(packet);
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
