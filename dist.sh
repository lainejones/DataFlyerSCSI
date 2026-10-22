#!/bin/sh
# dist.sh - stage the release package in dist/DataFlyerSCSI (+ DataFlyerSCSI.info).
# Build first (make); the .lha is made on an Amiga from this tree, so the
# files get real protection bits. Needs python for the manual.
set -e
cd "$(dirname "$0")"
VER=$(sed -n 's/.*\$VER: dataflyer.device \([0-9.]*\).*/\1/p' dev/start.S | head -1)
[ -n "$VER" ] || { echo "dist.sh: no version in dev/start.S" >&2; exit 1; }
D=dist/DataFlyerSCSI

# refuse to package a binary older than its sources (a forgotten make)
for pair in "out/dataflyer.device:dev" "out/RDBMount:tools/rdbmount.c tools/rdbmountcore.h" \
            "out/flytest:tools/flytest.c" "out/flystat:tools/flystat.c" "out/flyprobe:tools/flyprobe.c" \
            "out/devlist:tools/devlist.c" "out/fcmp:tools/fcmp.c" \
            "out/flyread:tools/flyread.c" "out/tick:tools/tick.c" "out/flychange:tools/flychange.c" "out/flyirq:tools/flyirq.c"; do
    f=${pair%%:*}; srcs=${pair#*:}
    [ -f "$f" ] || { echo "dist.sh: missing $f - run make" >&2; exit 1; }
    if [ -n "$(find $srcs -newer "$f" \( -name '*.c' -o -name '*.h' -o -name '*.S' \) | head -1)" ]; then
        echo "dist.sh: $f is older than its sources - run make" >&2; exit 1
    fi
done

# only the staged drawer: notes and built .lha/.adf in dist/ stay
rm -rf dist/DataFlyerSCSI dist/DataFlyerSCSI.info
mkdir -p $D/Devs $D/C $D/Tools $D/Storage/DOSDrivers
cp out/dataflyer.device $D/Devs/
cp out/RDBMount         $D/C/
cp out/flytest out/flystat out/flyprobe out/devlist out/fcmp out/flyread out/tick out/flychange out/flyirq $D/Tools/
cp dist-src/UserStartup.rexx $D/Tools/
cp dist-src/CD0         $D/Storage/DOSDrivers/
sed "s/@VER@/$VER/" dist-src/Install_DataFlyerSCSI > $D/Install_DataFlyerSCSI
sed "s/@VER@/$VER/" dist-src/README > $D/README
cp LICENSE              $D/
DF_GUIDEVER="$VER ($(date +%d.%m.%Y))" python tools/mkguide.py docs/manual $D/DataFlyerSCSI.guide >/dev/null

# icons: the committed .info files, copied as they are
I=icons
cp $I/Install_DataFlyerSCSI.info $I/README.info $I/DataFlyerSCSI.guide.info $D/
cp $I/DataFlyerSCSI.info dist/DataFlyerSCSI.info
for dr in C Tools Storage Devs; do cp $I/$dr.info $D/$dr.info; done
cp $I/drawer.info $D/Storage/DOSDrivers.info

echo "DataFlyerSCSI $VER:"
find dist -type f | sort
