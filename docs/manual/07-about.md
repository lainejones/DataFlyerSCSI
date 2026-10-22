# About

DataFlyerSCSI is free software under the GNU General Public License, version 3
(`LICENSE` in the package). The source is on GitHub:
https://github.com/lainejones/DataFlyerSCSI. Bug reports and questions go
there as issues.

## How it was made

There is no documentation of the DataFlyer 1200 SCSI+ hardware. The driver
was written from two sources that agree: WinUAE's emulation of the board, and
a disassembly of the original `ExpXDS.device`. It was tested in WinUAE and on
a real A1200 with the board, a ZuluSCSI and a CompactFlash on the built-in
port.

## Credits

- Wayne E. Miller wrote the original `ExpXDS.device`; Expansion Systems made
  the board.
- Toni Wilen's WinUAE emulates the DataFlyer 1200 SCSI+, which made most of the
  development possible without the real card.
- The RDB mounting engine started as a replacement for `DFMount` and became
  `RDBMount`.
