# USB 设备模式依赖链：完整核实 · 2026-11-15

## 目标

进入 UEFI 后即处于 fastboot 状态（`fastboot devices` 可识别），不用选菜单项。

## 依赖链（逐层核实）

```
AndroidFastbootApp (EDK2 源码)
 └─ FastbootTransportUsbDxe (EDK2 源码)
     └─ gUsbDeviceProtocolGuid
         └─ UsbDeviceDxe (vendor)
             └─ UsbfnDwc3Dxe (vendor)
                 └─ PmicSmbchgProtocol->ChargerPort()   ← USB 充电口握手
             └─ UsbInitDxe (vendor)
             └─ UsbConfigDxe (vendor)
                 ├─ ClockProtocol        ← 时钟投票
                 └─ PMIC version protocol
```

PMIC 子链（`PmicDxe` 的字符串）：
```
PmicDxe → TLMM(GPIO) + I2C + Clock
QcomChargerDxe / ChargerExDxe → PMIC
```

## 关键结论：这比 UFS 链更深、更宽

| 维度 | UFS 链（已做过） | USB 设备链（本次） |
| --- | --- | --- |
| 核心 vendor 模块 | 8 个（Smem/HWIO/Clock/Rpmh/UFS 等） | 5 个 USB + PMIC/Charger/TLMM/I2C |
| 是否需要 PMIC | 否（UFS 只走时钟/RPMh） | **是**（USB 充电口握手必需） |
| 是否需要 I2C/TLMM | 否 | **是**（PMIC 依赖） |
| 是否触及 2026-11-11 黑屏集 | 否（已排除 Display/I2C/Gpi 等） | **是**（I2CDxe 在黑屏集里） |

## 风险

2026-11-11 的黑屏集（DALSYSDxe/VcsDxe/DisplayDxe/GpiDxe/SPIDxe/I2CDxe/SdccDxe/
PIL/Tz）当初正是因为会黑屏被排除。USB 设备链**必然拉回 I2CDxe**，以及可能
拉回 TLMM/Gpi。这会**重新引入当初排除它的理由**。

这不是说一定黑屏——USB 可能只需要 I2C 读充电口状态，Display 那条分支不走——
但风险是真实的，且需要一次实机验证才能确认。

## 方案分叉

### 方案 1：完整 vendor USB 设备链（原计划）

- 把 USB 5 件 + PMIC + Charger + TLMM + I2C 一起进 vendor 变体
- 风险：拉回 I2CDxe（黑屏集），可能黑屏
- 收益：真正的 `fastboot devices` over USB

### 方案 2：仅 EUD（高通调试串口，非 fastboot）

- 高通 EUD 通过 USB 暴露**调试串口**，主机装驱动后是 COM 口
- 依赖：EUD 模式切换，可能比完整 USB 设备栈轻（但要验证）
- 收益：能实时读日志（serial），但不是 fastboot 协议，不能 `flash`/`getvar`

### 方案 3：先做 fastboot 源码层 + TCP，USB 后置

- 把 `AndroidFastbootApp` + 平台协议编译进去，传输用 TCP
- 但 UEFI 没有网络栈（无 SNP），TCP 也不通 —— 等于空转
- 收益：几乎为零（除非同时加网络驱动）

### 方案 4：接受 vendor 链，但**最小化**——只拉 USB 必需的最小子集

- 逐个确认 I2CDxe 能否用更轻的替代（或确认 PMIC 充电口握手是否可跳过）
- 若 UsbfnDwc3Dxe 的 ChargerPort 只在"探测充电器类型"时用，可考虑跳过

## 建议

在动手写代码前，先确认一个事实：**UsbfnDwc3Dxe 的 PmicSmbchgProtocol 调用是
硬性还是软性**（是否 ChargerPort 失败就走 SDP 继续）。字符串里有
`PmicSmbchgProtocol->ChargerPort() failed ... Assume SDP`，**暗示这是软性**——
失败时假设标准下游端口，继续枚举。

如果是软性，我们可以**不拉完整 PMIC 链**，让 ChargerPort 失败并走 SDP 分支，
大幅缩小 vendor 依赖面。这值得先验证。

## 决定性发现（2026-11-15，字符串实测）

`ChargerPort` 确实是软性（失败走 SDP），**但 PMIC 链仍然躲不开**：

- `PmicDxe` 硬性依赖 **I2C**：字符串含 `i2c_init_bq25882`、`i2c_init_bq28z610`
  （充电芯片 bq25882/bq28z610 走 I2C）。
- `ChipInfoDxe` 依赖 **SMEM**（共享内存分区表）。
- `UsbConfigDxe` 依赖 `gEfiPlatformInfoProtocolGuid` + `DTBExtnProtocol`。

**而 `I2CDxe` 在 2026-11-11 的黑屏集里被明确排除**。完整 USB 设备栈会把它
重新拉回来，等于重走当时黑屏的老路。

### 结论

- `fastboot over USB` 需要的 vendor 链是：USB 5 件 + PMIC + I2C + TLMM +
  PlatformInfo + DTBExtn + ChipInfo + SMEM + Clock + Charger。
- 这条链**包含黑屏集的 I2CDxe**，且深度远超 UFS 链。
- 风险：很可能重新触发 2026-11-11 黑屏。

### 因此，重新排序方案

在投入这次可能黑屏的 vendor 集成前，更稳妥的次序是：

1. **EUD 调试串口**（非 fastboot，但能实时读日志）——依赖可能比完整 USB
   设备栈轻，且只用于日志，值得先验证它的 vendor 依赖面。
2. **ramoops**（另一会话在做）——内核日志的非实时兜底。
3. **fastboot over USB** —— 等 EUD/ramoops 先把"读日志"这个刚需满足后，
   再决定是否值得冒 I2C 黑屏风险。

这个排序的理由：**读日志是当前最痛的刚需，fastboot over USB 是"锦上添花"
但它带的 vendor 链有黑屏风险**。先用低风险手段拿到日志，比先冒险更划算。
