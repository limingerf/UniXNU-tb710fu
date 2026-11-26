# TB710FU platform services: SPMI, clock, reset, backlight, thermal

Subsystem: `package/Tb710fuPlatformServices/`. Target: Lenovo TB710FU
(Qualcomm SM8650Q-AB), macOS 27.3 (26C5035r, XNU 12377.121.6, arm64e).

**Status: host-verified only.** The C cores and the arithmetic pass their suites
on a Linux host; the three IOKit translation units have never been compiled
against the kernel SDK. No RTC was read, no panel dimmed and no sensor sampled on
hardware. The build report keeps `device_rtc`, `device_backlight_brightness` and
`device_thermal` false until a boot log says otherwise.

## 1. Where every number comes from

Every address, bus, GPIO, interrupt, regulator and compatible string is read from
the four pinned devicetree files. Paths below are relative to
`d:\unixnu-tb710fu-clean\.local\input-sources\`.

| Fact | Value | Node | File:line |
| --- | --- | --- | --- |
| SPMI arbiter | `spmi@c400000`, `"qcom,spmi-pmic-arb"` | `spmi_bus` | sm8650.dtsi:6086-6110 |
| window `core` | `0x0c400000`, `0x3000` | same | sm8650.dtsi:6088 |
| window `chnls` | `0x0c500000`, `0x400000` | same | sm8650.dtsi:6089 |
| window `obsrvr` | `0x0c440000`, `0x80000` | same | sm8650.dtsi:6090 |
| window `intr` | `0x0c4c0000`, `0x20000` | same | sm8650.dtsi:6091 |
| window `cnfg` | `0x0c42d000`, `0x4000` | same | sm8650.dtsi:6092 |
| execution environment | `qcom,ee = <0>`, `qcom,channel = <0>`, `qcom,bus-id = <0>` | same | sm8650.dtsi:6102-6104 |
| PMK8550 | `pmic@0`, `reg = <0x0 SPMI_USID>` → SID 0 | `pmk8550` | pmk8550.dtsi:22-24 |
| RTC | `rtc@6100`, `"qcom,pmk8350-rtc"`, `reg = <0x6100>, <0x6200>`, irq `<0x0 0x62 0x1>` | `pmk8550_rtc` | pmk8550.dtsi:47-52 |
| reboot reason | `reboot-reason@48`, `reg = <0x48 0x1>`, `bits = <1 7>` | SDAM2 `nvram@7100` | pmk8550.dtsi:54-65 |
| reboot modes | only `mode-recovery = <0x01>`, `mode-bootloader = <0x02>` | `reboot-mode` | pmk8550.dtsi:12-18 |
| PMK8550 PWM | `status = "disabled"` | `pmk8550_pwm` | pmk8550.dtsi:77-85 |
| panel backlight IC | `backlight@2e`, `"silergy,sy7758-master"` | `&i2c6` | sm8650-lenovo-tb710fu.dts:745-750 |
| second SY7758 | `backlight@2e`, `"silergy,sy7758-salve"` | `&i2c2` | sm8650-lenovo-tb710fu.dts:715-720 |
| panel | `"lenovo,nt36532-wqhd"`, `backlight = <&backlight>`, `avdd-supply = <&aw37504>` | `&i2c6` | sm8650-lenovo-tb710fu.dts:792-803 |
| panel AVDD | `aw37504` at `0x3e` on `&i2c6`, reset GPIOs tlmm 90/91 | `regulator@3e` | sm8650-lenovo-tb710fu.dts:730-742 |
| panel reset | `reset-gpios = <&tlmm 133 GPIO_ACTIVE_LOW>` | panel | sm8650-lenovo-tb710fu.dts:802 |
| SE6 bus | `i2c@a98000`, `"qcom,geni-i2c"`, `reg = <0x00a98000 0x4000>`, `GIC_SPI 363`, `GCC_QUPV3_WRAP1_S6_CLK` | `i2c6` | sm8650.dtsi:3360-3379 |
| TSENS 0 | TM `0x0c228000`, SROT `0x0c222000`, `#qcom,sensors = <15>`, GIC_SPI 506/640 | `tsens0` | sm8650.dtsi:6022-6035 |
| TSENS 1 | TM `0x0c229000`, SROT `0x0c223000`, `#qcom,sensors = <16>`, GIC_SPI 507/641 | `tsens1` | sm8650.dtsi:6037-6050 |
| TSENS 2 | TM `0x0c22a000`, SROT `0x0c224000`, `#qcom,sensors = <13>`, GIC_SPI 508/642 | `tsens2` | sm8650.dtsi:6052-6065 |

