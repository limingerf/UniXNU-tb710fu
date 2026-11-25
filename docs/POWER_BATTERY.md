# Battery and charging

Status: the battery software layer is **host-verified**; the device shows no battery yet because the transport it
needs (ADSP / PMIC GLINK) is not part of this kernel. `porting.toml` area `power` carries this same split.

| Piece | State |
|---|---|
| Battery-manager message codec | implemented, host checks pass (`scripts/check-tb710fu-power-host.py`) |
| `IOPMPowerSource` provider (capacity, charging, adapter details) | implemented, compiled by `build-tb710fu-power.py` |
| PMIC GLINK transport to the ADSP | **not implemented**; the provider prints one line and registers nothing |
| Charging itself | the PMIC/DSP firmware's; this package never enables or disables charging |

## What this board actually has

The battery and charger are owned by the audio DSP, not by the application processor's PMIC bus:

- The live DT declares `/soc/qcom,pmic_glink` with `qcom,pmic-glink-channel = "PMIC_RTR_ADSP_APPS"`,
  `qcom,subsys-name = "lpass"`, `qcom,protection-domain = "tms/servreg", "msm/adsp/charger_pd"` and a child
  `qcom,battery_charger` (`compatible = "qcom,battery-charger"`).
- The same channel appears on the ADSP's `glink-edge` (`/soc/remoteproc-adsp@03000000/glink-edge`,
  `qcom,glink-label = "lpass"`, `qcom,glink-channels = "PMIC_RTR_ADSP_APPS"`, mbox `adsp_smem`, remote-pid 2).
- This device's `vendor_dlkm.img` loads `qti_battery_charger.ko` (on the same channel, owner 32778) and
  `qcom-spmi-adc5-gen3.ko`; the board DT has **no** fuel-gauge, charger or battery ADC node.

Consequence: no direct PMIC/SPMI read on this board can produce a state of charge. Any percentage has to come from
the battery manager on the DSP. This is the same architecture UniXNU-sheng used on SM8550 (hardware-verified there).

## The Linux port on this same board

The mainline Linux port for this exact device (`MeteoricmeteorLL/tb710fu-linux`, Linux 7.2.0 mainline plus a
49-file board diff) settles the architecture and gives a working reference:

- `docs/STATUS.md` lists **Battery reporting** and **Charging** as working on the real device (⚠️ partial):
  `qcom_battmgr` drives the battery (upower needs its system user and `PrivateUsers=no` under a root session),
  the tablet charges over USB-C, and some properties intermittently log `-110` (`ETIMEDOUT`).
- `board/hardware.md`: `battery | qcom_battmgr (ADSP)`. There is **no board-specific gauge or charger driver** in
  that port: stock mainline `drivers/power/supply/qcom_battmgr.c` is the whole battery stack.
- The ADSP is loaded by the usual remoteproc path; the board DTS names the firmware files
  `qcom/sm8650/xiaoxin/gt/adsp.mbn` and `adsp_dtb.mbn` (`&remoteproc_adsp { firmware-name = ...; status = "okay"; }`),
  and the firmware set also carries a small `battmgr.jsn`.
- `qcom_battmgr.c` (SM8550 pmic-glink variant, which this board's DT compatible selects) sends the **same 24-byte
  property request** (`{owner, type, opcode, battery 0, property, value}`) and the **same 28-byte
  notification-enable frame** as `Tb710fuBattmgr.c`, and numbers the properties exactly as this board's
  `qti_battery_charger.ko` does (`CHG_CTRL_LIM` 10 / `CHG_CTRL_LIM_MAX` 11, temperature 12, charge counter 14,
  cycle count 15). That is an independent confirmation of the table extracted from the local module.
- Its notification classes are `0x30`/`0x32`/`0x34` (a property family), `0x80` (battery status), `0x81` (battery
  information), `0x83` (charging state). `Tb710fuBattmgr.c` now re-reads the slow set (model, design/full capacity,
  cycles) only on `0x81`, instead of waiting for the ten-cycle pass.
- The `-110` timeouts are the behaviour the codec is built to tolerate: the Linux port ships no workaround for them,
  so one unanswered property is expected and must not invalidate the rest of the cycle.

Consequences for this port:

1. We do not need a TB710FU-specific battery driver; the DSP protocol is generic Qualcomm. The one missing piece is
   the **ADSP + PMIC GLINK transport**.
2. Firmware packaging is the gating item: `adsp.mbn` is 34.8 MB and `adsp_dtb.mbn` 64 KB. The current payload FAT
   has 5.49 MiB free of its 81.1 MiB budget (the LZMA kernel and the Data seed take the rest), so 34.9 MB do not
   fit raw; the realistic sources are the boot addon (zlib, 22.3 MB, needs a larger payload), a UFS partition read
   in place, or a file installed on the Data volume.
3. The closest transport template is sheng's own ADSP stack (`.work/UniXNU-sheng/package`: `AdspProbe.cpp`,
   `QcomGlink.c`, `QcomSmem.c`, `QcomSmp2p.c`, `QcomQmp.c`, `QcomRpmh.c`, `AdspFirmware.c`, `ShengAdsp.h`, all
   BSD-2-Clause-Patent): same PMIC GLINK battery protocol, already hardware-verified, and its channel API is the
   shape `Tb710fuPowerLink.h` mirrors. PianoPower remains the newer alternative, but its battery protocol is
   Xiaomi's MCA, so only its transport is reusable.

