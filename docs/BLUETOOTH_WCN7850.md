# TB710FU Bluetooth: Qualcomm WCN7850 "Hamilton" over GENI HSUART (SM8650Q-AB, macOS 27.3)

Target: Lenovo TB710FU, Qualcomm SM8650Q-AB, macOS 27.3 (26C5035r, XNU 12377.121.6, arm64e).
Package: `package/Tb710fuBluetooth/` · archive `libTb710fuBt.a` · boot argument `bt` (default: off).

**Stage statement.** Everything below is an archive plus a host-verified driver. It has **not** run on
this device. A built archive is not a prelinked kernel, a prelinked kernel is not a booted kernel, a
booted kernel with `bt=4` is not a working Bluetooth stack: this package publishes an `IOService` with an
HCI byte-level `IOUserClient` and attaches no `IOBluetoothHostController`. Each bring-up stage therefore
has its own boot argument, and every stage fails closed.

## 1. Hardware provenance (every constant cites its node)

Inputs, read-only: `d:\unixnu-tb710fu-clean\.local\input-sources\`. Nothing here is inherited from the
SM8550 (sheng) port; that board's serial engine, GPIO numbers, clock roots and firmware names all differ.

| Fact | Value used | Source |
|---|---|---|
| Serial engine | `serial@898000`, `qcom,geni-uart`, `reg <0 0x00898000 0 0x4000>` | `sm8650.dtsi:2489-2500` (`uart14`) |
| SE interrupt | `GIC_SPI 461` level high → INTID 493 (recorded, not requested) | `sm8650.dtsi:2493-2495` |
| SE clock | `<&gcc GCC_QUPV3_WRAP2_S6_CLK>` | `sm8650.dtsi:2496-2498` |
| QUP wrapper | `geniqup@8c0000`, `reg <0 0x008c0000 0 0x2000>` (wrapper 2; SE6 at 0x898000 is inside it) | `sm8650.dtsi:2026-2028` |
| GCC | `clock-controller@100000`, `reg <0 0x00100000 0 0x1f4200>` | `sm8650.dtsi:1967-1969` |
| UART pins | TX `gpio26`, RX `gpio27`, function `qup2_se6`, drive 2 mA, bias pull-up | `sm8650.dtsi` `qup_uart14_default` |
| Flow pins | CTS `gpio24`, RTS `gpio25`, function `qup2_se6`, drive 2 mA, bias pull-down | `sm8650.dtsi` `qup_uart14_cts_rts` |
| Pin mux value | **1** (`qup2_se6` is the second entry of each pin's own function list; pinctrl-msm writes that index, not global enum 103) | `.work/refs/pinctrl-sm8650.c` `[24]..[27]` |
| BT_EN | `gpio17`, function `gpio`, drive 16 mA (value 7), bias-disable | `dts:1020-1026`, `wcn7850-pmu` `bt-enable-gpios` at `dts:254-261` |
| SW_CTRL | `gpio18`, function `gpio`, input, bias pull-down | `dts:1028-1032` |
| BT node | `&uart14 → bluetooth { compatible "qcom,wcn7850-bt"; max-speed = <3200000>; 7 *-supply; }` — **no `enable-gpios`, no `swctrl-gpios`** | `dts:1223-1239` |
| TLMM | `pinctrl@f100000`, `reg <0 0x0f100000 0 0x300000>`; 0x1000 per pin, `.ctl` +0x0, `.io` +0x4 | `sm8650.dtsi:6113-6125`, layout `pinctrl-sm8650.c:30-58` |
| Rails | `vdd` s4i, `vddio` l15b, `vddio1p2` l3c, `vddaon` s2c, `vdddig` s3c, `vddrfa1p2` s1c, `vddrfa1p8` s6c, `RPMH_RF_CLK1` | `dts:263-269` |
| PMU LDOs | `ldo0..ldo6` behind the WCN7850 PMU interface (not RPMh) | `dts:273-310` |

Consequences the driver accepts rather than guesses:

* BT_EN and SW_CTRL come from the **PMU node**, not from the Bluetooth node. A driver that looked for
  `enable-gpios` in `bluetooth@…` would find nothing on this board.
* Because the rails are PMU LDOs behind an interface this package does not implement, **no PMIC or RPMh
  register is written**. A rail provider installs itself through
  `tb710fu_bluetooth_set_rail_provider()` before this driver starts, or the boot chain is relied on to
  have left them up. `bt=1` reports whether the pulse and the pin mux took effect either way.
* `GIC_SPI 461` is recorded but never requested: the SE's `SE_IRQ_EN` stays cleared and M/S status words
  are polled. That keeps a 493 interrupt out of the picture until the transport is proven.

## 2. GENI HSUART transport (`Tb710fuBluetoothGeni.c`)

Register offsets and field encodings are from upstream QUP v3, not invented: `include/linux/soc/qcom/geni-se.h`,
`drivers/tty/serial/qcom_geni_serial.c`, `drivers/soc/qcom/qcom-geni-se.c`. The same values are already in
use on this SoC class by the repository's GENI I2C driver (`GeniI2c.h`: `SER_M_CLK_CFG 0x48`, `M_CMD0 0x600`
with the opcode at bit 27, FIFO ports `0x700/0x780`, status `0x800/0x804`).

Mode: **FIFO/PIO only** — no SE DMA, no GSI, no GPI, no IOMMU work. Transfers are 32-bit words, four bytes
per word, with `LAST_BYTE` set on the final word so the engine terminates the packet.

Order at `bt=1` (`tb_bt_geni_probe` then `tb_bt_geni_config`):

1. Read-only identification: `FW_REVISION_RO` (0x068) must decode protocol **2** (`GENI_SE_UART`); the
   wrapper's `QUPV3_HW_VER` (0x4) is read through the caller and only recorded. `HW_PARAM_0/1` (0xe24/0xe28)
   give FIFO depths and widths.
2. Cancel any stale `M_CMD`/`S_CMD` (`M_CMD_CTRL 0x604` / `S_CMD_CTRL 0x634`, cancel then clear status).
3. `SER_M_CLK_CFG`/`SER_S_CLK_CFG` (0x048/0x04c): `SER_CLK_EN`, `CLK_DIV` in bits 4..15 (max 0xfff).
4. UART block: `TX_TRANS_CFG 0x25c`, `RX_TRANS_CFG 0x280`, `TX_WORD_LEN 0x268`/`RX_WORD_LEN 0x28c` = 8 data
   bits, `TX_STOP_BIT_LEN 0x26c` = `TB_TX_STOP_BIT_LEN_1`, `RX_STALE_CNT 0x294` = 160, watermarks
   `0x80c/0x810/0x814` = 2 words.
5. Packing: both directions configured with `geni_se_config_packing` values (two vectors), `BYTE_GRAN 0x254`
   = 4, `DMA_MODE_EN 0x258` cleared.
6. Clock select `CLK_SEL 0x07c` to the GCC root that the clock plan programmed, `CGC_CTRL 0x028 |= 0x7f`,
   `OUTPUT_CTRL 0x024 = 0x7f`, `IF_DISABLE` cleared.
7. Oversampling is **16**, not 32: QUP ≥ 2.5 halves `UART_OVERSAMPLING`, so
   `clk_div = ceil(rate / (baud * 16))`, and the driver asserts `clk_div <= 0xfff`.
8. RX armed with `M_CMD0 0x600` (`UART_START_READ` opcode with `S_PARAM_RFR_OPEN`), TX by one `M_CMD0` write
   per packet; `M_IRQ_STATUS 0x610` / `S_IRQ_STATUS 0x640` are cleared by writing back the bits read.

Baud switch (`tb_bt_geni_set_rate`, used at `bt=2` for 115200 → 3 200 000):

* `tb_bt_geni_set_flow(false)` first: `TX_TRANS_CFG |= CTS_MASK` and `MANUAL_RFR 0x2ac |= RFR_NOT_READY`,
  the sequence `hci_qca` uses around a rate change.
* TX FIFO must be drained (`tb_bt_geni_tx_words`), then RX stopped, `SER_*_CLK_CFG` rewritten, RX restarted,
  then `set_flow(true)`: `CTS_MASK` cleared and RTS released so the engine honours the hardware handshake
  on gpio24/25. `tb_bt_geni_cts()` samples the CTS level from `SE_GENI_IOS 0x908` bit 1 for evidence.
* A baud change is only attempted when the clock plan has an exact entry for the rate; there is no
  “close enough” divider.

**MMIO safety (the rule that matters on SM8650).** `Tb710fuBluetoothGeni.c` never touches device memory
itself: it goes through the `TbBtGeniOps` function table, and `Tb710fuBluetooth.cpp` implements that table
with `rd32`/`wr32` — a single aligned 32-bit volatile load or store plus `dsb sy`. `memcpy`, `bzero` and
`memset` only ever see `IOMallocData` buffers (firmware images), never a `Window`. No `volatile` value is
ever incremented (`val = *reg; *reg = val + 1;` instead), so `-Werror,-Wdeprecated-volatile` cannot fire.

## 3. Clock plan (`Tb710fuBluetoothClock.c`)

Source: SM8650 GCC description in `.work/usb-cdc-hardware-reference/linux-gcc-sm8650.c`.

* Root `gcc_qupv3_wrap2_s6_clk_src`: `cmd_rcgr 0x1e760`, `mnd_width 16`, `hid_width 5`, `parent_map_10`
  (lines 1191-1198). GPLL0_OUT_EVEN is index 6 of that map (389-394); `F(7372800, GPLL0_EVEN, 1, 384, 15625)`
  is the 7.3728 MHz reference entry (1167-1180).
* Branch `gcc_qupv3_wrap2_s6_clk`: halt `0x1e754`, enable `0x52010` bit 10 (2788-2793).
* Core branch `gcc_qupv3_wrap2_core_clk`: halt `0x23294`, enable `0x52010` bit 0 (2627-2632).
* Offsets are **inside GCC** (base 0x00100000); the driver passes them through unchanged.

| Baud | M / N | Divider | Source rate |
|---|---|---|---|
| 115 200 | 384 / 15625 | 4 | 7 372 800 |
| 921 600 | 768 / 15625 | 1 | 48 000 000 |
| 3 000 000 | 4 / 25 | 1 | 48 000 000 |
| **3 200 000** (board `max-speed`) | **64 / 375** | **1** | **51 200 000** |
| 4 000 000 | 16 / 75 | 1 | 64 000 000 |

`CFG = 0x2601` (`1 | 6<<8 | 2<<12`). For 115 200 the 16-bit M/N halves are `N=0xC476`, `D=0xC2F6`
(M=0x0180, D=0x3D09). D-state is written at `cmd_rcgr + 0x14` (0x1e774). The RCG update sequence is
`CMD_RCGR` root-enable → `CFG` → `M` → `N` → `D` → `CMD_UPDATE`, then poll `CMD_RCGR` bit 31; each branch
vote is released before its halt bit is polled.

## 4. WCN7850 power and pin sequence (`Tb710fuBluetoothPower.c`)

TLMM word layout (pinctrl-sm8650.c `PINGROUP()`): mux at bit 2, drive at bit 6 (`(val+1)*2 mA`), pull 2 bits
at bit 0 (0 none / 1 down / 2 keeper / 3 up), output-enable at bit 9, input at bit 0, output at bit 1 of `.io`.

```
ctl(17) = 0 | 7<<6 | 0<<0 | 1<<9      BT_EN  : gpio, 16 mA, no bias, output
ctl(18) = 0 | 0<<6 | 1<<0 | 0<<9      SW_CTRL: gpio, 2 mA, pull-down, input
ctl(26) = 1 | 0<<6 | 3<<0 | 1<<9      TX     : qup2_se6, 2 mA, pull-up, output
ctl(27) = 1 | 0<<6 | 3<<0 | 0<<9      RX     : qup2_se6, 2 mA, pull-up, input
ctl(24) = 1 | 0<<6 | 1<<0 | 0<<9      CTS    : qup2_se6, 2 mA, pull-down, input
ctl(25) = 1 | 0<<6 | 1<<0 | 1<<9      RTS    : qup2_se6, 2 mA, pull-down, output
```

The enable pulse: drive BT_EN low, wait, raise it, then sample SW_CTRL for a bounded number of polls
(each 1 ms) to record whether the module reports itself booted. `tb_bt_power_off()` returns the line low
and re-runs the two BT pin words. The rail hook (`TbBtRailOps`) is optional: with no provider the pulse is
still performed, so `bt=1` distinguishes “no rails” from “no UART firmware” by the SE protocol id alone.

## 5. Firmware: `hmtbtfw20.tlv` and `hmtnv20.bin` (`Tb710fuBluetoothQca.c`)

Board stock members, per `.local/input-sources/firmware/MD5SUMS`:

| File | Size | MD5 | Content |
|---|---|---|---|
| `lib/firmware/qca/hmtbtfw20.tlv` | 274 128 B | `bcaca7b2a1e02be62fbffaf746d52440` | QCA TLV type 1, 1 129 segments |
| `lib/firmware/qca/hmtnv20.bin` | 9 592 B | `c047013753ce0c3bec603a590b9fa7e4` | QCA TLV type 2, 40 segments |

Format facts (checked against the real bytes and `drivers/bluetooth/btqca.h`):

* `tlv_type_hdr { le32 type_len }`: type is the **low 8 bits**, length is the value **>> 8**. So the patch
  file begins `01 cc 2e 04` → type 1, length 274 124 (0x042ecc); NVM begins `02 74 25 00` → type 2,
  length 9 588.
* Patch header (24 bytes after the TLV header): `total_size 274092`, `data_length 274056`,
  `format_version 1`, `signature 0`, `download_mode 3`, `product_id 0x19`, `rom_build 0x0200`,
  `patch_version 0x80d2`, `entry 0`.
* Download: `MAX_SIZE_PER_TLV_SEGMENT = 243`. Patch mode 3 (`VSE_CC`) waits for a command complete only on
  the **last** segment; NVM waits on **every** segment (`cresp 0`, event `0x1e`, `status 0`).
* NVM tags rewritten before download: tag **17** (`data[0] |= 0x80`, `data[1] = baud code`), tag **27**
  (`data[0] |= 0x01`, deep sleep), tag **2** = BD_ADDR when one is supplied. Tag lengths must close the
  image exactly; the driver refuses a file whose tags do not sum to the TLV length.
* Baud codes are `enum qca_baudrate`: 115200=0 … 921600=10 … 3000000=14, 4000000=15, **3200000=17**.
* ROM version reply: `rtype` then one extra byte, then a 12-byte structure (`soc_id`, `rom_version`,
  `build_id`); the driver also accepts the 13-byte form without the extra byte and records where it found
  the structure in `parse_offset`.

**Where the bytes come from.** `Tb710fuBluetoothFirmware.h` declares the four `tb710fu_fw_*` entry points
(`open`, `open_addon`, `read`, `close`) `weak_import`. They are supplied by `package/Tb710fuAdsp` (the
Tb710fuAddon firmware reader used by the ADSP subsystem). If that archive is not linked, `bt=3` reports
“no firmware reader” instead of failing to link. The lookup order is the addon (`hmtbtfw20.tlv`,
`hmtnv20.bin` by basename), then the VFS directory `/private/var/firmware/qca`, overridable with
`tb710fu-bt-fw=<dir>`. **Merge prerequisite: `bt≥3` requires the ADSP archive (or an equivalent provider of
the four `tb710fu_fw_*` symbols).** No firmware image is embedded in this package.

## 6. HCI H4 framing and the macOS service

`Tb710fuBluetoothHci.c` is pure C, no IOKit: `tb_bt_h4_packet_len` (command 3-byte header, ACL 4, SCO 3,
event 2, ISO 4), `tb_bt_h4_build_cmd`, `tb_bt_h4_parse_cmd_complete` / `parse_cmd_status`
(`params_len = nparams - 4`, i.e. status byte excluded, and `nparams >= 4` required), and a streaming parser
(`init` / `feed` / `sink`) that accepts split and coalesced packets, drops garbage bytes and drops oversize
packets (`TB_BT_H4_MAX_PACKET`).

`Tb710fuBluetooth.cpp` publishes `Tb710fuBluetooth : IOService` at `bt=4` with one `IOUserClient`:

| selector | direction | shape |
|---|---|---|
| 0 `writeHci` | in 260 B H4 command | out 16 B `{written, event bytes appended to the ring, status, reply kind}` |
| 1 `readHci` | scalar: byte count | out 260 B raw H4 |
| 2 `status` | — | 96 B = `native_tb710fu_bt_stats[24]` |
| 3 `power` | scalar 0/1 | runs the power pulse again |

A transaction is synchronous under one `IOLock`; there is no poll thread and no interrupt. `gTap` is a
single transaction-scoped event tap armed by `hciTransact(op, params, plen, timeout, &snapshot)`, so a
reply cannot be stolen by a stale waiter.

**Zero Apple-private link symbols.** No `IOBluetoothFamily` (or networking/graphics/audio family) symbol is
referenced: the family's 26C5035r vtable layout is not measured on this board, so adopting it would be a
guess. The service is byte-level HCI only; attaching a host controller is a separate, later step. Any
`OSMetaClass` lookup that a future revision needs must go through
`OSMetaClass::getMetaClassWithName(...)` at runtime, never a direct symbol reference.

## 7. Boot-argument staged bring-up

`bt` (absent, `0`, or unparsable → the package does nothing). Sub-arguments: `tb710fu-bt-log=0` (quiet),
`tb710fu-bt-speed=<baud>` (must be in the clock table), `tb710fu-bt-fw=<dir>`, `tb710fu-bt-bdaddr=AA:BB:…`.

| `bt` | What runs | Failure semantics |
|---|---|---|
| 1 | Map GENI/GCC/wrapper/TLMM, BT_EN pulse + SW_CTRL sample, all six pin words, SE identification (read-only), GCC branch votes + RCG at 115 200, FIFO UART configured, hardware flow control on | Stage fails if the SE protocol is not UART (`no uart se firmware`) or the clock plan is unknown; power is dropped and windows unmapped |
| 2 | Stage 1, then HCI Reset (best effort), QCA ROM version read, baud switch to `max-speed` (3 200 000, code 17) with clock + divider + flow control re-programmed | A failed version read fails the stage; the reset is recorded but not fatal (btqca resets after NVM, not before) |
| 3 | Stage 2, then `hmtbtfw20.tlv` download, board id, `hmtnv20.bin` with tags rewritten, `QCA_DISABLE_LOGGING`, the authoritative HCI Reset, build info, `Read_BD_ADDR` (and optional write) | Any download error or a tag table that does not close the image fails the stage and drops power |
| 4 | Stage 3, then publish the `IOService` + `IOUserClient` | Refuses clients if the transport is not live |

`native_tb710fu_bt_stats[24]` is the evidence read-out (`status` selector 2): stage (negative = the stage
that failed), last errno, QUP hardware version, SE protocol, FIFO words, RCG command/CFG, branch register,
baud, divider, ROM product/version, patch product/version, segment counts, board id, tag count, BD_ADDR
words, CTS, flow control, firmware byte counts. `native_tb710fu_bt_text[96]` carries the human stage label.

## 8. Host verification and the archive build

```sh
# unit tests + syntax + stubbed-IOKit parse + MMIO hygiene rules (any Linux/macOS host with cc)
python3 scripts/check-tb710fu-bt-host.py
```

`tests/tb710fu_bluetooth_test.c` (≈1 000 lines, six fake register/IO groups) checks: board constants, the
clock plan and every M/N/D/CFG recomputation, the RCG program sequence, GENI probe/config/FIFO traffic/
`LAST_BYTE`/overflow/flow control/CTS/rate change, H4 build and CC/CS decoding, the streaming parser
(split, coalesced, garbage, oversize), QCA TLV and patch headers, NVM tag rewriting, download order and
segment counts, version/board-id/baud commands, BD_ADDR, TLMM control words and the BT_EN pulse write order.
Current result: **310 checks, 0 failures**, clean under `-fsanitize=address,undefined` and under clang
`-Wall -Wextra -Werror`. The textual rules additionally reject `memcpy`/`bzero`/`memset`/`bcopy`/`memmove`
anywhere in the package, any `++`/`--` on a volatile, and any `IOBluetooth*`/`IONetworkingFamily`/
`IOGraphicsFamily`/`IOAudioFamily` reference (comments are stripped first, so the prose above cannot trip
it).

```sh
# inside the materialised XNU tree, after the minimal kernel exists (UNIXNU_DEVICE set)
python3 scripts/build-tb710fu-bt.py            # archive + build-report.json
python3 scripts/build-tb710fu-bt.py --plan     # no tree needed: sources, records, boot args
```

The build compiles the five C files with the recorded XNU C command (`-mgeneral-regs-only`), the provider
with the recorded IOKit C++ command (`arm64e-apple-ios27.0`), moves
`__GLOBAL__sub_I_Tb710fuBluetooth.cpp` from `__mod_init_func` to `__mod_deferred` (exporting the symbol),
runs `xcrun ar rcs libTb710fuBt.a`, writes `build/tb710fu-bt/build-report.json` and invalidates
`mach.development.bcm2837.unstripped.noctf` when the digest changes.

## 9. Merge wiring (final integration; this worktree does not touch these files)

1. `package/Kit.toml` — add the component block (suggested; `Kit.toml` is not modified here):

   ```toml
   [[component]]
   name = "Tb710fuBluetooth"
   archive = "libTb710fuBt.a"
   sources = ["Tb710fuBluetooth.cpp", "Tb710fuBluetoothGeni.c", "Tb710fuBluetoothClock.c",
              "Tb710fuBluetoothHci.c", "Tb710fuBluetoothQca.c", "Tb710fuBluetoothPower.c"]
   headers = ["Tb710fuBluetoothBoard.h", "Tb710fuBluetoothGeni.h", "Tb710fuBluetoothClock.h",
              "Tb710fuBluetoothHci.h", "Tb710fuBluetoothQca.h", "Tb710fuBluetoothPower.h",
              "Tb710fuBluetoothFirmware.h"]
   build_script = "scripts/build-tb710fu-bt.py"
   host_checks = "scripts/check-tb710fu-bt-host.py"
   boot_arg = "bt"
   depends = ["Tb710fuAdsp"]   # tb710fu_fw_* reader, weak_import; required from bt>=3 on
   ```

2. `board.toml` `archive` list: `libTb710fuBt.a`.
3. `configs/tb710fu_macos_defconfig`: `-force_load libTb710fuBt.a` and `-DXNU_TB710FU_BT=1`.
4. `package/Macos27Services.cpp`, three lines plus two calls — install the metaclass in the preModLoad
   window and probe after postModLoad:

   ```cpp
   extern "C" void tb710fu_bluetooth_meta_init(void) __asm__("__GLOBAL__sub_I_Tb710fuBluetooth.cpp") __attribute__((weak_import));
   extern "C" void tb710fu_bluetooth_probe(void) __attribute__((weak_import));
   // preModLoad window: if (tb710fu_bluetooth_meta_init) tb710fu_bluetooth_meta_init();
   // after postModLoad: if (tb710fu_bluetooth_probe) tb710fu_bluetooth_probe();
   ```

5. `porting.toml` `paths`: the six sources, seven headers, build script, host script, this document.
6. CI (`macos-kernel.yml`): run `python3 scripts/build-tb710fu-bt.py` before the link and place the archive
   under `.work/artifacts/` — one bounded run per target, and the kernel target should not be retried until
   the first failure is diagnosed.

## 10. Limits and what still needs hardware

* Nothing here has executed on SM8650. The GENI sequence follows upstream, but `QUPV3_HW_VER` is only
  recorded and oversampling is fixed at 16; a QUP revision below 2.5 would need 32.
* `bt=2` resets the ROM before the QCA version read for evidence only. Whether the ROM answers `0x0C03`
  is unmeasured; the authoritative reset is after the NVM load at `bt=3`.
* The QCA version reply's “one extra byte after `rtype`” is inferred from `btqca.c` (`edl->data + 1`), so
  both 13- and 14-byte replies are accepted and the raw bytes are logged.
* `IOBluetoothFamily`'s 26C5035r slot/instance sizes are unmeasured → no host controller is attached and
  no family symbol is linked.
* The WCN rails are PMU LDOs; without a rail provider the driver relies on the boot chain. A provider that
  speaks the WCN7850 PMU interface is a separate subsystem (PMIC GLINK), not part of this package.
* Interrupts are not used (`SE_IRQ_EN` cleared, INTID 493 unused); the transport is polled.
