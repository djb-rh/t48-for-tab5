#!/bin/sh
# Builds the release files into dist/:
#   t48-for-tab5-<ver>.bin        merged image (bootloader 0x2000, partitions
#                                 0x8000, app 0x10000), flashed at offset 0
#   t48-for-tab5-sdcard-<ver>.zip the burner/ folder for a FAT32 SD card
# and the web installer (site/: index.html, manifest.json, the .bin) that the
# gh-pages branch serves. Usage: tools/release.sh v1.0
set -e
ver=${1:?usage: tools/release.sh <version>}
cd "$(dirname "$0")/.."
~/.platformio/penv/bin/pio run -e app
rm -rf dist site && mkdir -p dist site
cp .pio/build/app/firmware.factory.bin "dist/t48-for-tab5-$ver.bin"
python3 tools/mkparts.py third_party/minipro sd/burner/db
python3 - "$ver" <<'PY'
import os, sys, zipfile
ver = sys.argv[1]
with zipfile.ZipFile(f"dist/t48-for-tab5-sdcard-{ver}.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for n in sorted(os.listdir("sd/burner/db")):
        z.write(f"sd/burner/db/{n}", f"burner/db/{n}")
    z.writestr("burner/images/README.txt",
               "ROM images go here. Reads from the T48 land here too.\n")
PY
cp "dist/t48-for-tab5-$ver.bin" "site/t48-for-tab5-$ver.bin"
sed -e "s/@VER@/$ver/g" web-installer/index.html > site/index.html
sed -e "s/@VER@/$ver/g" web-installer/manifest.json > site/manifest.json
cp docs/main.png site/main.png
ls -la dist site
