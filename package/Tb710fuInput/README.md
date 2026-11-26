# Tb710fuInput

Novatek NT36532 touchscreen over QUPv3 GENI SPI4 and the TB710FU physical keys
(PON power, PON RESIN volume-down, PM8550 GPIO6 volume-up) for macOS 27.3
(`26C5035r`, arm64e).

Read `docs/INPUT_TOUCH_KEYS.md` first: it carries the DTS provenance table, the
boot-argument stages, the host evidence and the merge wiring. This file is only
a map of the directory.

## Layout

- `Tb710fuInputHardware.h` — every hardware fact, each with the DTS node it
  came from. Nothing else in this directory may hardcode an address.
- `Tb710fuInputConfig.{h,c}` — `tb710fu-touch=0..3`, `tb710fu-keys=0..1`, and the
  diagnosis switches (`-fuse`, `-frames`, `-verbose`). Defaults are off.
- `Tb710fuMem.h` — aligned 32/64-bit MMIO accessors. A mapping is never touched
  any other way.
- `Tb710fuGeni.{h,c}` — SE identity, init, SPI setup, divider, polled FIFO
  transfer, cancel/abort recovery.
- `Tb710fuNvt.{h,c}` — NT36532 wire protocol, non-destructive identify, frame
  read and 60x40 decode.
- `Tb710fuNvtFirmware.{h,c}` — stock firmware image parse and vendor CRC-32.
  Validation only; this port never uploads firmware.
- `Tb710fuTracker.{h,c}` — stable contact ids, radius gate, clamping.
- `Tb710fuHid.{h,c}` — descriptors, report packing, report-bit accounting.
- `Tb710fuSpmi.{h,c}` — SPMI arbiter v7 observer reads for the PMIC keys.
- `Tb710fuKeys.{h,c}` — key sampling, two-sample debounce, HID bit mapping.
- `Tb710fuInputHidSlots.h` — generated IOHIDDevice vtable table; regenerate with
  `scripts/build-tb710fu-input.py --emit-hid-slots`.
- `Tb710fuInputProbe.cpp` — the only IOKit translation unit: windows, threads,
  vtable adoption, HID creation.
- `Tb710fuInput.h` — `tb710fu_input_start()`, `tb710fu_input_stop()`,
  `tb710fu_input_firmware_check()`.
- `host/Tb710fuInputHostTests.c` — the host test body, driven by
  `scripts/check-tb710fu-input-host.py`.

## Rules that are easy to break

- No `memcpy`/`memset`/`bzero` on a mapping, and no `volatile` arithmetic
  (`*reg = *reg + 1`, never `*reg += 1` / `++*reg`).
- No Apple private kext symbols: the device is created by the live metaclass and
  the vtable is adopted (see `docs/INPUT_TOUCH_KEYS.md`).
- No raw-address function pointer without
  `ptrauth_sign_unauthenticated(..., ptrauth_key_function_pointer, ...)`.
- No writes to GCC branch registers or TLMM: neither is pinned by a DTS for
  this path.
