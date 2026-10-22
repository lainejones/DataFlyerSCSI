# Tools

`RDBMount` is in `C:`; the rest are in the tools drawer the installer made
(`SYS:Tools/DataFlyerSCSI` unless you chose another). All of them are Shell
commands; `?` shows the arguments.

## RDBMount

```
RDBMount [DEVICE=<name>] UNITS=<n>[,<n>...] | ALL [FORCE] [QUIET]
```

Reads the Rigid Disk Block of each unit and mounts its partitions, as the
system does for a boot drive. `ALL` looks at SCSI ids 0 to 6 and the IDE units
100 and 101. A partition whose name is already mounted is skipped, as is one
flagged "no automount" unless `FORCE` is given. `DEVICE` defaults to
`dataflyer.device`, so the tool works for other drivers too.

## flytest

```
flytest UNIT=<n> [DEVICE=<name>] [PATTERN] [WRITE] [SPEED]
```

Checks a unit through the device interface: the NSD query, the geometry, an
INQUIRY, an illegal command (the drive must answer with sense data), and MODE
SENSE for the geometry pages. `SPEED` reads 2.5 MB from the start of the unit
and prints the speed; it writes nothing. `WRITE` writes 300 KB at block 2048,
reads it back and compares: only on a drive you can spare. `PATTERN` is for a
test disk that holds a known pattern.

`DEVICE=ExpXDS.device` runs the same checks on the original driver, for
comparison.

## flystat

```
flystat [UNIT=<n>] [OPTS=<n>] [DIAG]
```

Prints the driver's counters since the last time it was asked: requests by
size, how many times each wait loop had to wait, timeouts. `DIAG` shows what
the IDE takeover found. `OPTS=0` turns off the 256-byte write runs (a byte at a
time instead); `OPTS=1` turns them on again. They are on by default.

## flychange

```
flychange UNIT=<n> [DEVICE=<name>] [SECONDS=<n>] [EJECT] [LOAD]
```

Watches a removable unit's disk-change notification: it registers for it as a
file system would, optionally ejects (or loads) the medium first, and reports
each time the driver fires, with the change count and whether a medium is in.
`flychange UNIT=2 EJECT` on a CD-ROM at SCSI id 2 is the quickest way to see
the notification work.

## flyprobe

Finds the card and lists the SCSI ids that answer, without the driver. Run it
only when the driver is not in use.

## devlist

Every mounted DOS device, with the exec device and unit behind it and the
BufMemType, MaxTransfer and Mask it was mounted with.

## fcmp

```
fcmp <path1> <path2>
```

Compares two files, or two drawers with everything in them, byte for byte.

## flyread

```
flyread UNIT=<n> [DEVICE=<name>] [LBA=<n>] [BLOCKS=<n>] [BSIZE=<n>]
```

One `READ(10)` through `HD_SCSICMD` and one `CMD_READ` of the same blocks, with
what came back: for looking at one drive's answers.

## tick

```
tick SET
tick <label> BYTES=<n>
```

Times the steps of a script: `SET` starts the clock, the second form prints
the time since then and the speed.
