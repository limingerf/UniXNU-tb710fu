# WCN7850 Wi-Fi on TB710FU (SM8650Q-AB, macOS 27.3)

`package/Tb710fuWifi` brings up the board's Qualcomm WCN7850 (PCI vendor/device `0x17cb:0x1107`) as a staged driver,
default off. This document records the exact hardware facts, what each stage really does, where the firmware comes
from, and the small surface the final merge has to touch. It is deliberately honest about the boundary: stages 1 and 2
are implemented end to end; stages 3-5 exist as scaffolding plus a dormant `IOEthernetController`, not as a working
association/data path.

## Hardware facts and their provenance

Every address below is read from the TB710FU kernel sources, not from an SM8550 reference:

| Item | Value | Source node |
| --- | --- | --- |
| PCIe0 PARF | `0x01c00000` + `0x3000` | `sm8650.dtsi` `pcie@1c00000`, compatible `"qcom,pcie-sm8650","qcom,pcie-sm8550"` |
| PCIe0 DBI / ELBI / ATU / config | `0x60000000` / `0x60000f20` / `0x60001000` / `0x60100000` (+`0x100000`) | same node, `reg` names |
| PCIe0 MMIO window | `0x60300000` + `0x3d00000` | same node, `ranges` |
| PCIe0 PHY | `0x01c06000` + `0x2000`, `"qcom,sm8650-qmp-gen3x2-pcie-phy"` | `sm8650.dtsi` `phy@1c06000` |
| Root port bus range | `0x01..0xff`; endpoint `wifi@0` at bus 1 dev 0 fn 0 | `sm8650-lenovo-tb710fu.dts` `&pcieport0`, `wifi@0` |
| PERST# / WAKE / WLAN_EN / BT_EN | TLMM 94 (active low) / 96 / 16 / 17 | board `&pcie0` `perst-gpios`,`wake-gpios`; `wcn7850-pmu` `wlan-enable-gpios`,`bt-enable-gpios` |
| CLKREQ | TLMM 95, function `pcie0_clk_req_n` | `sm8650.dtsi` `pcie0_default_state` |
| TLMM | `0x0f100000` + `0x300000`, stride `0x1000` | `sm8650.dtsi` `pinctrl@f100000` |
| GCC | `0x00100000` + `0x1f4200`, `"qcom,sm8650-gcc"` | `sm8650.dtsi` `clock-controller@100000` |
| TCSR | `0x01fc0000` + `0xa0000`; `PCIE_0_CLKREF_EN` at `+0x31100` bit 0 | `sm8650.dtsi` `tcsr@1fc0000`; `tcsrcc-sm8650.c` |
| apps-SMMU / ITS | `0x15000000` + `0x100000` / `0x17140000` + `0x40000` | `sm8650.dtsi` `iommu@15000000`, `msi-controller@17140000` |
| PCIe stream IDs | bus 0 -> `0x1400`, bus 1 -> `0x1401` | `iommu-map`/`msi-map` in `pcie@1c00000` |
| MSI SPI | 141..148 (msi0..7), 140 global; INTx 149..152 | `interrupts` / `interrupt-map` |
| Module rails | `vdd=vreg_s4i_0p3`, `vddio=vreg_l15b_1p8`, `vddio1p2=vreg_l3c_1p2`, `vddaon=vreg_s2c_0p5`, `vdddig=vreg_s3c_0p9`, `vddrfa1p2=vreg_s1c_1p2`, `vddrfa1p8=vreg_s6c_1p8`, `clocks=<&rpmhcc RPMH_RF_CLK1>` | `wcn7850-pmu` |
| Endpoint rails | `vddrfacmn`, `vddaon`, `vddwlcx`, `vddwlmx`, `vddrfa0p8`, `vddrfa1p2`, `vddrfa1p8`, `vddpcie0p9`, `vddpcie1p8` | `wifi@0` |
| PHY rails | `vdda-phy=vreg_l1i_0p88`, `vdda-pll=vreg_l3i_1p2` | `&pcie0_phy` |

Register field definitions that a DTS cannot express come from pinned upstream Linux (`gcc-sm8650.c`, `gdsc.c`,
`pcie-qcom.c`, `phy-qcom-qmp-pcie.c`, `ath12k/{mhi,htc,wmi}.h`) and are documented at each constant in
`Tb710fuWifiRegs.h`. The CRC-8 SID hash, PARF ordering and MHI boot phases follow the hardware-verified
`UniXNU-sheng` implementation for the same PCIe/MHI IP (BSD-2-Clause-Patent); no SM8550 address is copied.

## Board power and PCIe bring-up (stage 1)

`Tb710fuWifiPcie.cpp` performs, in this order:

1. TLMM pads: PERST# asserted, WLAN_EN low, BT_EN low, WAKE input; 5 ms, WLAN_EN high, 150 ms (covers the 100 ms
   PERST# minimum).
2. GCC: clear PCIe0/PHY collapse bits (`0x5214c` bits 0/3), wait for `GDSC_POWER_UP_COMPLETE` (bit 16 of `+4`) on
   both GDSCs (`0x6b004`, `0x6c000`).
