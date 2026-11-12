#!/bin/sh
# Builds an SD card image that boots straight into DSPi on a Raspberry Pi Zero 2 W: one FAT32 partition
# holding the GPU boot files (raspberrypi/firmware at a pinned tag, with Broadcom's licence for them),
# boot/config_card.txt, pizero_z4.img as kernel.img and the USB loader as loader.img, for updates over USB.
# macOS only (hdiutil and diskutil); no root needed.
# Usage: make_sd_image.sh [build-dir] [name]   (writes <build-dir>/<name>.img and <name>.img.zip)
set -eu

TAG=1.20260915
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$HERE/build-zero2}
NAME=${2:-dspi-zero2w}
CACHE=$BUILD/boot-firmware/$TAG
OUT=$BUILD/$NAME.img

for f in pizero_z4.img pizero_loader.img; do
    [ -s "$BUILD/$f" ] || { echo "no $BUILD/$f: build pizero_z4 and pizero_loader first" >&2; exit 1; }
done
mkdir -p "$CACHE"
for f in bootcode.bin start.elf fixup.dat LICENCE.broadcom; do
    if [ ! -s "$CACHE/$f" ]; then
        echo "fetching $f ($TAG)"
        curl -fsSL -o "$CACHE/$f.part" "https://raw.githubusercontent.com/raspberrypi/firmware/$TAG/boot/$f"
        mv "$CACHE/$f.part" "$CACHE/$f"
    fi
done

rm -f "$OUT" "$OUT.zip"
dd if=/dev/zero of="$OUT" bs=1m count=64 2>/dev/null
DEV=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$OUT" | awk 'NR==1 {print $1}')
trap 'hdiutil detach "$DEV" >/dev/null 2>&1 || true' EXIT
diskutil partitionDisk "$DEV" MBR "MS-DOS FAT32" DSPI 100% >/dev/null
MNT=$(diskutil info "${DEV}s1" | awk -F': *' '/Mount Point/ {print $2}')
[ -n "$MNT" ] || { echo "the new partition did not mount" >&2; exit 1; }

# -X and the find below keep macOS's ._ metadata files off the card.
cp -X "$CACHE/bootcode.bin" "$CACHE/start.elf" "$CACHE/fixup.dat" "$CACHE/LICENCE.broadcom" "$MNT/"
cp -X "$HERE/boot/config_card.txt" "$MNT/config.txt"
cp -X "$BUILD/pizero_z4.img" "$MNT/kernel.img"
cp -X "$BUILD/pizero_loader.img" "$MNT/loader.img"
cat > "$MNT/README.txt" <<EOF
DSPi for the Raspberry Pi Zero 2 W (bare metal), built from DSPi-Zero $(git -C "$HERE" describe --always --dirty).
Plug the Zero's USB port (not PWR) into a computer: it appears as the "Weeb Labs DSPi Zero" USB audio device
and works with DSPi Console. Outputs, inputs and pins: https://github.com/WeebLabs/DSPi-Zero
Updates over USB: tools/pizero_load.py in that repository sends a new image, which runs until the power is
removed. To keep it, copy it onto this card as kernel.img.
The GPU boot files are from https://github.com/raspberrypi/firmware ($TAG), under LICENCE.broadcom.
EOF
rm -rf "$MNT/.fseventsd" "$MNT/.Spotlight-V100" "$MNT/.Trashes"
find "$MNT" -name "._*" -delete
diskutil unmount "${DEV}s1" >/dev/null
hdiutil detach "$DEV" >/dev/null
trap - EXIT

(cd "$BUILD" && zip -q -9 "$NAME.img.zip" "$NAME.img")
ls -l "$OUT" "$OUT.zip"