## Property numbers are this device's own

`qti_battery_charger.ko` keeps its property tables in `.rodata`; the strings are contiguous in source order:

- `battery_props` (file offsets 113201..113631): STATUS, HEALTH, PRESENT, CHG_TYPE, CAPACITY, SOH, VOLT_OCV,
  VOLT_NOW, VOLT_MAX, CURR_NOW, **CHG_CTRL_LIM, CHG_CTRL_LIM_MAX**, TEMP, TECHNOLOGY, CHG_COUNTER, CYCLE_COUNT,
  CHG_FULL_DESIGN, CHG_FULL, MODEL_NAME, TTF_AVG, TTE_AVG, RESISTANCE, POWER_NOW, POWER_AVG, CHG_CTRL_EN,
  CHG_CTRL_START_THR, CHG_CTRL_END_THR, CURR_AVG, PROP_MAX.
- `usb_props` (115649..115892): ONLINE, VOLT_NOW, VOLT_MAX, CURR_NOW, CURR_MAX, INPUT_CURR_LIMIT, TYPE,
  ADAP_TYPE, MOISTURE_DET_EN, MOISTURE_DET_STS, TEMP, REAL_TYPE, TYPEC_COMPLIANT, SCOPE, CONNECTOR_TYPE,
  NUM_PORTS, PROP_MAX.

sheng's table inserted thermal level / its max / charge current before temperature, so its 10..21 are shifted by
one against this board. Using sheng's numbers here would read the charger's current limit as battery temperature.
`Tb710fuBattmgr.c` therefore uses status 0, present 2, capacity 4, voltage 7, current 9, **temperature 12**,
charge counter 14, cycles 15, design 16, full 17, time-to-full 19, time-to-empty 20, charge control limit 10.

The board-level ids (`0x8a/0x8b`, `0xa6/0xa7`, `0xa5`, `0x18`, `0x2a`, `0x33`, `0x03`, `0x07`) belong to the
OPPO/ColorOS "FAC" family this vendor image ships. They are read only when they answer; if a gauge or the second
cell is absent the snapshot falls back to `BATT_CAPACITY`, so a single-cell board does not get a wrong percentage.

## Package layout and wiring

`package/Tb710fuPower/`:

- `Tb710fuBattmgr.h/.c` — the codec: one request in flight, a cycle over the fields, notification enable,
  timeouts that skip a field, retention of the last known charger state, gauge/capacity percentage, adapter
  details, and the optional input-current-limit write.
- `Tb710fuPowerLink.h` — the transport contract (`acquire`, `send`, and the state/receive callbacks). The ADSP
  subsystem is expected to define these three functions; the battery code itself touches no hardware.
- `Tb710fuBattery.cpp` — the `IOPMPowerSource`: work loop, 2 s request timer, 60 s refresh (slow fields every
  tenth), a 16-slot message ring, `battery=1` opt-in, an ADT survey line, and publication only after a charge
  reading exists.

Wiring: `board.toml` archive list, `configs/tb710fu_macos_defconfig` (`-force_load` of `libTb710fuPower.a` and
`libTb710fuAdsp.a`, `-DXNU_TB710FU_POWER=1`, `-DXNU_TB710FU_ADSP=1`), and `package/Macos27Services.cpp` (metaclass
init before `postModLoad`, the ADSP probe ahead of the weak battery start).

## The ADSP transport (`package/Tb710fuAdsp`)

