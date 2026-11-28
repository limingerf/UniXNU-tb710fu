#!/usr/bin/env bash
# Package pl108: kernel carrying the noinline probe (patch 0053 v2).
#
# pl108 differs from pl107 only in the probe implementation:
#   noinline helper + string tags, so openat and fcntl survive compilation
#   (verified: OATN 0->1, FNCT 0->1, _tb710fu_dyld_probe_put 0->1)
#
# Boot args are identical to pl105, which reached GPU bring-up stage 9/9 and
# got past launchd, so the probe is the only new variable.
set -euo pipefail

SCRATCH=/mnt/c/Users/baiti/.gemini/antigravity/brain/e6e35059-f62d-4cac-979b-5de0846d0f50/scratch
ROOT=/mnt/d/unixnu-tb710fu-clean

cd "$ROOT"
python3 "$SCRATCH/package_pl16_data.py" \
  --run 46120267827 \
  --kernel "$ROOT/out/pl108/kernel.macho" \
  --tag pl108-probe \
  --payload-mib 86 \
  --mem-size 0x140000000 \
  --trustcache "$ROOT/out/pl86.trustcache" \
  --gpu-addon "$ROOT/out/pl86-combined.gpuaddon" \
  --live-checkpoints \
  --single-entry \
  --no-data-reset \
  --extra-args "adsp=2 battery=1 usb-sdp-ma=2000 gpu-metal=1 gpu-power=9 gpu-addon=1 fb-half=2"

echo "=== args ==="
grep '^args' "$ROOT/out/pl16-46120267827-pl108-probe-unsigned/unixnu.toml"
