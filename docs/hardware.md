# DataFlyer 1200 SCSI+ - the hardware, as the drivers see it

Expansion Systems' DataFlyer 1200 SCSI+ sits on the A1200's internal IDE (Gayle) port and
adds an NCR 53C80 SCSI controller next to it. The board has no autoconfig and no ROM; the
original software is Wayne E. Miller's `ExpXDS.device` (units `IDE_XDS` / `SCSI_XDS`),
`SCSIPlus` (installs it by patching `scsi.device`) and `DFMount` (mounts RDB partitions).
No sources exist; this map comes from two independent places that agree:

- WinUAE's emulation of the board (`dataflyerscsiplus`: `gayle.cpp` isdataflyerscsiplus(),
  `scsi.cpp` NCR5380_DATAFLYER, base `$DA0000`, `$DD2000` with A4000-style IDE);
- the disassembly of `ExpXDS.device` (`C:\projects\dataflyer\ExpXDS.dis`).

## One port, two personalities

The same Gayle IDE address window serves either the IDE task file or the 53C80, never both.
A read of a "secondary" register switches it:

| Address (A1200) | Access | Function |
|---|---|---|
| `$DA1012` | read (word) | **select SCSI** - the 53C80 replaces the IDE registers (ExpXDS: `tst.w $da1012`) |
| `$DA1016` | read (word) | **select IDE** - back to the IDE task file (ExpXDS: `tst.w $da1016`) |
| `$DA1002` | read/write byte | 53C80 **pseudo-DMA data port** (WinUAE: "53C80 fake dma port") |
| `$DA100E` | read/write byte | interrupt latch byte (WinUAE secondary reg 3: reads back the written byte while the IDE IRQ is pending) |

(The decode: offset `& ~$2020`, `>> 2`; bit `$400` of that = the "secondary" set; the
low bits pick the register. `$1012 >> 2 = $404` -> secondary 4, `$1016 -> $405` -> 5,
`$1002 -> $400` -> 0, `$100E -> $403` -> 3. The odd byte of each word is used.)

## The 53C80 while SCSI is selected

Register n at `$DA0000 + n*4 + 2` (ExpXDS addresses them as `($2,A4)`..`($1e,A4)` with
A4 = `$DA0000`):

| n | Offset | Read | Write |
|---|---|---|---|
| 0 | `$02` | current SCSI data | output data |
| 1 | `$06` | initiator command | initiator command |
| 2 | `$0A` | mode | mode |
| 3 | `$0E` | target command | target command |
| 4 | `$12` | current bus status | select enable |
| 5 | `$16` | bus and status | start DMA send |
| 6 | `$1A` | input data | start DMA target receive |
| 7 | `$1E` | reset parity/interrupt | start DMA initiator receive |

ExpXDS's read loop is the classic 5380 pseudo-DMA one: target command = phase, wait for
REQ / phase match in the bus status registers, mode `= 2` (DMA mode), write `$1E` (start
initiator receive), then stream the data through `$DA1002`; optionally with interrupts off
(`$DFF09A = $4000` ... `$C000`) around the transfer.

## What that means for a driver

- While SCSI is selected, the internal IDE port is gone. A driver must switch to SCSI only
  when no IDE transfer is in flight on that port, and switch back after. The original
  solves that by grafting itself into `scsi.device` (SCSIPlus `SetFunction`s its first six
  vectors) - which is what hangs OS 3.2's boot when it runs early, and what the `PI0:`
  (a314fs) crash after DataFlyer I/O points at.
- The development A1200 boots from the TF1232's own IDE (`ehide.device`), but it does have a
  CompactFlash on the internal port too, mounted through `scsi.device` unit 0. In WinUAE, the
  first switch (to SCSI or back) kills the internal IDE interrupt for good, so
  `scsi.device` would hang. `dataflyer.device` therefore takes the port over before its
  first switch and drives the IDE side itself, by polling (next section).
- No autoconfig: the driver finds the board by probing (the original simply assumes it),
  and boot drives stay on IDE; SCSI partitions are mounted from S:User-Startup.

## WinUAE test setup (step 2)

`dataflyerscsiplus_rom_file=:ENABLED`, disks as `hardfile2=...,scsi<id>_dataflyerscsiplus`
(controller unit = SCSI id), A1200 / OS 3.2.3 machine.

## Pseudo-DMA: reads in 256-byte runs

The 53C80 raises DMA REQUEST (bus and status bit 6) when it holds a byte. A read of
`$DA1002` returns it and acknowledges it; when the next byte is not in yet, the card holds
the CPU on that read until it is. ExpXDS uses that: its read loop (`ExpXDS.dis`, `$376`
and the unrolled `$418`-`$4A0`) tests DMA REQUEST once, then reads 256 bytes blind
(4 x 64 `move.b (A2),(A0)+`), and repeats until the buffer is full, the phase changes
(interrupt bit 4) or the transfer is aborted. Writes are a byte at a time with a DMA
REQUEST test per byte (`$2C6`), plus one extra byte at the end to finish the last
handshake.