3. AUX clock source `0x6b074` (TCXO/1) and rchng `0x6b058` (GPLL0_EVEN/3 = 100 MHz), pipe mux `0x6b070`,
   VOTE8 `0x52008` = `0x7f`, VOTE0 `0x52000` bits 12/19/22. Branch halt must clear for slv/mstr/cfg/aux.
4. TCSR `PCIE_0_CLKREF_EN` (`0x31100` bit 0), then pulse PCIe BCR `0x6b000` and PHY BCR `0x6c01c`.
5. PARF: device type 4 (RC), DBI base `0x60000000`, ATU base `0x60001000`, slave space, MHI AHB clock enable,
   `SYS_CTRL` master CGC disable clear (bit 29), AXI master halt.
6. PHY tables (v5 offsets: serdes `0x0`, pcs `0x200`, pcs_misc `0x600`, tx `0xe00`, rx `0x1000`, lane 2 at `+0x800`),
   pipe mux to the PHY, out of reset, start, wait for PHYSTATUS clear (`0x214` bit 6).
7. PARF SID hash at `0x2000` (256 words, CRC-8 poly 7 over BDF high byte first); bus 1 dev 0 hashes in as
   `0x100 << 16 | 1 << 8`, bypass bit `0x2c00` bit 0 cleared. DBI bus numbers `0x00010100`, memory decode + master.
8. PERST# high, 1 ms, `PARF_LTSSM` bit 8, poll `PORT_DEBUG0[5:0] == 0x11` and `PORT_DEBUG1` bit 4 for up to 1 s.
9. Outbound iATU region 0: 1 MiB config window at `0x60100000` targets bus 1 (`cfg0`, target `bus << 24`); region 1
   identity-maps the DTS MMIO window. Read `0x17cb:1107`, size BAR0, place it in the MMIO window, enable memory +
   bus master, map it.
10. WCN chip power-up through the BAR window (`0x310c = 0x40000000 | offset[24:19]`, register at
    `0x80000 + offset[18:0]`, MHI block at BAR+0): LTSSM reset word `0x1e081b0`, hot reset `0x1e40304`, stale IRQ
    clear `0x1e08228`, warm-boot control `0x1f8031c`, BHIe doorbells `0x360/0x368/0x394/0x39c`, warm entry
    `0x1f80504 = 0`, reset cause, global reset `0x3008`.