Two facts matter because they are *absences*:

- the panel node declares **no** `qcom,mdss-dsi-bl-pmic-control-type`, so the
  display's DCS brightness command (0x51) is not the control path, and neither is
  the PMIC's WLED block;
- `pmk8550.dtsi` **does not declare an `rtc-offset@bc` cell**. The sheng board's
  tree has one; this board does not. The clock therefore runs from the counter
  alone and the store path stays behind `tb710fu-rtc-store=1`.

## 2. How the panel is actually lit

The chain that decides it, in order:

1. the panel node has `backlight = <&backlight>`, so brightness belongs to that
   phandle (sm8650-lenovo-tb710fu.dts:803);
2. that phandle is `backlight@2e` on `&i2c6`, compatible
   `"silergy,sy7758-master"` — an I2C backlight controller, not a PMIC and not a
   PWM channel;
3. `pmk8550_pwm` is `status = "disabled"`, so there is no PMIC PWM to drive;
4. no `qcom,mdss-dsi-bl-pmic-control-type`, so nothing routes brightness through
   the DSI command channel.

`&i2c6` is `i2c@a98000` with a `0x4000` window and GIC_SPI 363; the driver maps
that window, runs the GENI I2C firmware the boot loader left loaded, and talks to
address `0x2e`.

### SY7758 registers

The devicetree gives no register map, so the map is the part's own, from
mainline `drivers/video/backlight/sy7758.c` (Kancy Joe, Neil Armstrong; v3, May
2026), which drives the same part on the SM8650 Ayaneo Pocket S2 — also at `0x2e`
on that board.

| Register | Offset | Use here |
| --- | --- | --- |
| `REG_BRT_8BIT` | `0x00` | not written |
| `REG_DEV_CTL` | `0x01` | read only: bits 2:1 select 8-bit (1) or 12-bit (2) brightness |
| `REG_STATUS` | `0x02` | not used |
| `REG_DEV_ID` | `0x03` | read: `0x63` identifies the part |
| `REG_DIRECT_CTL` | `0x04` | not used |
| `REG_STATUS2` | `0x05` | not used |
| `REG_BRT_12BIT_L` | `0x10` | written: level bits 7:0 |
| `REG_BRT_12BIT_H` | `0x11` | written: level bits 11:8 in the low nibble |
| `REG_LED_ENABLE` | `0x16` | not written |
| OTP `0x98`..`0xAE` | — | not written |

`max_brightness` in the mainline driver is `4080` (`0xFF0`), which is the level
range this package publishes and clamps to.

Two limits are deliberate and logged:

- **only the two brightness bytes are ever written.** `DEV_CTL`, `LED_ENABLE` and
  the OTP trim registers are read but never rewritten: the vendor's
  master/salve split (`-master` here, `-salve` on i2c2) is not the mainline
  part's single-chip init, and a wrong mode or channel mask leaves the panel dark
  with no console to report it.
- **a 12-bit write only happens when `DEV_CTL` already selects 12-bit mode.** If
  it does not, the service publishes what it read, says the mode is not 12-bit,
  and stays out of the write path instead of pretending to dim.

### The published service

