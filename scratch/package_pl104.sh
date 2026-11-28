#!/usr/bin/env bash
# Package pl104: pl103 plus ONLY text-fixup0 added back.
#
# Context: pl103 (live-checkpoints, no text-fixup0, no gpu-*, no tb710fu-gop)
# is the first image in this session whose boot reaches `exec: pid 1 launchd`.
# Its launchd then sits alone (thr=1 fp=182656, count=1) and never calls
# shared_region_map_and_slide_2_np (patch 0052 prints nothing).
#
# pl103 vs pl100 differ by many boot args, so this is a single-variable step:
# add back exactly one of them, text-fixup0, keeping everything else as pl103.
#
# text-fixup0 rewrites 4 bytes inside dyld_shared_cache_arm64e.03 at offset
# 0x5de1134. If the shipped .03 no longer has 08060037 there, the fixup could
# make dyld reject the cache and never reach map_and_slide.
set -euo pipefail

SCRATCH=/mnt/c/Users/baiti/.gemini/antigravity/brain/e6e35059-f62d-4cac-979b-5de0846d0f50/scratch
ROOT=/mnt/d/unixnu-tb710fu-clean

cd "$ROOT"
python3 "$SCRATCH/package_pl16_data.py" \
  --run 46058104010 \
  --kernel "$ROOT/out/pl100/kernel.macho" \
  --tag pl104-fixup \
  --payload-mib 86 \
  --mem-size 0x140000000 \
  --trustcache "$ROOT/out/pl86.trustcache" \
  --live-checkpoints \
  --single-entry \
  --no-data-reset \
  --extra-args "adsp=2 battery=1 usb-sdp-ma=2000 text-fixup0=dyld_shared_cache_arm64e.03,5de1134,08060037,1f2003d5"

echo "=== resulting args ==="
grep '^args' "$ROOT/out/pl16-46058104010-pl104-fixup-unsigned/unixnu.toml"
