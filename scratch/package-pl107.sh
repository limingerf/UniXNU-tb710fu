#!/usr/bin/env bash
# Package pl107: the kernel that carries patch 0053 (dyld cache probe).
#
# Uses the exact boot-args that got pl105 past launchd and had GPU bring-up
# reach stage 9/9, so the only new variable is the probe itself:
#   live-checkpoints, no text-fixup0, GPU group enabled, no tb710fu-gop
set -euo pipefail

SCRATCH=/mnt/c/Users/baiti/.gemini/antigravity/brain/e6e35059-f62d-4cac-979b-5de0846d0f50/scratch
ROOT=/mnt/d/unixnu-tb710fu-clean

cd "$ROOT"
python3 "$SCRATCH/package_pl16_data.py" \
  --run 46118781364 \
  --kernel "$ROOT/out/pl107/kernel.macho" \
  --tag pl107-probe \
  --payload-mib 86 \
  --mem-size 0x140000000 \
  --trustcache "$ROOT/out/pl86.trustcache" \
  --gpu-addon "$ROOT/out/pl86-combined.gpuaddon" \
  --live-checkpoints \
  --single-entry \
  --no-data-reset \
  --extra-args "adsp=2 battery=1 usb-sdp-ma=2000 gpu-metal=1 gpu-power=9 gpu-addon=1 fb-half=2"

echo "=== args ==="
grep '^args' "$ROOT/out/pl16-46118781364-pl107-probe-unsigned/unixnu.toml"
