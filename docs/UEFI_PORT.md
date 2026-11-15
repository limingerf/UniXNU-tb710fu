# UEFI 侧端口能力：可行性核实 · 2026-11-15

## 用户的诉求

1. UEFI 里能有 USB 端口能力，回 UEFI 就能读日志（不用回 Android）
2. 菜单能跳 A 槽 Android / A 槽 TWRP（免去每次进 9008 切槽）

关于第 1 点用户的关键观察：**boot 分区 96 MB，固件才 3 MB**，还有大量空间
可以塞东西。这个判断是正确的，而且 EDK2 已备好相应组件。

## 已核实：EDK2 的 USB 设备与 fastboot 组件全部在位

| 组件 | 路径 | 作用 |
| --- | --- | --- |
| `AndroidFastbootApp` | `EmbeddedPkg/Application/AndroidFastboot/` | 实现 fastboot 协议（含 `getvar`/`flash`/`oem`） |
| `FastbootTransportUsbDxe` | `EmbeddedPkg/Drivers/AndroidFastbootTransportUsbDxe/` | 把 fastboot 跑在 USB device 上 |
| `FastbootTransportTcpDxe` | `EmbeddedPkg/Drivers/AndroidFastbootTransportTcpDxe/` | fastboot over TCP（备选） |
| 协议定义 | `EmbeddedPkg/Include/Protocol/AndroidFastbootTransport.h` | `gAndroidFastbootTransportProtocolGuid` |

### 依赖链是通的

`FastbootTransportUsbDxe.inf` 需要 `gUsbDeviceProtocolGuid`：

```
gUsbDeviceProtocolGuid = { 0x021bd2ca, 0x51d2, 0x11e3, {0x8e,0x56,0xb7,0x54,0x17,0xc7,0x0b,0x44} }
```

而本机固件里的高通 `UsbDeviceDxe` 正好安装这个协议——它的字符串里有
`Failed to install UsbDevice protocol: %r`、`UsbDeviceStart`、`UsbDeviceSend`，
配合 `UsbfnDwc3Dxe`（DWC3 控制器）与 `UsbInitDxe`/`UsbConfigDxe`。

**结论：把 `AndroidFastbootApp` + `FastbootTransportUsbDxe` 加进 DSC/FDF，
UEFI 就能被主机 `fastboot devices` 看到。**

fastboot VID/PID 的 PCD 默认值：

```
PcdAndroidFastbootUsbVendorId  = 0xf00d
PcdAndroidFastbootUsbProductId = 0xbeef
```

可以改成 Android 惯例的 `0x18d1/0x4ee0` 之类，便于主机识别。

## 这能带来什么

| 能力 | 说明 |
| --- | --- |
| **`fastboot devices` 在 UEFI 里可见** | 主机直接操作，不必回 Android |
| **`fastboot oem` 自定义命令** | 可挂"切槽""读日志""重启到 X 模式"等 |
| **`fastboot getvar`** | 暴露 UEFI 侧状态（内存、payload、内核信息） |
| **日志回传** | 用 `oem log` 或 `getvar` 把缓冲区内容带回主机，**这就是实时读日志的通道** |

### 关于 adb

EDK2 没有官方 adb 实现（adb 需要 USB 上的 ADB 接口 + 主机 adbd 协议）。
可行的是：

- **fastboot 通道**（现成，工作量小）
- 若要 `adb devices`：需要实现 ADB 的 USB 接口类与协议解析，属自写组件；
  但**同一个 USB device 栈可以被它复用**，所以先做 fastboot 是合理的台阶。

## 第 2 点：菜单切槽

### 现状

- `misc` 分区**没有** `bootloader_control` 结构（magic `0x42414342` 不存在），
  所以槽位不在 BCB 里。
- 槽位状态在 **`devinfo`** 分区（4 KiB），由 ABL/XBL 维护；
  `VerifiedBootDxe` 用 `read_partition`/`write_partition` 读写它
  （字符串：`VB: RWDeviceState: Succeed using devinfo!`）。
- `GblEfiBootControl.h` 定义了标准 `SetActiveSlot`，但**树里没有实现者**。

### 两条实现路径

**路径 1（低风险，推荐先做）：菜单重启到 fastbootd**

UEFI 菜单项 `action` 只支持 `reset`/`shutdown`/`exit`/`tools`，没有"重启到
fastboot"。但可以用 `ResetSystem` 带自定义参数，或写 `misc` 的 BCB
`command = "boot-fastboot"` 后复位——ABL 读到就进 fastbootd。
之后在主机 `fastboot --set-active=a`。

**路径 2（真正在 UEFI 内切槽）：写 `devinfo`**

需要：
1. UEFI 能读 UFS（`UFSDxe` → `BlockIo` → 按 GPT 名找 `devinfo`）
2. 知道 `devinfo` 的槽位字节布局（**未知，必须先从设备 dump 出来分析**）
3. 写回并复位

风险：`devinfo` 还存着验证状态/设备状态，写错可能让 ABL 判定设备异常。

## 建议顺序

1. **UEFI fastboot 通道**（`AndroidFastbootApp` + `FastbootTransportUsbDxe`）
   —— 依赖已齐、改动集中、收益最大（日志 + 后续切槽命令都能挂上去）
2. **`fastboot oem` 子命令**：读日志缓冲、重启到 fastbootd
3. **UEFI 写 devinfo 切槽**：先 dump `devinfo` 分析格式，再做

## 待确认

- `devinfo` 真实内容（设备当前不可达，需下次在 TWRP 里 dump 4 KiB）
- USB 口在 UEFI 下是否被 ABL 继续持有（若 ABL 仍掌握 PHY，需要先 `UsbInitDxe`
  重新初始化）