The class is deliberately not named `AppleARMBacklight`: `AppleARMPlatform`
already defines that class and a second registration panics. Instead
`getMetaClass()` returns the live `AppleARMBacklight` metaclass found with
`OSMetaClass::getMetaClassWithName()` (arm64e: its vtable entries are signed, so
nothing is copied), which makes the registry's `IOClass` answer, `IOObjectConformsTo`
and backboardd's sandbox accept the service. Published properties:

- `IODisplayParameters` → `brightness` `{min, max, value}`;
- `BrightnessMilliNits` `{min, max, value}`;
- `backlight-marketing-table` (three 16.16 points), `LmaxProduct`/`LmidProduct`/`LminProduct`;
- `backlight-calibration-parameters` (`lMin`/`lMid`/`lMax`, product and panel);
- `backlight-control = true` and `NativeSandboxGrantProcess = "backboardd"`, set
  only when the service answers `IOObjectConformsTo` for `AppleARMBacklight`;
- `tb710fu-backlight-stats` (a 14-entry diagnostic array).

`setProperties` accepts `brightness` (with `FadePeriod`, 16.16 seconds) and
`brightness-nits` (signed 16.16) and only stores an atomic target plus a timeout;
the transfer is advanced by the work loop polling the engine's `M_IRQ` status
every millisecond, with a 25 ms per-step budget, and a newer request replaces one
not yet written. `runAction` is never called from `setProperties`: it blocks on
the work loop's gate and stalls backboardd's brightness thread.

The nits scale `2 / 140 / 500` is a slider placement, **not a measurement**. No
photometer has been on this panel; the numbers mirror the sheng board's and are
overridable from the boot log once measured.

## 3. The clock: SPMI v7 and the PMK8550 RTC

The transport is the arbiter's v7 protocol: `core` holds the version and the APID
map, `cnfg` the 1024 owner words, `obsrvr` the per-EE/per-APID observer, `chnls`
the 0x1000-per-APID channel windows. Transfers are synchronous: build the command,
wait for `DONE`, read the response, map the status to a named result. Writes are
whitelisted by APID and refused unless the owner word names this execution
environment (`qcom,ee = <0>`).

The clock lives at SID 0 / PPID `0x061` (the high byte of `0x6100` is the PPID).
Register layout is `pmk8350_regs` from `drivers/rtc/rtc-pm8xxx.c:440-446`:

| Offset in PPID 0x061 | Meaning |
| --- | --- |
| `0x46` | control, bit 7 = counter running |
| `0x40` | write base (four bytes) |
| `0x48` | counter read (four bytes) |
| `0x6200` block | alarm: `0x46` control, `0x48` control2, `0x40` data — untouched |

Reads follow `rtc-pm8xxx.c:121-149`: the counter is not latched, so the four bytes
are read and the LSB is re-read; a smaller second LSB means a carry straddled the
transfer and the whole read repeats. Wall time is `counter + stored offset`
(`rtc-pm8xxx.c:260-275`). With no `0xbc` cell on this board, the offset is treated
as zero (what `rtc-pm8xxx.c` does when the cell is absent) and the counter alone is
the clock. A nonzero cell that does not describe a plausible time is refused
rather than "repaired", because ownership of that cell on this board is unverified.

There is no sub-second register. The fraction comes from XNU's
`mach_absolute_time()`, anchored at the start of a read, and is reported invalid
when no fine source exists. The seconds field is the only field taken from
hardware.

Published as `IORTCController::getRealTimeClock()`/`setRealTimeClock()` with this
port's eight-byte blob (`Tb710fuRtcTime.h`), and `IORTC::getGMTTimeOfDay()`,
`setGMTTimeOfDay()`, `getUTCTimeOfDay()`, `setUTCTimeOfDay()` on top, plus
`IOService::publishResource("IORTC", …)` so `IOKitInitializeTime`'s thirty-second
wait is answered when the service exists.

