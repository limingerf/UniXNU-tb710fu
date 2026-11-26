# TB710FU touchscreen and physical keys (macOS 27.3)

Target: Lenovo TB710FU (SM8650Q-AB), macOS 27.3 `26C5035r`, XNU `12377.121.6`,
arm64e. Subsystem: Novatek NT36532 digitizer on QUPv3 GENI SPI4, plus the PON
power key, the PON RESIN volume-down key and the PM8550 GPIO6 volume-up key.

This document records what the port is, which pinned file each hardware fact
comes from, how it is switched on in stages, what the host tests actually
prove, and what still has to be wired in and measured. **Nothing in this
subsystem has run on hardware.** The archive has not even been compiled here:
the build recipe needs the supported macOS runner with `xcrun` and the minimal
XNU build tree. Everything below that is claimed as passing is a host claim.

## Deliverables

| Path | Role |
| --- | --- |
| `package/Tb710fuInput/Tb710fuInputHardware.h` | every DTS fact, with the node it came from |
| `package/Tb710fuInput/Tb710fuInputConfig.{h,c}` | boot-argument stages and parsing |
| `package/Tb710fuInput/Tb710fuMem.h` | aligned 32/64-bit MMIO helpers, the only way a mapping is touched |
| `package/Tb710fuInput/Tb710fuGeni.{h,c}` | GENI SE identity, init, SPI setup, divider, polled FIFO transfer, recovery |
| `package/Tb710fuInput/Tb710fuNvt.{h,c}` | NT36532 page/read/write, non-destructive identify, frame read, 60x40 decode |
| `package/Tb710fuInput/Tb710fuNvtFirmware.{h,c}` | stock firmware image parse and vendor CRC-32 |
| `package/Tb710fuInput/Tb710fuTracker.{h,c}` | stable contact ids, radius gate, clamping |
| `package/Tb710fuInput/Tb710fuHid.{h,c}` | touch and key descriptors, report packing, report-bit interpreter |
| `package/Tb710fuInput/Tb710fuSpmi.{h,c}` | SPMI arbiter v7 observer reads |
| `package/Tb710fuInput/Tb710fuKeys.{h,c}` | PON/GPIO sampling, two-sample debounce, HID mapping |
| `package/Tb710fuInput/Tb710fuInputHidSlots.h` | generated IOHIDDevice slot table (334 slots) |
| `package/Tb710fuInput/Tb710fuInputProbe.cpp` | the single IOKit translation unit (mapping, threads, vtable adoption) |
| `package/Tb710fuInput/Tb710fuInput.h` | the three exported entry points |
| `scripts/build-tb710fu-input.py` | archive build (C record + IOKit C++ record) and `--emit-hid-slots` |
| `scripts/check-tb710fu-input-host.py` | host compile/run, slot re-proof, firmware check |

## Hardware provenance

Every number below is copied from one of four pinned board files
(`sm8650-lenovo-tb710fu.dts`, `sm8650.dtsi`, `pm8550.dtsi`, `pmk8550.dtsi`).
Nothing is taken from the SM8550/sheng reference port. The same facts, with
the same citations, are in `Tb710fuInputHardware.h`; that header is the copy the
code actually uses.

