# UEFI framework source patches

Baseline: UniXNU/uefi-sheng commit
66421cb7bb5694843aec8de1ca3d1de4c20cca38.

Apply 0001-simplefb-validate-region-and-propagate-gop-errors.patch to a clean
checkout of that base before overlaying/compiling the TB710FU platform.
This is a source patch for uefi-sheng, not a firmware.patches entry: the
UniXNU firmware.patches mechanism applies files to Mu_Basecore instead.
Do not edit fetched caches or silently omit a patch that no longer applies.

The patch changes only SimpleFb.c:
- initialize the memory descriptor and honor the memory lookup status;
- use Display_Reserved only when Display Reserved is not found;
- reject absent/empty framebuffer regions and packed-pixel size/range overflow;
- compute the advertised size in UINTN rather than overflowing UINT32 first;
- return the real GOP installation failure and safely release allocated buffers,
  including when a prior allocation left Mode unset.

Existing pixel formats and stride assumptions are retained. In particular,
32-bit pixels remain BGRA and PixelsPerScanLine remains the configured width.
The actual TB710FU ABL pixel format, scanout address, row stride, orientation,
and ownership/reservation still need same-device handoff evidence. This patch
neither selects a new layout nor proves that ABL scanout survives UEFI entry.
Missing framebuffer data must fail explicitly; no zero framebuffer or external
board RAW/binary is substituted. Full hardware/UEFI build qualification is pending.

Offline check evidence is in .local/uefi-simplefb-patch-check.json and the
UEFI cloud plan is .local/uefi-cloud-audit.md (both local-only reports).
