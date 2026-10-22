# DataFlyerSCSI

`dataflyer.device` is a new, open-source driver for the Expansion Systems **DataFlyer 1200
SCSI+** on AmigaOS 3.1 and newer. The DataFlyer is a small board that clips onto the A1200's
internal IDE connector and adds a SCSI port; the SCSI chip (an NCR 53C80) and the A1200's own
IDE drive share that connector through a switch on the board.

The driver replaces the board's original software: `ExpXDS.device`, `SCSIPlus` and `DFMount`
are not needed any more, and nothing of them is used. GPL-3.0-only.

## Why a new driver

The original software dates from the Kickstart 2 and 3.0 days and does two things that
cause trouble on a modern setup:

- **It patches `scsi.device`.** `SCSIPlus` overwrites the entry points of the system's own IDE
  driver so that every request goes through `ExpXDS.device`, which flips the board's switch
  as needed. On **AmigaOS 3.2.x** that patch goes in while the boot drive is still busy, and
  the machine **locks up during the boot**. Moving the lines to the end of `S:User-Startup`
  avoids the lock-up, but the patch is still there, and anything else that touches
  `scsi.device` afterwards is at its mercy.
- **It fights with other expansions on the same machine.** With an A314 (the Raspberry Pi
  board) in the clock port, mounting the Pi's `PI0:` file system *after* the DataFlyer has done
  any I/O crashes the Amiga; it only works the other way round. The patched `scsi.device`
  and the way the original driver holds the bus are the reason. With `dataflyer.device`
  the order no longer matters.

`dataflyer.device` is a device of its own: file systems and programs open it like any other
disk driver, no system component is patched, and the switch on the board is only ever flipped
for the length of one SCSI command, with nothing else running meanwhile.

## What it offers over the original software

- **Works on AmigaOS 3.1 to 3.2.3** with no boot lock-up; the SCSI partitions are mounted
  from one line at the end of `S:User-Startup`.
- **The built-in IDE drive keeps working, faster.** On its first open the driver takes the
  IDE port over from `scsi.device` cleanly (it lets `scsi.device` finish what it had queued
  first, which is the step the original skipped) and then drives both ports itself. ATA disks
  and ATAPI CD-ROMs on the IDE port, as `scsi.device` units 0/1 or `dataflyer.device` units
  100/101. A CompactFlash on the port copies about 10 % faster than through the ROM's
  `scsi.device`.
- **Speed.** Reads at the original driver's speed; writes 60 % faster on the SCSI port
  (550 against 337 KB/s for a 20 MB file on a 68030 A1200 with a ZuluSCSI). The data moves
  in 256-byte runs per handshake check, as the original did for reads, now for writes too.
- **HDToolBox** partitions the drives (`HDToolBox dataflyer.device`); no `DF-Prep`.
- **Drives over 4 GB:** TD64 and NSD 64-bit commands.
- **`HD_SCSICMD` with automatic sense data**, for HDToolBox, CD-ROM file systems and SCSI
  tools; SCSI logical units (unit = id + 10 x LUN).
- **CD-ROMs** on the SCSI port or the IDE port with the system's `CDFileSystem` (a mount
  file is in the package), with **disk-change notification** so Workbench shows a new disc by
  itself.
- **Coexists with the A314** and anything else on the machine: no patches, no hooks.
- **RDBMount** mounts the partitions of one, several or all units at boot.
- **An installer** (install, update, remove) that edits the startup files for you, turns the
  original software's lines into comments and scans the SCSI port;
  **an AmigaGuide manual**; test and diagnostic tools.
- Small: the driver is 15 KB; it runs on the A1200's 68EC020 and on any accelerator.

Two things the board itself rules out: **interrupts** (the SCSI chip's interrupt pin reaches
no CPU interrupt line; measured, see `docs/hardware.md`), so the driver polls, but it sleeps
on a timer while a drive thinks or commits a write, and nothing is `Forbid()`den during a
command, so the rest of the system keeps running; and **autoboot** (no ROM on the board).
Data still moves through the CPU, as on any 53C80 board without DMA: during a large
transfer the CPU is busy moving bytes, whatever the driver does.

## Status

1.0. In daily use on the development A1200 (TF1232 68030, a ZuluSCSI as hard disks and
CD-ROM, a CompactFlash on the built-in IDE port), installed by the package's installer.
Tested in WinUAE as well, which emulates the board. Known: one stall of file-system reads after
a very large write was seen once during testing and could not be reproduced in repeated runs;
the driver's counters showed no timeouts. Please report anything like it as an issue.

The hardware, as far as it is known, and what the original driver does are described in
[docs/hardware.md](docs/hardware.md); the user manual is in [docs/manual](docs/manual/README.md)
(it becomes `DataFlyerSCSI.guide` in the package).

## Building

In WSL with amiga-gcc (Bebbo's toolchain): `make` builds `out/dataflyer.device` (68020+) and
every tool; `make DEBUG=1` logs every request to the serial port. `sh dist.sh` (Git Bash, needs
python for the manual) stages the package in `dist/DataFlyerSCSI`; the `.lha` is made on an
Amiga so the files get real protection bits.

## Tools

| Tool | What it does |
|---|---|
| `RDBMount [DEVICE=x] UNITS=n,n / ALL [FORCE] [QUIET]` | Mounts the RDB partitions of the units (ALL = SCSI ids 0-6 + IDE 100/101); leaves the units found in `ENV:DataFlyerSCSI/units` for the installer. |
| `flytest UNIT=n [DEVICE=x] [PATTERN] [WRITE] [SPEED]` | Device tests: NSD, geometry, INQUIRY, sense data, MODE SENSE, a pattern disk check, a scratch write and read-back, read speed. `DEVICE=ExpXDS.device` runs them on the original. |
| `flystat [UNIT=n] [OPTS=n] [DIAG]` | The driver's counters since the last call; `DIAG` shows the IDE takeover; `OPTS=0` turns the 256-byte write runs off. |
| `flychange UNIT=n [SECONDS=n] [EJECT] [LOAD]` | Watches a removable unit's disk-change notification; can eject or load the medium first. |
| `flyprobe` | Finds the 53C80 and lists the SCSI ids that answer, by programmed I/O. |
| `flyirq UNIT=n [TIMES=n]` | Does the card's SCSI interrupt reach a CPU interrupt line? (On the boards measured: no.) |
| `devlist` | Every mounted DOS device with its exec device, unit, BufMemType, MaxTransfer and Mask. |
| `fcmp <path1> <path2>` | Compares two files, or two drawers with everything in them. |
| `flyread UNIT=n [DEVICE=x] [LBA=n] [BLOCKS=n] [BSIZE=n]` | One READ(10) and one CMD_READ, with what came back. |
| `tick SET` / `tick label BYTES=n` | Times script steps and prints KB/s. |

## Testing

- WinUAE emulates the card (`dataflyerscsiplus`). Copies of the test configurations are in
  `docs/`: "DataFlyer test" (SCSI only), "DataFlyer test IDE" (plus an IDE disk) and
  "DataFlyer test CD" (plus an IDE CD-ROM); each runs NetHarness so the tests, the installer
  and HDToolBox can be driven from the host.
- On real hardware the tests write only into a scratch drawer of an existing partition and
  compare every file afterwards.

## The original software

The board shipped with `ExpXDS.device` (Wayne E. Miller), `SCSIPlus`, `DFMount` and the
partitioner `DF-Prep` on an "AutoInstall" floppy. There are no sources; this driver was written
from WinUAE's emulation of the board and a disassembly of `ExpXDS.device`, which agree on every
register.