| Fact | Value | Exact source node |
| --- | --- | --- |
| SPI4 SE window | `0x00A90000` + `0x4000` | `sm8650.dtsi` `spi4: spi@a90000` `reg` |
| SPI4 GIC interrupt | `GIC_SPI 357`, level high | same node `interrupts` |
| SPI4 clock | `GCC_QUPV3_WRAP1_S4_CLK` | same node `clocks` |
| SPI4 DMA | `gpi_dma1` 0/1, channel 4, `QCOM_GPI_SPI` | same node `dmas` |
| SPI4 SMMU | none declared (no `iommus`) | same node |
| Wrapper window | `0x00AC0000` + `0x2000` | `sm8650.dtsi` `qupv3_id_0: geniqup@ac0000` `reg` |
| SPI pins | MISO 48 / MOSI 49 / CLK 50 / CS 51, function `qup1_se4` | `sm8650.dtsi` `qup_spi4_data_clk`, `qup_spi4_cs` |
| Touch node | `compatible = "novatek,NVT-ts-spi"`, `reg = <0>`, `spi-max-frequency = <9600000>` | `sm8650-lenovo-tb710fu.dts` `touchscreen@0` |
| Touch IRQ GPIO | `&tlmm 162 0x2001` (ACTIVE_LOW \| PULL_UP) | same node `irq-gpios` |
| Touch firmware name | `novatek/novatek_nt36532_fw.bin` | same node `firmware-name` |
| Touch reset / rails | not declared | same node (absent) |
| Panel | `compatible = "lenovo,nt36532-wqhd"`, `reset-gpios = <&tlmm 133 0>` | board DTS `panel@0` |
| Panel extent | 3200 x 2000 | `docs/HARDWARE.md` (declared mode; not a DTS cell) |
| GCC window | `0x00100000` + `0x1F4200` | `sm8650.dtsi` `clock-controller@100000` |
| TLMM window | `0x0F100000` + `0x300000`, 211 GPIOs | `sm8650.dtsi` `pinctrl@f100000` |
| SPMI core | `0x0C400000` + `0x3000` | `sm8650.dtsi` `spmi@c400000` `reg[0]` |
| SPMI observer | `0x0C440000` + `0x80000` | same node `reg[2]` |
| SPMI config (ownership) | `0x0C42D000` + `0x4000` | same node `reg[4]` |
| PON peripheral | SID 0, PID `0x13`, `reg = <0x1300>, <0x800>` | `pmk8550.dtsi` `pon@1300` |
| Power key IRQ | `<&pmk8550 0x13 7 EDGE_BOTH>` | `pmk8550.dtsi` `pon_pwrkey` |
| Volume-down (RESIN) | `<&pmk8550 0x13 6 EDGE_BOTH>`, board sets `linux,code = KEY_VOLUMEDOWN` | `pmk8550.dtsi` `pon_resin` + board `&pon_resin` |
| Volume-up GPIO | `&pm8550_gpios 6`, `GPIO_ACTIVE_LOW`, `debounce-interval = <15>` | board DTS `gpio-keys/key-volume-up` |
| PM8550 GPIO block | SID 1, `gpio@8800`, 13 GPIOs | `pm8550.dtsi` `gpio@8800` |
| PON status bits | `RT_STS` at base + `0x10`, gen3 KPDPWR bit 7, RESIN bit 6 | `pm8941-pwrkey.c` |
| PMIC GPIO block | one `0x100` block per pin, `RT_STS` at + `0x10`, bit 0 active-low | `pinctrl-spmi-gpio.c`; GPIO6 => PID `0x88 + 5 = 0x8D` |

Consequences that follow directly from the table and are encoded in the code:

- The touch node has no `reset-gpios` and no supply rails, so this driver
  drives neither. Reset is owned by the panel/PMIC side (`panel@0` names
  TLMM 133, which this subsystem does not touch).
- `spi4` declares no `iommus`, so no SMMU stream mapping is attempted and no
  DMA is used. Touch traffic is polled FIFO only.
- GCC register offsets for the S4 branch are not described by any pinned file,
  so none are written: the SE must arrive already clocked by the firmware that
  handed it over, and stage 2 only programs the SE-internal divider from the
  19.2 MHz TCXO to 9.6 MHz.
- TLMM is not mapped at all: pins 48..51 are already in `qup1_se4` and no pin
  needs reprogramming for a polled SPI transfer.

## Boot arguments and bring-up stages

Off by default. Every level is a superset of the previous one and is safe to
stop at, so a failed attempt never leaves a half-initialised controller that a
later boot has to recover from.

| Argument | Meaning | What it does |
| --- | --- | --- |
| `tb710fu-touch=0` | default | nothing mapped, nothing written |
| `tb710fu-touch=1` | snapshot | maps SE4 + wrapper, reads identity registers only; zero writes |
| `tb710fu-touch=2` | engine | + GENI init, SPI mode/CS, divider; must reach IDLE; no controller traffic |
| `tb710fu-touch=3` | full | + non-destructive identify, frame loop, tracking, HID publication |
| `tb710fu-keys=0` | default | off |
| `tb710fu-keys=1` | keys | poll PON + PM8550 GPIO6 through the SPMI observer, publish Consumer-page keys |
| `tb710fu-touch-fuse=1` | diagnosis | allow the single read of `SE_GENI_IF_DISABLE` (`0x64`); the sibling hardware is documented to abort on this register |
| `tb710fu-touch-irq=1` | diagnosis | service GENI completion from the GIC instead of polling (partially implemented, see open items) |
| `tb710fu-input-verbose=1` | diagnosis | per-frame log line |
| `tb710fu-input-frames=N` | diagnosis | stop the frame loop after N frames (0 = unlimited) |

