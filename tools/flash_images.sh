#!/usr/bin/env bash
# Build the image filesystem and flash it to a badge's `storage` partition.
#
# Bitmaps live on their own LittleFS partition, NOT in the firmware. That means
# artwork is updated WITHOUT touching code, an OTA never re-sends a 150KB
# picture that has not changed, and the same picture is not duplicated across
# both A/B app slots.
#
#   ./tools/flash_images.sh /dev/cu.usbmodemXXXX
#
# Add or replace art by dropping .raw files into images/ and re-running:
#
#   python3 tools/interlace.py --left L.png --right R.png --name skull \
#           -o images/skull.raw
#   ./tools/flash_images.sh /dev/cu.usbmodemXXXX
#
# The badge scans the partition at boot, so a new file appears in the SHOW menu
# with no firmware change at all.
set -euo pipefail

PORT="${1:-}"
if [ -z "$PORT" ]; then
  echo "usage: $0 /dev/cu.usbmodemXXXX" >&2
  exit 1
fi

HERE="$(cd "$(dirname "$0")/.." && pwd)"
IMAGES="$HERE/images"
OUT="$HERE/.build/storage.bin"

# Must match partitions.csv exactly. A mismatch here does not fail loudly - it
# writes a filesystem the badge cannot mount, or overruns into coredump.
OFFSET=0x2F0000
SIZE=0x100000

MK="$(ls ~/Library/Arduino15/packages/esp32/tools/mklittlefs/*/mklittlefs | head -1)"
ET="$(ls ~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool | head -1)"

mkdir -p "$(dirname "$OUT")"

echo "packing $IMAGES -> $OUT"
"$MK" -c "$IMAGES" -b 4096 -p 256 -s "$SIZE" "$OUT"
"$MK" -l -b 4096 -p 256 -s "$SIZE" "$OUT"

BYTES=$(ls -l "$IMAGES"/*.raw 2>/dev/null | awk '{s+=$5} END {print s+0}')
echo "art: $BYTES bytes of $((SIZE)) partition"

echo "flashing to $OFFSET on $PORT"
"$ET" --port "$PORT" write-flash "$OFFSET" "$OUT" 2>/dev/null \
  || "$ET" --port "$PORT" write_flash "$OFFSET" "$OUT"

echo "done - reboot the badge and the new art appears in the SHOW menu"
