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

Publishing an `IOPMPowerSource` needs the transport. The facts for it are in this board's DT:

- PAS node `/soc/remoteproc-adsp@03000000`, `compatible = "qcom,pineapple-adsp-pas"`, reg `0x03000000 0x10000`,
  `clocks = xo`, `memory-region = <adsp_dtb, adsp_mem>` (resolve the phandles in `/reserved-memory`),
  interrupts `wdog/fatal/handover/ready/stop-ack`, `qcom,smem-states = <master-kernel 0>` (`stop`).
- SMP2P `/soc/qcom,smp2p-adsp`: `qcom,smem = <0x1bb 0x1ad>`, local-pid 0, remote-pid 2, `mboxes = <ipcc 3>`,
  entries `master-kernel`, `slave-kernel`.
- IPCC `/soc/qcom,ipcc@406000`; ADSP `glink-edge` uses mbox channel 3 (`adsp_smem`), label `adsp`/`lpass`.
- GLINK channel `PMIC_RTR_ADSP_APPS`; the intents listed on `qcom,adsp_qrtr` belong to QRTR, not this channel.

Reference implementation to port: `UniXNU-xiaomipad8pro` → `package/PianoPower` (BSD-2-Clause-Patent;
`PianoAdsp.cpp`, `QcomGlink.c`, `QcomQrtr.c`, `QcomSmem.c`, `QcomSmp2p.c`, `QcomQmp.c`, `FatVolume.c`,
`PianoAdspFirmware.c`) with SM8750 addresses replaced by the SM8650 ones above. Note that PianoPower's battery
protocol is Xiaomi's MCA, not the Qualcomm battery manager this board speaks; only its transport is reusable.

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