The battery manager only runs inside the audio DSP, so the PMIC GLINK client needs the DSP awake. That is this
package, adapted from UniXNU-sheng's probe (BSD-2-Clause-Patent, hardware-verified on SM8550) with this board's
addresses:

- `QcomSmem.c`, `QcomSmp2p.c`, `QcomGlink.c`, `QcomQmp.c`, `QcomRpmh.c`, `QcomMdt.c`, `QcomScm.h` — copied
  unchanged (SMEM partitions/alloc under the TCSR mutex, SMP2P items, GLINK native protocol on a pair of SMEM
  FIFOs, the AOSS mailbox handshake, RPMh proxy votes, `.mbn` parsing and SCM/PAS calls).
- `Tb710fuAdspFirmware.c` — VFS reads of the two firmware files (no FAT/modem partition path: nothing is
  unpacked from the vendor image).
- `Tb710fuAdsp.cpp` — the probe/thread: maps SMEM/TCSR/IPCC/AOSS/RSC, surveys (`adsp=1`), then for `adsp=2`
  allocates the SMP2P out item, attaches the two GIC SPIs, runs QMP `load_state adsp`, votes lcx/lmx through
  RPMh, authenticates and loads `adsp_dtb.mbn` (PAS 36) then `adsp.mbn` (PAS 1), waits for the DSP's ready bit,
  allocates the GLINK descriptor and TX FIFO items and registers channels.
- `tb710fu_power_link_acquire/send` — the client API from `Tb710fuPowerLink.h`; registration succeeds whether or
  not the DSP has opened `PMIC_RTR_ADSP_APPS` yet, and the state callback reports the open/close.

Constants were checked against mainline, not guessed: `sm8650-adsp-pas` uses `sm8550_adsp_resource` in
`qcom_q6v5_pas.c` (PAS 1, DTB PAS 0x24, crash-reason item 423, `smem_host_id` 2, rails lcx/lmx, load state
`adsp`); `sm8650.dtsi` gives SMEM `0x81d00000 + 0x200000` with TCSR hwlock 3, SMP2P `443/429`, IPCC `0x406000`
(SPI 229, LPASS client 3), AOSS QMP `0xc300000 + 0x400`, apps RSC `0x17a20000` drv 2 with TCS `+0xd00` (SPI 5),
cmd-db `0x81c60000`, `adspslpi` `0x9df00000 + 0x4080000`, `q6-adsp-dtb` `0x9de80000 + 0x80000`; GLINK items
478/479/480 are the ids `qcom_glink_smem.c` itself allocates. `armcpu_spi_attach` resolves in this kernel
(`_armcpu_spi_attach` is in the built symbol table).

Firmware: `adsp.mbn` (34.8 MB) and `adsp_dtb.mbn` (64 KB) come from the Linux port's archive
(`firmware/tb710fu-firmware-20261005.tar.gz`, revision `16e800d4`), verified against its `MD5SUMS`:
`a0686c2b0ed4b6a8a0250c31aa2bc46c` and `4b2480da1dbff4932c6d9f4eb40222e4`. They are **not** built into the
image: the payload FAT leaves only 5.49 MiB free, so 34.9 MB cannot travel raw in `vendor_boot`; the addon route compresses it to 22.3 MB (zlib) but needs a larger payload. Install them once on
the Data volume at `/private/var/firmware/qcom/sm8650/xiaoxin/gt/` (the same paths the Linux port uses), or
point `adsp-fw=DIR` at another mounted directory. The probe retries for about five minutes, so it can start
before the Data volume is mounted.

Boot arguments:

- `battery=1` — required; without it the provider only prints that it was not requested.
- `adsp=1` — read-only survey into `native_tb710fu_adsp_stats`; writes nothing and starts no DSP.
- `adsp=2` — start the DSP and bring up GLINK. Needed before `battery=1` can publish anything.
- `adsp-fw=DIR` — firmware directory, default `/private/var/firmware/qcom/sm8650/xiaoxin/gt`.
- `usb-sdp-ma=N` — input current limit for a standard host port, 0..3000 mA. Default 0: the firmware's limit is
  left alone, which is also what the DSP does when no host asks for more.
- `battery` sets no other charger state; charging stays autonomous in the PMIC/DSP firmware.

## Host verification

```
python3 scripts/check-tb710fu-power-host.py
```