Suggested order on hardware, one boot argument change at a time:

1. `tb710fu-touch=1` — expect one line with `geni-fw`, `proto`, `qup-hw`,
   FIFO depth/width and `status`. A window that reads all-zero or all-ones means
   the SE is not clocked or not GENI; the stage refuses and writes nothing.
2. `tb710fu-touch=2` — expect `engine idle, spi=9600000 Hz`. A refusal here is
   a divider/protocol problem, still with no controller traffic.
3. `tb710fu-touch=3 tb710fu-input-frames=8` — bounded first contact with the
   controller: chip id line, then eight frames, then the loop stops itself.
4. `tb710fu-touch=3` — unbounded loop, HID devices published.
5. `tb710fu-keys=1` — separate boot; expect the two APIDs and then key reports.

## What the port changes relative to the early work

The early work is `doubao/w4-input` (`package/Tb710fuInput/` in
`d:\unixnu-tb710fu-w4`, commits `3e1bf36`, `33539ce`). It had stage-2 GENI
transport, an NT36532 core, HID and keys, but four behaviours were unsafe or
belonged to another board:

1. **Unconditional fuse read.** `tb710fu_geni_read_info` always read
   `SE_GENI_IF_DISABLE`. The reference implementation records that this register
   aborts on the v53..v55 SEs. It is now behind `tb710fu-touch-fuse=1`, and FIFO
   availability is judged by whether the first transfer completes.
2. **Destructive identify.** Identify always reset the engine (`0x5A` at
   `0x7FFF80`) and the bootloader (`0x69` at `SWRST_SIF`) before reading the chip
   id, which kills a controller the firmware already trained. Identify is now
   non-destructive by default; `reset_first` exists only for a future firmware
   reload path, and this port has no upload path at all.
3. **Borrowed geometry.** The old node table was the 0.1 mm physical table from
   the sheng board. Node centres are now computed from the declared 3200x2000
   extent (60 columns, 40 rows, `COORD_UNITS = 16` fixed point). The absolute
   sensor pitch is not in any pinned file, so it stays a calibration item.
4. **No tracking layer.** A contact id used to be whatever the frame said. A
   separate tracker now assigns stable ids (greedy nearest neighbour, radius
   gate `max_y / 10 = 199`, `max_missing = 3`, jitter 2) and clamps to the HID
   logical maximum, so a lifted and re-placed finger does not teleport.

## Publication without Apple link symbols

`IOHIDDevice` lives in a prelinked Apple kext. Naming it in C++ would create
undefined symbols this fileset does not export (the same problem
`package/Tb710fuEthernet.cpp` solves for `IOEthernetController`). The port uses
the live class instead of a declaration:

1. `OSMetaClass::allocClassWithName("IOHIDDevice")` returns an instance built by
   the shipped class (its own constructor and field initialisers). The instance
   extent must be `0xD0`; a mismatch refuses the device.
2. The live vtable is read from the object itself, stripped with
   `ptrauth_key_cxx_vtable_pointer`, and every one of its 334 entries is
   stripped with `ptrauth_key_function_pointer` and required to land in kernel
   text. Any poisoned entry refuses the device instead of calling through it.
3. Every entry is re-signed for its new slot address:
   `ptrauth_sign_unauthenticated(raw, ptrauth_key_function_pointer, blend(&table[i], diversity[i]))`.
4. Exactly one slot is replaced: 284 `newReportDescriptor`, which selects the
   touch or key descriptor by comparing `this` with the two published devices.
   `start`/`stop` are not overridden, so the family's own bring-up runs.
5. The vptr is signed with `ptrauth_key_cxx_vtable_pointer` and the
   `OSMetaClassBase` diversity `0xcda1` blended with the object address, the
   convention `Tb710fuEthernet.cpp` uses.
6. Reports go out through slot 285 `handleReport`, re-authenticated with that
   slot's own diversity before the call.

The table in `Tb710fuInputHidSlots.h` is generated from this board's prelinked
kernel, not guessed:

| Item | Value |
| --- | --- |
| source image | `.local/prelink-kernel.macho`, sha256 `63239c4e1ba02cfb87bde6094f334c363d7727151d548b344304572f2a06a227` |
| `__ZTV11IOHIDDevice` | VA `0xfffffff0085ef6d0`, address point `+0x10` |
| slot count | 334, instance size `0xD0` |
| named slots | 7 `getMetaClass` `0xfffc`, 190 `IOService::start` `0x3c68`, 191 `IOService::stop` `0xb085`, 284 `newReportDescriptor` `0x3f76`, 285 `handleReport` `0xd548` |