`dataflyer.device` does the same (`df_dmain` / `df_dmaout` in `dev/start.S`), with
interrupts off per 512 bytes. Testing DMA REQUEST before every byte, as the first version
did, halves the read speed.

## Swapped MaxTransfer / Mask in RDBs

The RDBs on the development machine's SCSI drives had `de_MaxTransfer` and `de_Mask` the
wrong way round (from an edit at some point, not from `DF-Prep`: its disassembly writes new
partitions with MaxTransfer `$7FFFFFFF` and Mask `$FFFFFFFE` - `$211C`, `$393C`, `$7050` - reads
and writes the two fields in the right order - `$19A6`, `$44B4` - and contains neither
`$0001FE00` nor `$FFFFFFFC`) (MaxTransfer `$FFFFFFFC`, Mask `$0001FE00`). With that Mask, FFS can never read into
the caller's buffer and reads one 512-byte block at a time (41,308 single-block reads for a
20 MB copy). `RDBMount` treats a Mask below `$00FFFFFE` as swapped, mounts with Mask
`$7FFFFFFE` and MaxTransfer `$1FE00`, and says so. It changes only what is mounted, not the
drive. `devlist` shows the values each mounted DOS device uses.

## Speed (A1200, TF1232 68030 @ 50 MHz, ZuluSCSI, 2026-10-21)

| | 20 MB copy SCSI partition -> RAM | raw read (`flytest SPEED`) |
|---|---|---|
| ExpXDS, sane mask | 610 KB/s | 820 KB/s |
| dataflyer.device, byte-wise DMA REQUEST test | 307 KB/s | 393 KB/s |
| dataflyer.device, 256-byte runs | 605 KB/s | 780 KB/s |
| either driver with the swapped mask | ~113-310 KB/s | |

Read only on the real drives; an `lha t` of 291 files on the SCSI partition checked out with the new
read loop. WinUAE (pattern disk, 11/11): 20 MB read 756 KB/s, write 481 KB/s.

## The IDE side

With the port switched to IDE, the Gayle task file is where it always is: data
`$DA2000` (word), registers n = 1-7 at `$DA2000 + n*4 + 2`, device control / alternate
status at `$DA301A` (`$DA1012` and `$DA1016` are the card's switch reads, so the driver
never touches the other `$DA1xxx` registers). A long read or write of the data register
moves two words. Every word arrives byte-swapped compared with how ATA numbers its
IDENTIFY data; sector data is left as it comes, as `scsi.device` does.

The takeover (`dev/ide.c`), on the first open of `dataflyer.device`:
1. open `scsi.device` units 0 and 1 (to know their unit structures);
2. under Forbid(), SetFunction its Open, Close, BeginIO and AbortIO;
3. one read per unit through the old BeginIO: what `scsi.device` had queued is done;
4. switch the port to IDE, nIEN on, IDENTIFY both positions.

The original `SCSIPlus` patches the same vectors but without step 3, which is what hung
OS 3.2 when it ran early in the boot.

ATAPI: after the packet, BSY can drop before DRQ comes up for the data (WinUAE shows a
status of 0 for a moment); the end of a command is the status phase, interrupt reason
I/O + C/D, not just BSY clear.

## Interrupts: none reach the CPU (measured 2026-10-22)

`flyirq` (DF_IRQPROBE in the driver) installs counting servers on INT2 (PORTS, where the
original `SCSIPlus` put its own handler) and INT6 (EXTER), switches the port to SCSI with
interrupts enabled, and runs a READ(10) with the 53C80 armed for a phase-mismatch interrupt
(DMA mode, a DMA send started, TCR left at COMMAND). On the real A1200 (TF1232):

- The chip's interrupt latch works: with monitor-busy set on a free bus, bus-and-status reads
  `$1C` (interrupt request, busy error, phase match).
- **No interrupt arrives on INT2 or INT6**, and Gayle's interrupt status (`$DA9000`) stays
  `$00` while the latch is set: the chip's IRQ pin is not connected to anything the CPU can
  see. Gayle's interrupt-enable register (`$DA9800`) reads `$00` and ignores writes while the
  port is switched.
- The phase-mismatch condition shows (phase match drops, DMA REQUEST stays up) but does not
  even set the latch on this chip in that setup.

So the driver polls; there is no interrupt-driven variant to build. What *can* be done is to
sleep between polls during the long waits (a target's command latency, a ZuluSCSI's write
commit) on a timer, so the CPU is free meanwhile; see the README's status.