The platform-expert path is `armdt_platform_clock_register(read, set)` (UniXNU's
`ArmDTPlatform` hook), declared `weak_import`: a kernel whose `ArmDTPlatform` does
not carry the hook links, logs that the hook is absent, and keeps the resource.

**Timing limit.** This archive is reached from `native_tb710fu_services_start()`,
which runs from `bsd_init`'s storage hook. If the boot calendar was already
initialised by then, the hook and the resource serve later readers and the next
initialisation, not this boot's `kern.boottime`. Moving the registration ahead of
`IOPlatformExpert`'s time initialisation is a later rung, not part of this change.

## 4. Reset, shutdown and EDL

`PE_halt_restart` is the platform's entry (`IOKit/IOPlatformExpert.h`;
`kPEHaltCPU` powers off and `kPERestartCPU` restarts, per `osfmk/i386/acpi.c`).
Both map onto PSCI through SMC #0, the conduit the board's `psci` node selects:

| Action | Method | Effect |
| --- | --- | --- |
| off | `SYSTEM_OFF 0x84000008` | powers the PMIC down |
| restart | `SYSTEM_RESET 0x84000009` | warm reset |
| recovery | SDAM2 cell = `0x01`, then restart | boot loader reads the cell |
| bootloader | SDAM2 cell = `0x02`, then restart | boot loader reads the cell |
| EDL | `SYSTEM_RESET2 (0xC4000012)` vendor type | enters 9008 |

The SDAM2 cell is `reboot-reason@48`, field `bits = <1 7>`: the raw byte is
`(previous & ~0xfe) | (mode << 1)`. `pmk8550.dtsi` declares only
`mode-recovery = 0x01` and `mode-bootloader = 0x02` — **there is no EDL mode
value**, so EDL does not write a guessed constant into that cell.

EDL uses the PSCI vendor reset type instead, which is recorded as verified on this
board, not inferred:

- `package/Tb710fuUsbCdc/CdcXnu.c:669-809` documents the live test:
  `SYSTEM_RESET2 (0xC4000012)`, `reset_type = 0x80000000`, `cookie = 1`
  (`PSCI_1_1_RESET_TYPE_VENDOR_START`), then `0x80000001, 1`, then plain
  `SYSTEM_RESET 0x84000009`;
- `CURRENT_STATUS.md:146` records "COM5 -> edl (TWRP 同款 PSCI
  SYSTEM_RESET2(0x80000000, 1)) 实机 3 秒直进 9008".

The vendor tree's `mode-edl = <0x80000000 0x1>` is the same pair, which is why the
ladder tries that type first. The `0x01FD9000` TCSR path is not used: nothing in
this board's tree or the verified log selects it.

One exported helper covers every transition:
`tb710fu_platform_power_action(0..4)` — `0` off, `1` restart, `2` recovery,
`3` bootloader, `4` EDL. It returns the arbiter result and the SMC result, and
prints one line naming the stage, the cell result, the SMC function, the reset
type, the cookie and the EDL vendor flag. Power-off and plain restart need no
SPMI access, so a failed arbiter mapping does not refuse them.

## 5. Thermal

Six windows, `0x1000` each, from the three `thermal-sensor@` nodes above. The
status word of sensor *n* is at `0xa0 + 4*n` in the TM window: bits 11:0 are the
temperature in tenths of a degree Celsius, two's complement; bit 21 is valid;
bits 20:16 are the five threshold flags. The SROT window holds the version at
`0x0` and the control word at `0x4`, bit 0 the block enable and bits 18:3 one
enable per sensor (`drivers/thermal/qcom/tsens-v2.c`, `tsens.c`).

A reading takes up to three status reads: the first valid word wins; with nothing
valid, two consecutive reads that agree are the block's last conversion; with
neither, the sensor has no reading. Values outside -40.0..119.9 °C are treated as
not a temperature.

The one write in this file is the SROT control word, and only when the block is
found disabled, because without it every status word reads invalid. Nothing is
written when the block is already on.