Diversities are clang's `ptrauth_string_discriminator` (SipHash-2-4, fixed key,
mod `0xffff + 1`) over the mangled method name. An override method keeps the
*base* class's mangling, which is why `start`/`stop` hash as `IOService` while
284/285 hash as `IOHIDDevice`. Both the generator and the host check assert
this, so a table read from a different OS build fails instead of publishing
garbage.

## Memory rules this subsystem follows

- MMIO is mapped with `IODeviceMemory::withRange(phys, len)` and
  `map(kIOMapAnywhere | kIOMapInhibitCache)` and is touched **only** through
  `tb710fu_mmio_read32/write32/read64/write64` in `Tb710fuMem.h`. No
  `memcpy`/`memset`/`bzero` ever lands on a mapping: unaligned access on SM8650
  raises ESR `0x96000061`.
- No `volatile` arithmetic: every increment is `*reg = *reg + 1` style.
- No raw-address function pointer without
  `ptrauth_sign_unauthenticated(..., ptrauth_key_function_pointer, ...)`.
- The C++ translation unit has no static constructors, so the build recipe can
  defer (or omit) `__mod_init_func` handling with `required=False`.

## Keys

Both key paths read status registers through the SPMI arbiter v7 observer
channel (`0x0C440000`), the same access the UEFI `Tb710fuKeyInputDxe` uses:

- power: PON SID 0 PID `0x13`, `RT_STS` + `0x10`, gen3 KPDPWR bit 7;
- volume-down: same block, gen3 RESIN bit 6;
- volume-up: PM8550 SID 1, PID `0x8D`, `RT_STS` + `0x10`, bit 0 active-low.

Sampling every 10 ms with a two-sample filter covers the 15 ms debounce the
board declares. Repeat is left to macOS: the filter is stepped with repeat
disabled, and only a stable change produces a report, so the HID event driver
generates key repeat while the Consumer usage stays set. The key descriptor is
one byte, no report id: bit 0 power, bit 1 volume-up, bit 2 volume-down.

## Host evidence

Run (Python 3, no XNU needed):

```
python3 scripts/check-tb710fu-input-host.py \
  --kernel .local/prelink-kernel.macho \
  --firmware .tmp-fw/lib/firmware/novatek/novatek_nt36532_fw.bin
```

Result on 2026-11-27:

```
PASS 322 checks
hid slot table re-proved against .../.local/prelink-kernel.macho
firmware parse=0 bytes=245760 ilm=0x000200+130615@0x000000 dlm=0x022000+82439@0x100000 dlm-crc=ok
PASS
```

Coverage, by group:

| Group | What is actually asserted |
| --- | --- |
| GENI | register model: identity words, protocol/FIFO decode, `IF_DISABLE` only under `probe_fuse`, command packing, TX/RX length words, completion/error bits, timeout, recovery, refusal when the window reads all-ones |
| NVT | page-select framing, 0x80/0x40 opcodes, chunked writes, identify without reset, frame burst length (`+1` status byte), header marker/complement check, 60x40 matrix decode, node centres and clamping |
| tracker | id stability across frames, radius gate, `max_missing` ageing, jitter suppression, clamp to logical max |
| HID | descriptor byte-for-byte expectations, report lengths (61 bytes / 10 contacts, 1 byte / keys), report-bit accounting (488 and 8 bits), contact packing and null-array guard |
| SPMI/keys | arbiter layout, APID discovery for PID `0x13` and `0x8D`, observer read path, two-sample filter, repeat disabled, mapping to HID bits, power/volume polarity per DTS |
| firmware | real blob parse (offsets, SRAM addresses, lengths), vendor CRC-32 over the DLM payload, rejection of truncated and inconsistent images |

The suite compiles the C layer with
`-std=c11 -O2 -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -fno-strict-aliasing`,
so it also proves the headers are self-contained and warning-clean.

## Firmware image: validated, never uploaded

`.local/input-sources/firmware/MD5SUMS` pins
`bca72e403b2b4a521127dcc84fc9848f  novatek/novatek_nt36532_fw.bin`
(245760 bytes). The header is six little-endian 32-bit triples, not the
two-word guess the early work used:

