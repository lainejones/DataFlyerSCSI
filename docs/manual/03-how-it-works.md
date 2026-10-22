# How it works

## The card

The DataFlyer 1200 SCSI+ sits on the A1200's internal IDE connector. It has an
NCR 53C80 SCSI chip and a switch: the IDE connector's signals go either to the
A1200's own IDE drive or to the SCSI chip, never to both at once. Software
flips the switch by reading one of two addresses on the card. Both the IDE
drive and the SCSI chip are reached through the same addresses the A1200
uses for its IDE port; which of them answers depends on the switch.

The card has no ROM and no autoconfig: nothing finds it unless a driver knows
it is there, and nothing boots from it.

## What the original software did

`SCSIPlus` patched the system's `scsi.device` (the driver of the built-in
IDE port) so that every request to it went through `ExpXDS.device` instead,
which flipped the switch as needed. That is why it had to run at boot, and why
it hangs OS 3.2 when it runs early: the patch went in while the boot drive was
still busy. `DFMount` then mounted the SCSI partitions.

## What this driver does

`dataflyer.device` is a device of its own. Programs and file systems open it
like any other disk driver, and it talks to the SCSI chip directly, with the
switch set to SCSI only for the length of one command and set back to IDE
after. Nothing else runs on the Amiga during that command (it is inside
`Forbid()`), so no IDE access can land while the switch is the other way.

**The built-in IDE drive.** Flipping the switch has a side effect: the IDE
drive's interrupt no longer reaches the system, and `scsi.device`, which waits
for that interrupt, would wait forever. So the first time the driver is
opened it takes the IDE port over:

1. it opens `scsi.device`'s units 0 and 1 (master and slave) to learn which
   unit structures the system's own mounts use;
2. it points `scsi.device`'s entry points at its own code;
3. it sends one read to each unit the old way and waits for it, so anything
   `scsi.device` had queued is finished first (this is the step `SCSIPlus`
   skipped);
4. from then on it drives the IDE port itself, by polling, with the drive's
   interrupt turned off.

A drive mounted through `scsi.device` before that moment keeps working: its
requests now reach this driver. ATA disks get the SCSI commands a file system
uses translated into ATA ones; an ATAPI drive (a CD-ROM) takes them as they
are. Requests that arrive during the takeover wait for it.

**Data transfer.** The 53C80 has no DMA on this card; the CPU moves every byte
through a port on the card. The driver checks once that the chip has a byte
and then reads 256 bytes blind, because the card holds the CPU until each
next byte is in; that is what the original driver did, and it is twice as fast
as checking every byte. Writes work the same way. The checks that remain are
on the block boundaries, where a drive may end a transfer early. Interrupts
are off for each 512-byte block (about 0.6 ms), so a drive is never left
waiting in the middle of a handshake.

**Polling, with sleeps.** The card's SCSI chip has an interrupt output, but
on the DataFlyer it reaches no interrupt line of the Amiga (measured with
the `flyirq` tool), so the driver has no interrupt handler: a command runs in
the driver's own task, which waits for the drive by reading the chip's
status. It spins for the first millisecond of a wait and then sleeps on a
timer, a quarter of a millisecond at a time, so the CPU is free while a drive
looks for a block or commits a write, and nothing else on the Amiga is held
up during a command. The data itself moves through the CPU, as on every
53C80 board without a DMA controller: while a large transfer runs the CPU is
busy moving bytes, and that is the same with any driver for this card.

## Units

`dataflyer.device` unit numbers:

| Unit | Drive |
|---|---|
| 0 to 6 | SCSI id 0 to 6 (id 7 is the card itself) |
| id + 10 x LUN | a logical unit of a SCSI target, for example 12 = id 2, LUN 1 |
| 100 | the built-in IDE port's master |
| 101 | the built-in IDE port's slave |

After the takeover, `scsi.device`'s units mean the same: 0 and 1 are the
built-in IDE drives, 2 to 6 the SCSI ids 2 to 6 (as with `SCSIPlus`). A
program that only knows `scsi.device`, like an old CD-ROM file system, finds the
SCSI drives there.

## What it supports

`CMD_READ`, `CMD_WRITE`, `CMD_UPDATE` (flushes the drive's write cache),
`TD_GETGEOMETRY`, `TD_CHANGESTATE`, `TD_CHANGENUM`, `TD_EJECT`, `TD_PROTSTATUS`,
`TD_FORMAT`, the 64-bit commands (`TD_READ64` and the NSD ones) for drives over
4 GB, `NSCMD_DEVICEQUERY`, `HD_SCSICMD` with automatic sense, and disk-change
notification (`TD_ADDCHANGEINT` / `TD_REMCHANGEINT`): while a file system has
asked to be told, the driver asks a removable drive every two seconds whether
its medium is still there (a `TEST UNIT READY`), and fires the file system's
interrupt when a disc goes, comes, or the drive reports that it may have
changed. Workbench then shows the new disc by itself, as it does for a floppy.
Drives nobody has asked about are not polled.
