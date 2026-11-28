#!/usr/bin/env bash
# Package pl109: the complete probe image.
#
# pl109 = pl108's recorder (noinline + string tags, verified present) PLUS the
# dump path in BootWatchdogPolicy.c so the ring can actually be read back.
#
# Offline verification on the kernel before packing (scratch/verify-pl109.py):
#   12/12 checks pass -- four recorder tags, noinline helper symbol, ring
#   globals, three dump format strings, and the two control markers for
#   patches 0043 and 0052.
#
# Boot args identical to pl105 (GPU stage 9/9, got past launchd).
set -euo pipefail

SCRATCH=/mnt/c/Users/baiti/.gemini/antigravity/brain/e6e35059-f62d-4cac-979b-5de0846d0f50/scratch
ROOT=/mnt/d/unixnu-tb710fu-clean

cd "$ROOT"
python3 "$SCRATCH/package_pl16_data.py" \
  --run 46121210251 \
  --kernel "$ROOT/out/pl109/kernel.macho" \
  --tag pl109-probe \
  --payload-mib 86 \
  --mem-size 0x140000000 \
  --trustcache "$ROOT/out/pl86.trustcache" \
  --gpu-addon "$ROOT/out/pl86-combined.gpuaddon" \
  --live-checkpoints \
  --single-entry \
  --no-data-reset \
  --extra-args "adsp=2 battery=1 usb-sdp-ma=2000 gpu-metal=1 gpu-power=9 gpu-addon=1 fb-half=2"

echo "=== args ==="
grep '^args' "$ROOT/out/pl16-46121210251-pl109-probe-unsigned/unixnu.toml"