| Offset | Field | Blob value |
| --- | --- | --- |
| `0x00` | ILM file offset | `0x000200` |
| `0x04` | ILM SRAM address | `0x000000` |
| `0x08` | ILM byte count | `0x01FE37` |
| `0x0C` | DLM file offset | `0x022000` |
| `0x10` | DLM SRAM address | `0x00100000` |
| `0x14` | DLM byte count | `0x014207` |
| `0x18` | stored checksum | `0xF97BA55A` (recorded, not verified: the reference checksums ILM after relocation to SRAM) |
| `0x1C` | DLM checksum | `0x8E59A050`, equals vendor CRC-32 of the DLM payload (verified) |

Vendor CRC-32: initial `0xFFFFFFFF`, polynomial `0x04C11DB7`, no final
inversion, little-endian 32-bit words XORed in
(`ShengNovatekTouch/NvtProtocol.c`, revision `3b44b1b7`).

There is deliberately no download path: a wrong upload resets the controller
and leaves the panel without a digitizer until the next power cycle. The merge
may pack the blob as an addon entry and call
`tb710fu_input_firmware_check(image, bytes)` to fingerprint it; that call
parses and checks the DLM CRC and never writes to the controller.

## Merge wiring (not done here, by rule)

This PR touches only `package/Tb710fuInput/`, `scripts/build-tb710fu-input.py`,
`scripts/check-tb710fu-input-host.py` and this document. Three items belong to
the merge owner:

1. **`board.toml` `[kernel] archives`** — add `"build-tb710fu-input.py"` (after
   `"build-tb710fu-power.py"` is a natural place). The framework turns each
   entry into a `-Wl,-force_load,<tree>/build/tb710fu-input/libTb710fuInput.a`.
2. **`package/Macos27Services.cpp`** — add a weak declaration next to the
   existing ones
   (`extern "C" int tb710fu_input_start(void) __attribute__((weak_import));`)
   and a guarded call in the probe section next to `tb710fu_ethernet_probe()`.
   Weak import keeps a build without this archive working, which is why the
   archive is a separate entry rather than extra objects in the services
   archive.
3. **Optional firmware addon** — if `novatek_nt36532_fw.bin` is packed like the
   ADSP blobs (`package/Tb710fuAdsp/Tb710fuAddon.h` is the pattern), call
   `tb710fu_input_firmware_check()` once after the addon is mapped.

There is no per-archive registry in `package/Kit.toml` in this tree (it only
carries kit name/version/description), and no `scripts/build-tb710fu-package.py`
exists here; the archive list in `board.toml` is the actual mechanism, as it
already is for power, GPU, display, CDC and storage.

## Open items and risks

- **Coordinates need calibration.** Node centres assume a 60x40 grid spread
  evenly over 3200x2000. Touch may be offset or scaled; that needs a finger on
  the panel and a one-line correction in `tb710fu_nvt_node_x/y`.
- **`handleReport` parameters and device properties are unproven.** The call
  uses report type 0, options 0, plus `Built-In`, `DisplayIntegrated`,
  `HIDDefaultBehavior`, `PrimaryUsagePage`/`PrimaryUsage` and `Transport`
  properties. Whether that is enough for backboardd to open the digitizer can
  only be answered on hardware.
- **Interrupt mode is partial.** `tb710fu-touch-irq=1` only clears the poll
  path; there is no GIC handler, no `IOInterruptEventSource`. Leave it at 0.
- **No power management.** The frame and key threads never stop on sleep or
  wake; only `tb710fu_input_stop()` sets the stop flag, and that is terminal for
  the boot. A sleep/wake story needs a different design.
- **FIFO availability is judged by the first transfer.** If the fuse really did
  disable the FIFO interface, stage 3 fails closed: no HID device is published.
  There is no DMA/SMMU path in this port to fall back on (`spi4` declares no
  `iommus`).
- **Volume-up polarity** rests on `GPIO_ACTIVE_LOW` in the board DTS plus the
  PMIC GPIO convention (RT_STS bit 0 low = pressed). If the key reads inverted
  on hardware, one bit in `Tb710fuKeys.c` is the fix.
- **Archive never compiled.** The C layer is compiled by the host check; the
  IOKit translation unit has only been reviewed against the in-tree XNU headers
  and the patterns that already compile here (`Tb710fuEthernet.cpp`,
  `GopDisplayStart.cpp`, `SourceStorage.cpp`). First macOS runner build is the
  real compiler gate.

## Out of scope

Stylus/pen reporting, hover, palm rejection, gesture recognition, the panel
reset line, display backlight and audio/haptic feedback. The digitizer is
published as a plain multi-touch device; anything above that belongs to macOS
userspace once the device is actually visible.
