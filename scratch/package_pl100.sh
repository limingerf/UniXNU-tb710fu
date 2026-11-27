#!/usr/bin/env bash
# Package pl100: the reverted baseline plus patch 0052 (SRMAP trace).
set -euo pipefail

SCRATCH=/mnt/c/Users/baiti/.gemini/antigravity/brain/e6e35059-f62d-4cac-979b-5de0846d0f50/scratch
ROOT=/mnt/d/unixnu-tb710fu-clean

EXTRA="adsp=2 battery=1 usb-sdp-ma=2000 gpu-metal=1 gpu-power=9 gpu-addon=1 fb-half=2 text-fixup0=dyld_shared_cache_arm64e.03,5de1134,08060037,1f2003d5 tb710fu-keys=1"

cd "$ROOT"
python3 "$SCRATCH/package_pl16_data.py" \
  --run 46058104010 \
  --kernel "$ROOT/out/pl100/kernel.macho" \
  --tag pl100-srmap \
  --payload-mib 86 \
  --mem-size 0x140000000 \
  --trustcache "$ROOT/out/pl86.trustcache" \
  --gpu-addon "$ROOT/out/pl86-combined.gpuaddon" \
  --no-data-reset --no-live-log --single-entry \
  --extra-args "$EXTRA"