Compiles the codec and `tests/tb710fu_battmgr_test.c` with the host compiler (`-Wall -Wextra -Werror`, sanitizers
when the toolchain has a runtime), runs a fake DSP through a full cycle and checks: percentage from summed gauges
with capacity fallback, voltage/current/temperature units, adapter details and watts, time-to-full/empty, the
notification enable frame, a foreign owner and malformed frames being ignored, a timed-out field keeping the last
known external state, and the input-limit policy (no write at 0; one set at `usb-sdp-ma`, three tries per port).
Result: all checks pass; `build/tb710fu-power-host/report.json` records compiler, command and hashes.

## Device acceptance

The transport is in the tree; nothing about it has been run on the device yet. Two steps remain before battery
readings can exist:

1. **Install the firmware once.** Copy `adsp.mbn` and `adsp_dtb.mbn` (verified copies are in
   `.local/input-sources/firmware/lib/firmware/qcom/sm8650/xiaoxin/gt/`, from the Linux port's archive) to the
   device's Data volume at `/private/var/firmware/qcom/sm8650/xiaoxin/gt/`. They are not committed and not part
   of any image; the payload FAT has only 5.49 MiB free. A different directory works with `adsp-fw=DIR`.
2. **Build with the new archives.** `build-tb710fu-adsp.py` needs the macOS runner (XNU compile records), like
   `build-tb710fu-power.py`; neither has been target-compiled since the transport was added.

Acceptance order on hardware, each step separately observable:

1. `adsp=1` — the survey writes nothing. `native_tb710fu_adsp_stats[4]` shows which of the five SMEM items
   (443, 429, 478, 479, 480) exist and bits 8/9 the APPS/global partitions; `[3]` is the DSP's `slave-kernel`
   value (`-1` when the item is absent, `>0` when the DSP is already up); `[5]` carries the QMP state and AOSS
   link bits; `[6]` the RSC DRV id. A failure here is a fact about boot state, not a driver bug.
2. `adsp=2` — `native_tb710fu_adsp_stats[0]` walks stages 1..13 (`-N` means step N failed, `[1]` its error):
   mapped, surveyed, firmware, SMP2P, doorbells, load-state, rails, dtb, loaded, started, ready, glink, channel.
   `[8]` is the milliseconds from `auth_and_reset` to ready; `[9]/[10]` GLINK messages in/out; `[11]` channels up
   (bit 0 is `PMIC_RTR_ADSP_APPS`), version agreed and broken link; `[12]` opens nobody serves; `[13]/[14]` the
   last SCM return; `[15]` the lcx/lmx vote and RSC completion.
3. `adsp=2 battery=1` — the battery prints its ADT survey, registers at `native_battery_stats[0]=2`, goes to 3 on
   channel up and 4 when it publishes. A published `IOPMPowerSource` should carry a percentage close to Android's
   on the same charge, a plausible voltage/current/temperature, and `[9]` (notifications) should grow.
4. Unplug and replug within one 60 s refresh: `ExternalConnected`/`IsCharging` (seen in `native_battery_stats[5]`
   bits 1 and 2) must follow, and the DSP's occasional `-110` timeout must not lose the charger state
   (`native_battery_raw[8]` records the skipped field).
5. Reboot and compare the menu-bar percentage with the previous boot.

The property table and the transport's addresses are both independently confirmed (this device's
`qti_battery_charger.ko` string table and mainline `qcom_battmgr.c` for the former; `sm8650.dtsi` and
`qcom_q6v5_pas.c` for the latter), so a mismatch at step 1-3 points at boot state (firmware not installed, DSP
already running, a wrong carveout) rather than at the codec.

## Limits

- No battery percentage is invented when the transport is absent: the provider registers nothing and prints one
  line. `native_battery_stats[0]` records the stage (`-1` no transport archive, `-2` the ADSP was not started
  with `adsp=2`, `2` channel registered, `3` channel up, `4` published).
- The ADSP code is sheng's SM8550 sequence with this board's addresses. Every address and PAS/item id above is
  cross-checked against mainline sources, but none of it has been executed on this device; the first `adsp=1`
  survey is what turns those constants into evidence.
- The firmware is a vendor blob from the Linux port and is not committed. The Data-seed path keeps it off the
  boot image deliberately; the cost is that the battery cannot appear before the Data volume is mounted.
- `ShengPlatformPower` (the previous reference's iPadOS ABI fixture) is still not imported; only the battery
  manager design and the transport shape are taken from sheng, with its own device's property order corrected.
- Chromium/AppleSmartBattery keys this package does not publish (serial, manufacture date, SOH) are not faked.
