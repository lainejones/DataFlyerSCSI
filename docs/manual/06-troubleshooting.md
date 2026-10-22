# Troubleshooting

**No partitions appear after the boot.** Open a Shell and run `C:RDBMount ALL`
without `>NIL:`: it says what it found on each unit. "no drive" on every unit
means the card did not answer: check that the driver is in `DEVS:`
(`Version DEVS:dataflyer.device`) and that the drive is on and terminated.
"no RDB on it" means the drive is there but has no partitions: HDToolBox.

**flytest says "No drive answers at that unit".** The SCSI id is not in use,
or the drive is still spinning up. Try again, or `flyprobe` to see which ids
answer.

**The old software's lines are still in the startup files.** `SCSIPlus` and
this driver cannot share the port. Comment the old lines out (the installer
does this) and reboot.

**A SCSI CD-ROM is not found by the CD-ROM file system.** Set `Unit` in the
mount file to the drive's SCSI id. A file system that only knows
`scsi.device` finds the drive there as unit id (2 to 6) once anything has
opened `dataflyer.device`, which `RDBMount` in `S:User-Startup` does at boot.

**Requesters from the file system right after an update.** The driver in
memory is the old one until a reboot. Reboot.

**Everything is slow.** Run `devlist` and look at the partition's MaxTransfer
and Mask; see [Partitions](05-partitions.md). `flystat` shows whether the
driver spends its time waiting for the drive ("select" and "REQ" polls).

**Something hangs with the drive light on.** The driver waits up to ten
seconds for a drive to answer each step of a command and then gives up with an
error, so a drive that stops answering shows up as errors, not a hang. If the
whole machine is frozen, note what ran last and tell us (see
[About](07-about.md)).

**A swapped CD-ROM is not noticed.** Only a file system that asked for
disk-change notification is told (the system's `CDFileSystem` does); the
driver looks every two seconds. `flychange UNIT=<n> EJECT` in the tools drawer
ejects the disc and reports whether the notification fired.

**What `flystat` counts.** "wait polls" are how many times a wait loop looked
at the chip before it was ready, by place: "select" (waiting for a drive to
answer its id), "REQ" (waiting for the next step of a command), "DMA in/out"
(waiting for the next byte of a transfer), "IDE" (waiting for the IDE drive).
Each poll is about 1.4 microseconds. "timeouts" should stay at 0.