Grouping follows `sm8650.dtsi`'s `thermal-zones` table, and two of its properties
are worth stating because they are easy to misread:

- the CPU sensors follow the cpu nodes' `qcom,freq-domain` values, which are
  `0` (cpu0-1), `3` (cpu2-4), `1` (cpu5-6) and `2` (cpu7) — not numeric order.
  So tsens1's sensors 4/5 are cpu0/cpu1, tsens0's 5..10 are cpu2..cpu4 and
  11..14 are cpu5/cpu6, and cpu7's three sensors are tsens1's 1..3;
- tsens2's sensors 6..11 are named twice (Soc zones and the nsphvx/nsphmx zones).
  `#qcom,sensors = <13>` bounds the controller and the Soc names (gpuss0..7 at
  1..8, modem0..3 at 9..12) are the ones used; the duplicates are not. Every raw
  sensor is printed anyway, so nothing is hidden by that choice.

Published: `temperatures-mc` (3 × 16 little-endian int32), `groups-mc` (one
OSNumber per group that has a reading), `hottest-mc`, and
`tb710fu-thermal-stats`. `setProperties{"sample"}` re-samples once; there is no
periodic timer.

## 6. Boot-argument ladders

Nothing is mapped, published or installed unless the boot asks. Levels above the
maximum are clamped and reported; a value that does not parse leaves the service
off. The parser is plain C (`Tb710fuPlatformStage.c`) so the host test proves the
mapping for every argument the kernel sees, and each driver repeats the gate at
its own entry point so a direct call cannot map a window the boot did not ask for.

| Argument | Level | Behaviour |
| --- | --- | --- |
| `platform-svc` | 0 (default) | nothing mapped; `PE_halt_restart` keeps the platform's own value |
| | 1 | map the four SPMI windows, probe the v7 arbiter, read the counter and the SDAM2 cell, print; publish and write nothing |
| | 2 | level 1 plus `IORTC`, the `ArmDTPlatform` clock hook and `PE_halt_restart` |
| `backlight` | 0 (default) | nothing mapped |
| | 1 | map SE6, check the GENI firmware, read `DEV_ID`/`DEV_CTL`/level, print |
| | 2 | the published service; writes the 12-bit brightness bytes |
| `thermal` | 0 (default) | no TSENS window mapped |
| | 1 | map six windows, enable the block only if disabled, sample once, publish |

`platform-svc=2` alone does not start the backlight or thermal: those have their
own arguments. `tb710fu-rtc-store=1` additionally enables the offset-cell write
path, which stays off by default because the `0xbc` cell is undeclared here.

## 7. Verification

```
# the C suites: SPMI v7 framing, the clock against a modelled arbiter,
# epoch/civil and the blob, PSCI/reboot-reason including EDL, and the
# stage/SY7758/TSENS arithmetic. Prefer gcc or clang; ASan+UBSan on.
python3 package/Tb710fuPlatformServices/Test/run_host_tests.py

# the suites plus the kernel-source audit and the devicetree evidence check;
# writes build/tb710fu-platform-host/report.json
python3 scripts/check-tb710fu-platform-host.py
```

Recorded result on 2026-11-27 (WSL Debian, gcc 14.2, Python 3.13):

- `platform_spmi_test`: PASS
- `platform_rtc_test`: PASS
- `platform_rtc_time_test`: PASS
- `platform_reset_test`: PASS (includes `ok EDL ladder and the action path`)
- `platform_drivers_test`: PASS
- kernel-source audit: `Tb710fuBacklight.cpp`, `Tb710fuThermal.cpp`,
  `Tb710fuPlatformServicesXnu.cpp` — ok

