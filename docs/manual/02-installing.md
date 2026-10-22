# Installing

## With the installer

Double-click `Install_DataFlyerSCSI`. It asks what to do and then:

1. copies `dataflyer.device` to `DEVS:`;
2. copies `RDBMount` to `C:`;
3. puts the tools in a drawer of your choice (`SYS:Tools/DataFlyerSCSI` is
   the default);
4. looks at the SCSI port and lists the drives it finds;
5. adds two lines to the end of `S:User-Startup` that mount their partitions
   at boot, and turns the original software's lines (`SCSIPlus`, `DFMount`,
   `XDSFix`) into comments, if they are there.

Reboot afterwards. The old `ExpXDS.device`, `SCSIPlus` and `DFMount` stay
where they were, unused.

The installer starts in Intermediate mode, so it shows what it does and asks
about anything it is not sure of. Run it again later to update the driver, to
change the boot lines, or to remove it all.

## By hand

1. Copy `Devs/dataflyer.device` to `DEVS:`.
2. Copy `C/RDBMount` to `C:`.
3. Put these lines at the **end** of `S:User-Startup`:

   ```
   ; DataFlyerSCSI: mount the SCSI drives' partitions
   C:RDBMount >NIL: ALL
   ```

   `ALL` looks at every SCSI id and both built-in IDE positions and mounts every
   Rigid Disk Block it finds. Naming the units is faster at boot, since each
   empty id costs a quarter of a second:

   ```
   C:RDBMount >NIL: UNITS=3,4
   ```

4. Take the original software's lines out of `S:Startup-Sequence` and
   `S:User-Startup` (`SCSIPlus`, `DFMount`, `XDSFix`): put a `;` in front of
   each. They must not run any more: `SCSIPlus` patches `scsi.device` in a way
   that would collide with this driver.
5. Reboot.

The lines go at the end of `S:User-Startup` on purpose: the boot drive has
settled by then, and anything the system itself mounts is up.

## Where the partitions appear

Each partition comes up under the name in its Rigid Disk Block (`HD0:`,
`HD1:`, ... for drives set up with DF-Prep; whatever you chose in HDToolBox).
Workbench shows them by their volume names.

## A CD-ROM

A SCSI CD-ROM takes the system's own `CDFileSystem`. `Storage/DOSDrivers/CD0`
in the package is a mount file for it: copy it to `SYS:Storage/DOSDrivers`
(double-click it to mount) or to `DEVS:DOSDrivers` (mounted at every boot),
and set `Unit` in it to the drive's SCSI id (the file says `2`). A CD-ROM on the
built-in IDE port is unit `100` (master) or `101` (slave).

## Updating

Run the installer from the new package. It replaces `DEVS:dataflyer.device`
and `C:RDBMount` and leaves your boot lines alone. Reboot: the old driver
stays in memory until then.

## Removing

Run the installer and choose *Remove*. It deletes `DEVS:dataflyer.device`,
`C:RDBMount` and the tools drawer, takes its lines out of `S:User-Startup`
and, if you want, turns the original software's lines back on.