All MMIO goes through aligned 32-bit volatile helpers (`regRead`/`regWrite`) plus `dsb sy`; there is no
`memcpy`/`bzero`/`memset` on a mapping, and no `volatile++`. The only raw-address function pointer
(`IOEthernetController`'s C2) is `ptrauth_sign_unauthenticated(..., ptrauth_key_function_pointer, 0)`.

## Firmware and MHI (stage 2)

Files, from `.local/input-sources/firmware/tb710fu-firmware-20261005.tar.gz` (`lib/firmware/ath12k/WCN7850/hw2.0/`):

| File | Stock size | Role | Check |
| --- | --- | --- | --- |
| `m3.bin` | 299,660 B | M3/secondary boot loader, through BHI in one DMA buffer | ELF32 LE, `e_machine=0x28` |
| `amss.bin` | 6,090,816 B | full firmware, through BHIe as 512 KiB segments | ELF32 LE, `e_machine=0x28` |
| `board-2.bin` | 2,253,964 B | board container, later handed over by QMI (not booted via MHI) | `"QCA-ATH12K-BOARD\0"` |

Search order: boot addon `EFI/UniXNU/firmware/Wifi/<name>` first, then `wifi-fw=<dir>` or
`/private/var/firmware/ath12k/WCN7850/hw2.0`. Sizes are range-checked before allocation (amss 3-16 MiB, m3
150 KiB-4 MiB, board 64 KiB-8 MiB), so the community 7,458,880 B amss and 113 KiB board file also pass.

`Tb710fuWifiMhi.c` is an MMIO-free phase machine: BHI loads the SBL and waits for READY; the caller programs the 128
channel contexts, two event and one command context, plus their 16-byte-element rings; M0 is requested; once the
device reports SBL the BHIe vector table (address/size per 512 KiB segment) starts the full image; mission mode is
EE=AMSS/WFW. `Tb710fuWifiDriver.cpp` owns one contiguous DMA buffer for all of that, one for the m3 copy, one for
amss and one for the table, cleans them to PoC with `dc civac`, and programs `CCABAP/ECABAP/CRCBAP`, control/data
bases and `MHICFG.NER`.

DMA addresses are physical addresses. If apps-SMMU translation is on, stage 2 refuses to proceed unless the owner
names a context bank with `wifi-smmu-bank=<n>`; the bank is planned against the live SMMU geometry and every write is
read back (stage 2 has no way to invent an IOVA). S2CR.TYPE=BYPASS is never used: the SM8650 firmware SMMU reads it
back as FAULT (measured by `Tb710fuUsbCdc`), so the identity route is a disabled VA64 stage-1 bank
(`SCTLR=0`, `CBAR=0x10000`, `CBA2R=1`).

The MHI rings and firmware DMA are intentionally not freed after mission mode: the device owns those addresses for
the rest of the boot. Only host copies of the images are released.

## Stages 3-5: exactly what is and is not there

`wifi=0..5` (default 0; parsed with `PE_parse_boot_argn`):

| Level | What runs | Expected result today |
| --- | --- | --- |
| 1 | Power, PCIe link/PHY, endpoint ID, BAR0, chip power-up | should reach link L0 and read `17cb:1107` |
| 2 | Firmware manifest, DMA, BHI/BHIe, MHI contexts, mission mode | needs firmware installed and the SMMU question settled |
| 3 | QRTR/QMI plan + encoders | returns `WIFI_ERR_PROTO`: the MHI pipe assignment the firmware publishes is not wired |
| 4 | HTC/WMI | same gate as 3; `HTC_SVC_ID_WMI_CONTROL` and frame/TLV builders are host-tested |
| 5 | Attach dormant `Tb710fuWifiNet` | registers an `IOEthernetController`; link stays down |

The protocol builders (`Tb710fuWifiProto.c`) are complete for framing: QRTR v1 32-byte header, QMI 6-byte header +
3-byte TLVs, HTC 8-byte frame + connect/ready words, WMI 4-byte command header + TLV word, DP ring geometry. The
ath12k HAL descriptors for the DP rings are deliberately not invented; that belongs to the data-path milestone.

`Tb710fuWifiNet.cpp` creates its object from the live 26C5035r `IOEthernetController` metaclass/vtable, re-signing
all 400 slots and overriding 11 of them (metaclass, start/stop, outputPacket, features, enable/disable, hardware
address, multicast/promisc). It carries no IONetworkingFamily link symbol and no SDK networking header.

## Firmware installation

Two supported placements (either is enough):

- Boot addon: add `EFI/UniXNU/firmware/Wifi/amss.bin`, `m3.bin`, `board-2.bin` through the existing
  `scripts/addon-add-dir.py` before the payload is built. The container magic stays `NGPUADD2`; this subsystem carries
  its own reader so it does not depend on `Tb710fuAdsp`'s archive.
- Data volume: copy the three files to `/private/var/firmware/ath12k/WCN7850/hw2.0/` (the Linux port's layout), or
  pass `wifi-fw=/some/dir` to point elsewhere.

## Merge wiring (only these touch points)

1. `scripts/build-tb710fu-package.py`: run `scripts/build-tb710fu-wifi.py` when the Wi-Fi component is selected, and
   add `build/tb710fu-wifi/libTb710fuWifi.a` to the kernel link inputs. The archive already invalidates the XNU link
   target on change.
2. `package/Kit.toml`: add the `Tb710fuWifi` component (sources/headers listed in the build script), and define
   `XNU_TB710FU_WIFI=1` for the C++ files that need the package's kernel flags.
3. `package/Macos27Services.cpp`: add
   `extern "C" void tb710fu_wifi_probe(void) __attribute__((weak_import));` and call it next to
   `tb710fu_ethernet_probe` after `postModLoad`. Nothing else in that file changes.
4. Payload: optionally add the three firmware files to the addon (see above). If they stay on the Data volume, no
   payload change is needed.
5. Boot args: `wifi=1..5`, `wifi-fw=<dir>`, `wifi-smmu-bank=<n>`. All default to off/unset.

No file under `package/Tb710fuGpu/`, `package/Tb710fuUsbCdc/`, `package/GopDisplay*` or `.github/workflows/` is
modified by this subsystem.

## Verification

- Host: `python3 scripts/check-tb710fu-wifi-host.py` compiles the portable C half with `clang`/`gcc`
  (`-std=c11 -Wall -Wextra -Werror`), checks every portable header is self-contained, and runs 106 checks covering
  TLMM words, BAR sizing/placement, iATU encodings, the SMMU plan/apply, MHI plan/contexts/BHIe/phase machine, the
  firmware manifest, and QRTR/QMI/HTC/WMI/DP encoding. Result: PASS with gcc 11 and clang 17.
- Syntax: the three C++ files (`Pcie`, `Driver`, `Net`) and the BSD firmware reader were additionally parsed by
  clang 17 `-Wall -Wextra` against stub IOKit/BSD headers, which caught one real `volatile uint32_t *` to `void *`
  conversion and a missing brace. This is not an SDK build: the kernel objects still have to be produced by the macOS
  runner command below.
- Kernel: `python3 scripts/build-tb710fu-wifi.py` on the macOS runner (needs the minimal XNU build records). It
  generates `Tb710fuWifiSlots.h` from `scripts/net-ether-26C5035r.json`, compiles the C files with the XNU/BSD records
  and the C++ files with the IOKit record, archives `libTb710fuWifi.a`, and writes `build/tb710fu-wifi/build-report.json`.
- Hardware: a successful native build is not a link, a link is not firmware boot, and mission mode is not Wi-Fi.
  Report the exact stage: `native_tb710fu_wifi_stats[0]` (last completed), `[1]` (error), `[5]` (MHI phase), `[6]`
  (EE), `[7]` (MHI state), plus the `native_tb710fu_wifi_text` line.
