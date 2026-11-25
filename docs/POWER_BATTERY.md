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
2. Firmware packaging is the gating item: `adsp.mbn` is 34.8 MB and `adsp_dtb.mbn` 5.9 MB. This project's built
   `vendor_boot.img` is 11.8 MB inside a 96 MB partition, so both files fit in that FAT payload; the alternative is
   reading them from a UFS partition at boot.
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

Wiring: `board.toml` archive list, `configs/tb710fu_macos_defconfig` (`-force_load` of `libTb710fuPower.a`,
`-DXNU_TB710FU_POWER=1`), and `package/Macos27Services.cpp` (metaclass init before `postModLoad`, weak start call).

Boot arguments:

- `battery=1` — required; without it the provider only prints that it was not requested.
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

## What device acceptance still needs

Publishing an `IOPMPowerSource` needs the transport. The facts for it are in this board's vendor SoC DTB
(decompiled from `vendor_boot_a.img`):

- PAS node `/soc/remoteproc-adsp@03000000`, `compatible = "qcom,pineapple-adsp-pas"`, reg `0x03000000 0x10000`,
  `clocks = xo`, `memory-region = <adspslpi 0x9df00000 + 0x4080000, q6_adsp_dtb 0x9de80000 + 0x80000>`,
  `interrupts-extended` `wdog/fatal/handover/ready/stop-ack`, `qcom,smem-states = <master-kernel 0>` (`stop`).
  PAS ids follow the SM8550 family (1 for `adsp.mbn`, 36 for `adsp_dtb.mbn`); confirm them against
  `qcom_q6v5_pas.c` for `pineapple-adsp-pas` before the first load.
- SMEM `0x81d00000 + 0x200000`, TCSR mutex hwlock index 3; SMP2P `/soc/qcom,smp2p-adsp`: `qcom,smem = <443 429>`,
  local-pid 0, remote-pid 2, `mboxes = <ipcc 3>`, entries `master-kernel`, `slave-kernel`.
- IPCC `/soc/qcom,ipcc@406000` (GIC SPI 229 = 0xe5), LPASS client 3; AOSS QMP `0xc300000 + 0x400`, client 0;
  apps RSC `0x17a00000` drv 2 with its TCS block at +0xd00; command DB `0x81c60000`.
- ADSP `glink-edge` uses mbox channel 3 (`adsp_smem`), label `adsp`/`lpass`, `qcom,glink-label = "lpass"`.
- GLINK channel `PMIC_RTR_ADSP_APPS`; the intents listed on `qcom,adsp_qrtr` belong to QRTR, not this channel.
- These are almost sheng's SM8550 constants (SMEM identical; RSC 0x17a00000 vs 0x17a20000; IPCC 0x406000 vs
  0x408000; ADSP memory 0x9df00000 vs 0x9ea00000), so its probe can be parameterised rather than rewritten.
- Firmware: `adsp.mbn` (34.8 MB) and `adsp_dtb.mbn` (5.9 MB) from this device's own vendor image, the same two files
  the Linux port loads as `qcom/sm8650/xiaoxin/gt/adsp.mbn` / `adsp_dtb.mbn`; `battmgr.jsn` is their metadata.

Reference implementation to port: sheng's ADSP stack first (`.work/UniXNU-sheng/package`, BSD-2-Clause-Patent,
same PMIC GLINK battery protocol already hardware-verified on SM8550), with the SM8650/ITB710FU DT facts above
replacing its SM8550 addresses, then `UniXNU-xiaomipad8pro` → `package/PianoPower` (`PianoAdsp.cpp`, `QcomGlink.c`,
`QcomQrtr.c`, `QcomSmem.c`, `QcomSmp2p.c`, `QcomQmp.c`, `FatVolume.c`, `PianoAdspFirmware.c`) for anything newer.
Note that PianoPower's battery protocol is Xiaomi's MCA, not the Qualcomm battery manager this board speaks; only
its transport is reusable.

Acceptance for the transport, in order: (1) `battery=1` prints the ADT survey; (2) channel opens and the enable
request answers (`native_battery_stats[9]` grows); (3) one full cycle yields plausible voltage/current/temperature
and a percentage that matches Android on the same charge; (4) unplug/replug changes `ExternalConnected`/`IsCharging`
within one refresh; (5) the menu bar shows the same percentage after a reboot.

## Limits

- No battery percentage is invented when the transport is absent: the provider registers nothing and prints one
  line. `native_battery_stats[0]` records the stage (`-1` no transport, `-2` channel not open, `4` published).
- `ShengPlatformPower` (the previous reference's iPadOS ABI fixture) is still not imported; only the battery
  manager design and the transport shape are taken from sheng, with its own device's property order corrected.
- Chromium/AppleSmartBattery keys this package does not publish (serial, manufacture date, SOH) are not faked.
