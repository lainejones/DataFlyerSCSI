#!/bin/sh
# mkadf.sh - the install floppy: dist/DataFlyerSCSI-<VER>.adf, an 880 KB (DD)
# FFS disk named DataFlyerSCSI with the install drawer's contents at its root
# (insert it, open it, double-click Install_DataFlyerSCSI). The whole package
# fits, the manual included. Run in WSL after dist.sh; needs amitools'
# xdftool (~/amitools-venv). GPL-3.0-only, see LICENSE.
set -e
cd "$(dirname "$0")/.."
X=${XDFTOOL:-$HOME/amitools-venv/bin/xdftool}
VER=$(sed -n 's/.*\$VER: dataflyer.device \([0-9.]*\).*/\1/p' dev/start.S | head -1)
D=dist/DataFlyerSCSI
ADF=$(pwd)/dist/DataFlyerSCSI-$VER.adf
[ -d "$D" ] || { echo "mkadf.sh: no $D - run dist.sh first" >&2; exit 1; }

S=$(mktemp -d)
cp -r "$D/." "$S/"
cp icons/Disk.info "$S/Disk.info"

rm -f "$ADF"
"$X" "$ADF" create + format DataFlyerSCSI ffs >/dev/null
cd "$S"
for d in $(find . -mindepth 1 -type d | sort); do
    "$X" "$ADF" makedir "${d#./}" >/dev/null
done
find . -type f | sort | while IFS= read -r f; do
    if ! "$X" "$ADF" write "$f" "${f#./}" >/dev/null 2>&1; then
        echo "mkadf.sh: $f does not fit on the floppy" >&2
        rm -f "$ADF"
        exit 1
    fi
done
cd - >/dev/null
rm -rf "$S"
"$X" "$ADF" info | sed -n 's/^\(used\|free\):/  &/p'
echo "mkadf.sh: $ADF"