The audit fails on `memcpy`/`memset`/`bzero`/`memmove` in a driver, on `++`/`--`
through a register pointer (clang's `-Wdeprecated-volatile`), on an
integer-to-function-pointer cast without `ptrauth_sign_unauthenticated`, on
`getPlatform()` without `<IOKit/IOPlatformExpert.h>`, on any SM8550 sensor window
(`0x0c271000`..`0x0c273000`), and on a unit missing its `#ifdef KERNEL` or its
ladder.

## 8. Merge wiring (not done in this change)

1. `configs/tb710fu_macos_defconfig`:
   - `CONFIG_XNU_EXTRA_LD_ARCHIVES` +=
     `-Wl,-force_load,{tree}/build/tb710fu-platform/libTb710fuPlatformServices.a`
   - `CONFIG_XNU_EXTRA_CFLAGS` += `-DXNU_TB710FU_PLATFORM=1`
2. `package/Macos27Services.cpp`, inside `native_tb710fu_services_start()`:
   - declare, weak:
     `extern "C" void tb710fu_platform_meta_all(void) __attribute__((weak_import));`
     and `extern "C" void tb710fu_platform_services_start(void) __attribute__((weak_import));`
   - call `tb710fu_platform_meta_all()` in the metaclass window, next to the other
     `*_meta_init()` calls, i.e. after `tb710fu_gpu_power_meta_init()` and before
     `OSMetaClass::postModLoad(handle)`;
   - call `tb710fu_platform_services_start()` after the `IOResources` check
     (`IOService::getResourceService()` only answers from there) — e.g. right
     after the `if (!resources)` block, or next to the battery start.
   - The archive's own metaclass constructor is `__GLOBAL__sub_I_Tb710fuPlatformServicesXnu.cpp`;
     `tb710fu_platform_meta_all()` calls all three deferred constructors by name,
     which is exactly why `scripts/build-tb710fu-platform.py` renames their
     `__mod_init_func` sections and exports the symbols.
3. `porting.toml`: add one area, e.g.

   ```toml
   [[area]]
   id = "platform-services"
   status = "host-verified"
   components = ["Tb710fuPlatformServices"]
   paths = ["package/Tb710fuPlatformServices/**", "scripts/build-tb710fu-platform.py",
            "scripts/check-tb710fu-platform-host.py", "docs/PLATFORM_BACKLIGHT_RTC.md"]
   ```

   `package/Kit.toml` only carries the kit identity (`schema`/`[kit]`) and needs no
   change; archives are selected by the defconfig, not by Kit.toml.
4. Build: `scripts/build-tb710fu-package.py` **does not exist in this tree**. The
   archive is built by `scripts/build-tb710fu-platform.py`, invoked from the macOS
   job with `UNIXNU_DEVICE` set and the resolved tree as the working directory,
   the same way the other per-subsystem archives are built. That needs one new
   step in `.github/workflows/macos-kernel.yml` (off-limits here) plus copying
   `build/tb710fu-platform/build-report.json` into the job summary and artifacts.
   The script refuses to run without a materialised XNU and the pinned framework
   revision, and it invalidates the XNU link target when the archive changes.

## 9. Known limits and the next rungs

- No GCC clock vote for the SE6 engine: no SM8650 QUP clock register offset is
  available from the pinned sources, so `geni_i2c_prepare()` (firmware revision and
  timing registers) is the liveness gate instead. If the boot loader left the
  engine unclocked, the probe fails and says so.
- 8-bit brightness mode is not written; only 12-bit, and only when `DEV_CTL`
  already selects it.
- No `SMC` dispatcher other than the PSCI calls above; no thermal-pressure
  channel, no trip handling and no throttling.
- The nits scale is a declared slider placement, not a measurement.
- The `0xbc` RTC offset cell is undeclared on this board and unverified; the store
  path is opt-in and guarded.
- The clock hook is registered from the storage hook, which may be after this
  boot's calendar initialisation (see section 3).
- Interrupt paths are deliberately absent: the display backlight and the thermal
  monitor both poll, because this port has no verified SPI attach symbol.
