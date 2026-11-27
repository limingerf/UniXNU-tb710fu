#!/usr/bin/env bash
# Package pl102: a minimal "is the tablet still alive" recovery image.
#
# Purpose is diagnostic, not feature work. The tablet has not enumerated any
# USB device since pl100-srmap was flashed, so we cannot tell whether it is
# (a) sitting in the UEFI menu, or (b) dying before USB init in the kernel.
#
# This image keeps the kernel identical to pl100 (so the SRMAP trace is still
# present) but strips every optional subsystem from boot-args: no gpu-*, no
# adsp/battery, no text-fixup0, no tb710fu-keys. It is the smallest possible
# boot surface that still reports through COM5.
#
# If this does not enumerate COM5 either, the problem is not in the optional
# subsystems and a human has to check the screen.
set -euo pipefail

SCRATCH=/mnt/c/Users/baiti/.gemini/antigravity/brain/e6e35059-f62d-4cac-979b-5de0846d0f50/scratch
ROOT=/mnt/d/unixnu-tb710fu-clean

# Nothing optional. Kernel + storage + CDC only.
EXTRA=""

cd "$ROOT"
python3 "$SCRATCH/package_pl16_data.py" \
  --run 46058104010 \
  --kernel "$ROOT/out/pl100/kernel.macho" \
  --tag pl102-minimal \
  --payload-mib 86 \
  --mem-size 0x140000000 \
  --trustcache "$ROOT/out/pl86.trustcache" \
  --no-data-reset --no-live-log --single-entry \
  --extra-args "$EXTRA"
