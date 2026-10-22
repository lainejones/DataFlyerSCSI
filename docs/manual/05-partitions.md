# Partitions

## HDToolBox

The system's HDToolBox partitions the SCSI drives through this driver:

```
HDToolBox dataflyer.device
```

or, from Workbench, set the tooltype `SCSI_DEVICE_NAME=dataflyer.device` on
HDToolBox's icon. It finds every drive on the SCSI port (ids 0 to 6) and on the
built-in IDE port (units 100 and 101). HDToolBox also works through
`scsi.device` once the driver has run: there the SCSI drives are units 2 to 6.

The board's own `DF-Prep` is not needed.

## Speed

Measured on an A1200 with a TF1232 (68030 at 50 MHz), a ZuluSCSI as the SCSI
drive and a SanDisk CompactFlash on the built-in port, copying with the
Shell's `Copy`:

| | dataflyer.device | ExpXDS.device |
|---|---|---|
| SCSI, read a 20 MB file | 605 KB/s | 610 KB/s |
| SCSI, write a 20 MB file | 550 KB/s | 337 KB/s |
| SCSI, write 2354 small files (22.5 MB) | 144 KB/s | 122 KB/s |
| IDE (CompactFlash), read 2354 files | 703 KB/s | 639 KB/s (scsi.device) |
| IDE, write a 20 MB file | 1376 KB/s | 1050 KB/s |
| IDE, write 2354 files | 553 KB/s | 497 KB/s |

Raw reads (no file system) run at 780 KB/s on the SCSI port and 2.8 MB/s on
the IDE port. The SCSI side is at the limit of the card: every byte goes
through the CPU with a handshake of about 1.25 microseconds on the bus. Small
files are slow on both drivers because the ZuluSCSI takes its time to commit
each write to its SD card; the driver waits for it.
