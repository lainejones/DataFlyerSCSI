# flysuite - the all-in-one test for dataflyer.device

`flysuite` is one CLI program that drives a unit through the normal device interface
(CMD_READ, TD64, NSD, HD_SCSICMD, the trackdisk queries). It prints one `PASS` / `FAIL` /
`SKIP` / `HANG` line per check, then a summary. It works with any trackdisk-style driver
(`DEVICE=`), but some checks and the counters are dataflyer.device's.

```
flysuite UNIT=<n> [DEVICE=<name>, default dataflyer.device] [UNIT2=<n>] [TESTS=<list>] [MINUTES=<n>] [SECONDS=<n>]
         [WRITEFILE=<vol:dir>] [RAWWRITE=<first>,<count>] [FUZZ] [FUZZN=<n>] [IKNOW] [DANGER]
         [TOOLBOX] [WORKERS=<n>] [TIMEOUT=<s>] [SEED=<n>] [LOG=<file>] [QUIET] [SKIPIDS=<id,id>]
         mix: [RAWWRITE2=<first>,<count>] [FILES=<dir>,<dir>,...]
         dataflyer.device only: [IDE=<unit>] [RAWIDE=<first>,<count>]
```

Units are numbered as in the driver: SCSI id + 10 x LUN; on dataflyer.device 100/101 = the IDE
master/slave. With any other `DEVICE=` every unit is a plain unit of that driver (see "Testing
another driver"). The header says which mode a run is in (`mode: ...`).

| Option | Meaning |
|---|---|
| `TESTS=` | Comma list: `conf,big,edge,conc,soak,toolbox,fuzz,mix,latency,dma16` and the audit tests (below). `all` = conf, big, edge, toolbox, dma16, latency, conc, soak, errmap, cdbpass (+ fuzz with `FUZZ`). `audit` = errmap, cdbpass, dirmis, cdlate, raceopen. Default `all`. |
| `MINUTES=` | Soak length (1). |
| `SECONDS=` | conc length, length of each mix phase, CDLATE's wait (20). |
| `WORKERS=` | conc reader tasks (4, max 9). |
| `UNIT2=` | A second unit for conc, mix and SLOWCMD. |
| `WRITEFILE=` | Soak file writes in this drawer (made and removed if missing). |
| `RAWWRITE=f,c` | Raw writes on UNIT, only in blocks f..f+c-1 (at most 1 MB). |
| `RAWWRITE2=`, `RAWIDE=` | The same for UNIT2 and the IDE unit (dataflyer.device only), for mix. |
| `IDE=` | The IDE unit for conc/mix (100). dataflyer.device only. |
| `FILES=` | Drawers for the mix's file copies, comma separated (`FLY0:,DATA2:,CF0:`). |
| `FUZZ`, `FUZZN=` | Turn the fuzzer on; how many CDBs (400). |
| `IKNOW` | Fuzz a non-emulator target; RAWWRITE over an RDB/partition that is not mounted. |
| `DANGER` | Allow ATAPIDIR and RUNEND (may wedge or freeze the machine). |
| `TOOLBOX` | Probe the BlueSCSI/ZuluSCSI Toolbox even when INQUIRY doesn't name one. |
| `SKIPIDS=` | SCSI ids never opened, not even by the absent-id check (a network unit). |
| `TIMEOUT=` | Seconds before a request is a HANG (30). |
| `SEED=` | Repeat a random run exactly (printed in the header). |
| `LOG=` | Everything, PASS lines too, into a file (flushed every line). |
| `QUIET` | Print only FAIL/SKIP/HANG/OUTLIER and the summary. |

Return code: **0** all passed (SKIPs allowed), **5** a FAIL, **10** a HANG or the unit would not
open, **20** bad arguments, no memory, or a refused request (FUZZ/RAWWRITE safety, network unit).

## Bounded waits

- Every request goes out with SendIO, and a timer.device request goes out with it. One that is
  not back after `TIMEOUT` is a **HANG**. The line shows the command, CDB, length and block.
  flysuite then tries AbortIO and waits 5 s more. A request that still does not come back is
  left with the driver: its port is set to PA_IGNORE, and its memory and buffers are never
  freed. That unit is not used again; the run ends with the summary.
- OpenDevice and every DOS call (soak files, mix files) run in a child process. The main task
  watches it: a child that has not moved for `TIMEOUT` (+15 s for workers) is a HANG, with what
  it was doing and the task's state. flysuite never quits while a child is still inside the
  driver, because the child's code is flysuite's.
- Any single request over 2 s is logged as an `OUTLIER` line, with the command, size and time.
  So is any DOS Read/Write call over 2 s in the soak or mix.

## The groups

**conf** (read-only), the HD_SCSICMD conformance:
- TEST UNIT READY, also with no buffer and with CmdLength 12/16.
- INQUIRY with allocation 5/36/255/37/7, into a 20-byte buffer, a 100-byte buffer, no buffer,
  allocation 0, and chip/fast RAM at odd addresses. Guard bytes after every buffer.
- REQUEST SENSE 18 and 13.
- READ CAPACITY(10), also sent with CmdLength 6 (driver zero-pads). READ CAPACITY(16).
- READ(6)/(10)/(12)/(16) of the same blocks compared with CMD_READ. Zero-length READ(10).
  READ(10) into odd chip/fast buffers. CMD_READ into an odd buffer.
- READ(10) of 2 blocks into a 612- and a 512-byte buffer (`scsi_Actual`, guard, the next
  command still works). One block into a 2-block buffer.
- MODE SENSE(6) page $3F, plus allocation 13. MODE SENSE(10) with allocation 1023 and 1024.
- CHECK CONDITION on purpose: READ past the end and opcode $FF.
  - With and without AUTOSENSE.
  - REQUEST SENSE by hand afterwards.
  - Sense into 255 bytes.
  - AUTOSENSE with no sense buffer.
- CmdLength 17 and a short io_Length (IOERR_BADLENGTH).
- The commands that work before they answer (the driver's long time budgets): VERIFY(10) of 16
  blocks, SYNCHRONIZE CACHE, SEND DIAGNOSTIC self-test. GOOD, or ILLEGAL REQUEST = SKIP; never a
  timeout error. (An emulator answers at once; this shows the budget on a real disk.)
- Every absent SCSI id opened twice, the second time through the driver's 3 s cache. Both opens
  must fail cleanly with TDERR_BadUnitNum within 3 s.
- LUN 1 of the unit, and invalid units 7/80/102.
- A vendor CDB keeps byte 1: Toolbox GET of an index >= 32.

"The TARGET sends less than its header says" means the target is at fault, not the driver; the
check then passes. WinUAE does this for INQUIRY and MODE SENSE(10). WinUAE also answers an
unknown opcode with ASC $24, not $20; both pass. A command the target does not have
(ILLEGAL REQUEST) is a SKIP.

**big** (read-only):
- A reference is read one block at a time (CMD_READ or TD64): the first and the last 1 MB.
- Then 1, 3, 126, 127, 128, 129, 130, 256, 2047 and 2048 blocks are read at block 0 and at the
  end, each with CMD_READ, TD_READ64, NSCMD_TD_READ64 and SCSI READ(10), and compared with the
  reference.
- On a pattern disk the reference is also checked against the pattern.
- Across 2 GB, across 4 GB and above 4 GB, when the unit is that big (else SKIP). The reference
  is single-block SCSI READ(10)s.

**edge** (read-only):
- Reads that end exactly at the end of the unit.
- One block past the end, at the block count, 1000 past, 130 blocks (two driver chunks) ending 1
  past, block $FFFFFFFF and block 2^32. All must fail, and the buffer must not hold block 0 (no
  wrap).
- Lengths and offsets that are not whole blocks. The driver's contract (dataflyer.c `rw`) is
  IOERR_BADLENGTH with nothing transferred. A length of 0 does nothing.
- TD_CHANGESTATE, TD_PROTSTATUS (compared with the MODE SENSE header's WP bit; a CD-ROM must say
  protected), TD_CHANGENUM, TD_GETGEOMETRY (also with a short length), NSCMD_DEVICEQUERY (also
  with an 8-byte buffer), an unknown command (IOERR_NOCMD), CMD_UPDATE (on dataflyer.device the
  flush must really go out: one command counted, also on a "removable" CF card), CMD_RESET (on
  dataflyer.device a SCSI bus reset: block 0 and the geometry must be the same right after it).
- OpenDevice/CloseDevice 100 times.
- Two opens of the unit with a request in flight on each at once.

**toolbox** (read-only; a BlueSCSI/ZuluSCSI Toolbox unit, or `TOOLBOX`):
- COUNT ($D2), and LIST ($D0): 40-byte entries, numbered in order.
- The vendor-CDB byte-1 check: GET from index 32 on.
- GET ($D1) of files 0, 1, 31, 33 and the last one, each twice: same bytes, size as listed.

**dma16** (read-only; writes only inside `RAWWRITE`): buffers above the 16 MB line.
- A Zorro II DMA controller (A2091, GVP Series II, ...) reaches only the first 16 MB; its driver
  has to copy through a buffer it can reach when the caller's buffer is higher - accelerator RAM
  on an A2000/A3000/A4000, or a TF/Blizzard board in an A1200.
- The buffer comes from a memory header that starts at or above `$01000000` (exec `Allocate()`).
  No such RAM: SKIP, and the line says why.
- 1, 16 and 128 blocks (at most 64 KB), at an even address and 2 bytes off, through CMD_READ,
  TD_READ64, NSCMD_TD_READ64 and SCSI READ(10), each compared with the same blocks read into chip
  RAM just before. Guard bytes after the buffer. A path the driver lacks is a SKIP.
- The blocks are a third of the way into the unit, away from block 0 and from an FFS root block.
  When a compare fails, the blocks are read again: if they changed (a file system writing there)
  the check is repeated once.
- With `RAWWRITE`: 16 blocks written from that RAM (even and 2 bytes off), read back into chip
  RAM, compared, then the saved blocks are written back and checked.

**latency** (read-only): does the rest of the system keep running while the driver works?
- A child process at priority 21 (above input.device) asks timer.device for 10 ms sleeps over and
  over and notes how late each comes back: 3 s idle, then 6 s while the main task reads 64 KB at a
  time through CMD_READ and SCSI READ(10) in turn.
- Nothing below priority 21 can make it late. A driver that holds `Forbid()` or `Disable()` through
  a transfer, or busy-polls in a task above 21, can - the user sees a pointer that freezes and a
  network that stalls during disk access.
- FAIL when the worst lateness under load is more than 50 ms above the idle worst. The line gives
  both, the read speed, and how many sleeps were over 20 ms. If the idle worst is already over
  50 ms, a note says that something else on the machine holds the CPU and the result says little.
- Checked against a program that holds `Forbid()` for 100 ms at a time: the sampler saw 95-99 ms.

**conc**: `WORKERS` reader processes, each with its own IORequest, on UNIT. With UNIT2, every
other reader is on UNIT2. Plus one AbortIO worker, and one reader on the IDE unit if it opens.
- For `SECONDS`, random reads of 1-128 blocks through all four read paths.
- Each read is checked against the pattern, or against a second read through another path.
- Afterwards the unit must answer normally.

**soak** (`MINUTES`), one loop that interleaves the enabled modes:
- **WRITEFILE**: files of 1 KB-20 MB (capped at a third of the free space) are written in 64 KB
  calls with a regenerable pseudo-random content. Each is read back and compared. The previous
  file is read again, then deleted. Everything is removed at the end, and the drawer too if
  flysuite made it.
- **RAWWRITE**:
  - The whole range is saved to RAM first, with the block before and after it.
  - Then: random writes of 1-256 blocks through CMD_WRITE / TD_WRITE64 / NSCMD_TD_WRITE64 /
    SCSI WRITE(10), each read back through a rotating read path and compared.
  - At the end, also after an error, a break or a refused write, the saved data is written back,
    read back and compared, and the neighbours are checked.
  - A write outside the range is refused inside flysuite and never sent.
- **Neither**: read-only. Random reads, each read twice through two different paths, plus the
  pattern.

**fuzz** (`FUZZ`; refused unless INQUIRY names an emulator - "UAE" or "QEMU" - or `IKNOW`):
- Random CDBs: 60 % from a list of read-only commands, 40 % any opcode below $C0 not on the
  never-list. The never-list covers every write, format, erase, mode/log select, reserve/release,
  start/stop, prevent/allow, send diagnostic, cache lock, copy, ATA pass-through, security-out
  and vendor opcode.
- Transfer-length fields are clamped (at most 64 blocks, 16 for 6-byte CDBs). Half the LBAs are
  real ones.
- Random CmdLength 0-16, data lengths 0 to 8 KB, both directions, with or without AUTOSENSE.
- Pass: every request comes back in time, `scsi_Actual` <= `scsi_Length`, no guard byte is
  overwritten (data or sense), and the unit still answers with the same block 0 afterwards.

**mix** (`TESTS=mix`): SCSI and IDE, reads and writes, raw and files, at the same time, in phases of
`SECONDS` each:

| Phase | UNIT | UNIT2 | IDE | files |
|---|---|---|---|---|
| 1 | read | | read | |
| 2 | write | | read | |
| 3 | read | | write | |
| 4 | write | | write | |
| 5 | write | write | read | |
| 6 | write | write | write | |
| 7 | write | write | write | all FILES |
| 8 | read | read | read | all FILES |

- A "write" unit writes in its own range (RAWWRITE / RAWWRITE2 / RAWIDE) and reads the range back.
  It keeps a shadow of the range and checks all of it at the end of the phase.
- With no range, the writes come from file copies in a FILES drawer that lives on that unit
  (flysuite finds the unit behind each drawer).
- Readers read the whole unit at random and check every block they can: the shadow inside the
  range, the pattern outside it, and two reads through different paths agreeing. Reads inside a
  volume that a file worker is writing in that phase are made, but not compared.
- File workers make `<dir>/flysuite.mixN`, write, read back, re-read and delete files of
  1 KB-2 MB, then remove the drawer.
- Every range is saved before phase 1. After the last phase, and after any error or break, it is
  written back and verified.

## The audit tests (docs/audit-2026-10-31.md)

| Test | Finding | Safe? | What it does |
|---|---|---|---|
| `errmap` | H5 | read-only (in `all`) | CMD_READ/TD64/NSD past the end must not give TDERR_BadSecSum/WriteProt. With RAWWRITE (the unit is scratch), also CMD_WRITE/TD_WRITE64 past the end (the target refuses them). NOT READY on a removable unit with no medium. Bad opcode = HFERR_BadStatus. |
| `cdbpass` | M1 | read-only (in `all`) | INQUIRY with byte 1 = $20 on LUN 0. If the target has no LUN 1 (unit+10 doesn't open) and LUN 0's answer comes back, the driver cleared the bits. Also TUR with byte 1 = $E0 and READ(10) with RDPROTECT = 7 (information). |
| `dirmis` | H6 | writes only with RAWWRITE (the first 2 blocks, saved/restored) | INQUIRY with SCSIF_WRITE; WRITE(10) of 1 block sent with SCSIF_READ; WRITE(10) of 2 blocks with a 1-block scsi_Length. |
| `cdlate` | H4 | read-only | A removable unit opened with no disc: insert one within SECONDS, then CMD_READ of sector 16 must hold CD001. **Manual**: it needs a disc inserted by hand. |
| `raceopen` | M3 | read-only | 4 processes at priority 10 do the very first OpenDevice at once, then CMD_READ. Needs a fresh boot: it runs before flysuite's own open and SKIPs if the device is already loaded. |
| `slowcmd` | H1 | START STOP UNIT stop + start (IMMED=0), named only | Times the start; then TEST UNIT READY on the unit and on UNIT2. Needs a real mechanical disk (> 10 s spin-up) to show anything. |
| `atapidir` | H2 | **DANGER**, named only, runs last | INQUIRY (then MODE SENSE) with scsi_Flags = 0 to an IDE ATAPI unit (100/101; on another driver any CD-ROM unit), 15 s limit. flysuite raises itself to priority 5 so its timer still runs if the driver task spins. Run it in its own boot. |
| `runend` | H3 | **DANGER**, emulator only, runs last | READ(10) into a longer buffer whose data is not a multiple of 256 bytes (needs 520/2352-byte blocks; on 512 it only runs the safe case). A freeze is the failure. |

## Safety rules

- Nothing is written to a unit except:
  - inside RAWWRITE / RAWWRITE2 / RAWIDE;
  - by DIRMIS (the first two blocks of RAWWRITE);
  - by the ERRMAP write half (past the end of a RAWWRITE unit; the target refuses those).

  Files are written only in WRITEFILE / FILES drawers.
- A raw range is at most 1 MB. Before it is used, flysuite reads the RDB and every RDB partition
  (it lists them all, so the free gaps show) and every mounted DOS device on that unit (also
  scsi.device unit n for IDE unit 100+n).
  - Overlapping the RDB or a partition is refused; `IKNOW` overrides only that.
  - Overlapping a **mounted** volume, or going past the unit's end, is always refused.
- Every raw range is saved to RAM before the first write. It is written back and verified at the
  end, on errors, on a break (CTRL-C) and on refused writes. Only a unit whose request got stuck
  cannot be restored; that is reported with the exact blocks.
- `FUZZ` and `RUNEND` run only on an emulator's unit (`FUZZ` + `IKNOW` for a real one). RUNEND
  also needs `DANGER`.
- A network/processor unit (INQUIRY type 3 or 9, or "DaynaPORT") is refused as UNIT or UNIT2.
  `SKIPIDS=5` also keeps the absent-id check from opening it.

## Testing in WinUAE

- "DataFlyer test": id 3 = `scsi3.hdf`, 64 MB scratch with DF3 at blocks 32-98335, so 98336+ is
  free for RAWWRITE. Id 4 = the pattern disk.
- "DataFlyer test IDE" adds `ide0.hdf` as IDE unit 100: IDE0: at blocks 32-57375, 57376+ free.
- "DataFlyer test CD" adds an IDE CD-ROM as unit 101.
- `RDBMount UNITS=3` mounts DF3: for WRITEFILE/FILES.

```
flysuite UNIT=4 TESTS=all
flysuite UNIT=3 UNIT2=4 TESTS=all,dirmis RAWWRITE=100000,2048 WRITEFILE=DF3:flysoak MINUTES=3
flysuite UNIT=3 UNIT2=4 TESTS=conc,mix RAWWRITE=100000,2048 RAWWRITE2=40000,2048 RAWIDE=60000,2048 FILES=DF3:,IDE0:
flysuite UNIT=4 TESTS=fuzz FUZZ FUZZN=800
flysuite UNIT=4 TESTS=raceopen          (first command of a fresh boot)
flysuite UNIT=101 TESTS=atapidir DANGER (CD config, own boot, last)
```

## Testing another driver

flysuite works with any trackdisk-style driver that has HD_SCSICMD: `DEVICE=csascsi.device`
(a path such as `DEVS:csascsi.device` works too; mount entries are matched by the name alone).
Nothing else is needed: no RDBMount, no ENV: files - mount the partitions with the driver's own
tools if you want WRITEFILE/FILES.

What changes when `DEVICE=` is not dataflyer.device (the header says `mode: another driver`):

- Every unit is a plain SCSI/disk unit of that driver, also 100 and up. `IDE=` and `RAWIDE=` are
  ignored (with a note), conc has no IDE reader, mix has no IDE role.
- The RAWWRITE safety checks look only at the RDB and at volumes mounted on that driver (on
  dataflyer.device also at scsi.device units 0/1, which it takes over).
- The driver counters (DF_STATS) are not read.
- Checks that test dataflyer.device's own contract accept any clean refusal: an absent SCSI id may
  fail with any error (still within 3 s), a CMD_READ that is not whole blocks with any error (still
  nothing transferred, buffer untouched); unit 102 is not tried as an invalid unit.
- AUTOSENSE with no sense buffer runs only with `DANGER`: a driver that writes the sense anyway
  writes to address 0 and up and kills the machine (OS 3.2's own scsi.device does, in WinUAE).

Which groups apply: all of them. conf, big, edge, errmap and cdbpass are read-only and a good
first run; conc and soak next; mix with two units (`UNIT2=`) and `FILES=` on the driver's volumes;
fuzz only on an emulator's unit (or with `IKNOW`). The safety rules below hold the same: raw
writes only inside RAWWRITE / RAWWRITE2, refused over an RDB partition or a mounted volume, saved
and written back.

What the audit tests mean for a driver that isn't ours (each one names the dataflyer.device finding
in docs/audit-2026-10-31.md it came from, but asks a question any driver should answer well):

| Test | The question |
|---|---|
| `errmap` | A read/write past the end: a seek/address error, not "bad sector" or "write protected"? A bad opcode: HFERR_BadStatus with sense? |
| `cdbpass` | Does CDB byte 1 reach the target as the caller built it (LUN 0)? |
| `dirmis` | A WRITE sent with SCSIF_READ, an INQUIRY with SCSIF_WRITE, a WRITE longer than its buffer: an error, and no invented data on the disk? |
| `raceopen` | Four first OpenDevice calls at once (fresh boot): one unit, four good reads? |
| `slowcmd` | A start that takes longer than the driver's time-out (a real slow disk)? |
| `cdlate`, `atapidir` | A CD inserted after the open; INQUIRY to a CD-ROM with the direction flag wrong (DANGER: may hang the machine). |
| `runend` | A READ whose data is not a multiple of 256 bytes into a longer buffer (DANGER, emulator). |

A FAIL there means the driver does something other than what flysuite expects of a careful
driver; read the line - it names the command and what came back.

## On the A1200

ZuluSCSI id 3 (FLY0:) and 4 (DATA2:), id 2 = CD, **id 5 = DaynaPORT: never a UNIT, always
`SKIPIDS=5`**. CF0: is IDE unit 100.

1. Read-only, each unit:
   ```
   flysuite UNIT=3 SKIPIDS=5 TESTS=all MINUTES=5 LOG=RAM:fs-u3.log
   flysuite UNIT=4 SKIPIDS=5 TESTS=all MINUTES=5 LOG=RAM:fs-u4.log
   flysuite UNIT=100 SKIPIDS=5 TESTS=all MINUTES=5 LOG=RAM:fs-u100.log
   flysuite UNIT=2 SKIPIDS=5 TESTS=conf,big,edge,errmap LOG=RAM:fs-cd.log
   flysuite UNIT=3 SKIPIDS=5 TESTS=toolbox TOOLBOX
   ```
2. Find a free raw range. This is refused, but it lists every partition:
   `flysuite UNIT=3 SKIPIDS=5 TESTS=soak RAWWRITE=0,1`. Pick up to 2048 blocks outside them,
   below the unit's end. Same for 4 and 100. No gap means no raw writes on that unit; use files.
3. Soak through the file systems:
   ```
   flysuite UNIT=3 SKIPIDS=5 TESTS=soak WRITEFILE=FLY0:flysoak MINUTES=30 LOG=RAM:fs-soak3.log
   flysuite UNIT=4 SKIPIDS=5 TESTS=soak WRITEFILE=DATA2:flysoak MINUTES=30
   flysuite UNIT=100 SKIPIDS=5 TESTS=soak WRITEFILE=CF0:flysoak MINUTES=30
   ```
   With a free range, add `RAWWRITE=<first>,2048` to the first one.
4. Every SCSI/IDE combination at once:
   ```
   flysuite UNIT=3 UNIT2=4 IDE=100 SKIPIDS=5 TESTS=conc,mix SECONDS=60 FILES=FLY0:,DATA2:,CF0: LOG=RAM:fs-mix.log
   ```
   Add `RAWWRITE=`, `RAWWRITE2=` and `RAWIDE=` with free ranges to add raw writes to every write
   phase. Without them, the write phases use the file copies on that unit's volume.
5. The audit on a scratch range: `flysuite UNIT=3 SKIPIDS=5 TESTS=audit RAWWRITE=<first>,16`.
   RACEOPEN needs dataflyer.device unopened, which a boot that runs RDBMount never is: comment
   that line out of S:User-Startup for one boot, then run `flysuite UNIT=3 SKIPIDS=5 TESTS=raceopen`
   first.
