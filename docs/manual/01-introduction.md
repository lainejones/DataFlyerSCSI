# Introduction

The DataFlyer 1200 SCSI+ is a small board from Expansion Systems that clips
onto the A1200's internal IDE connector and adds a SCSI port. It came with its
own software: `ExpXDS.device`, `SCSIPlus` and `DFMount`, written for Kickstart
2 and 3.0. On AmigaOS 3.2 that software hangs the boot when it runs early, and
it was never fast.

DataFlyerSCSI is a new driver for the board, `dataflyer.device`, written from
the hardware up for AmigaOS 3.1 and newer. Nothing of the original software is
needed any more.

## What you get

- **dataflyer.device**: a disk driver for the SCSI port and for the A1200's
  built-in IDE port, which the board shares. Hard disks, CD-ROMs and anything
  else that speaks SCSI; the usual AmigaOS commands (TD64 and NSD for drives
  over 4 GB, `HD_SCSICMD` for HDToolBox and CD-ROM file systems).
- **Your IDE drive keeps working**, and gets a little faster: the driver takes
  the built-in port over from `scsi.device` and drives it too.
- **Speed**: reads at the speed of the original driver, writes faster than it,
  on a 68030 A1200 (see [Partitions](05-partitions.md#speed)).
- **RDBMount**, which mounts the partitions of your SCSI drives at boot; it
  replaces `DFMount`.
- **HDToolBox works**: partition your SCSI drives with the system's own tool,
  no need for the board's `DF-Prep`.
- **Tools** to test, measure and look at drives.
- A manual (this one) and an installer.

## What you need

- An A1200 with a DataFlyer 1200 SCSI+. The driver runs on the A1200's 68EC020
  and on any accelerator.
- AmigaOS 3.1 or newer (Kickstart 40). Written and tested on AmigaOS 3.2.3.
- A SCSI drive with a Rigid Disk Block (partitions made with HDToolBox or
  DF-Prep), or a CD-ROM with a CD-ROM file system.

## What it is not

The driver does not autoboot: the DataFlyer has no ROM, so the Amiga boots
from the built-in IDE drive (or an accelerator's) as before, and the SCSI
partitions appear a moment later, when `S:User-Startup` runs `RDBMount`.
