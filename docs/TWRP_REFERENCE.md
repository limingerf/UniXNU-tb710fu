# TB710FU TWRP 设备树借鉴价值 · 2026-11-15

来源：https://github.com/morannlx/adnroid_device_TB710FU_recovery (branch twrp-16.0)

## 这个仓库是什么

TWRP recovery 的**设备树**（不是内核、不是 UEFI）。它配置的是在**完整 Linux
内核**上跑的 TWRP + fastbootd，USB/ADB 走的是内核的 DWC3 驱动 + configfs/
functionfs，**不是 UEFI 的 vendor USB 二进制**。

## 对三个目标的直接价值

### 1. 槽位切换（你的核心痛点）

- `device.mk` 继承 `virtual_ab_ota/compression_with_xor.mk` → **这是 Virtual A/B**
- `BoardConfig.mk` 的 `AB_OTA_PARTITIONS` 列出全部 A/B 分区
- **关键事实**：`ro.boot.slot_suffix` 是运行时参数；VAB 的槽位状态在
  `super` 分区的 metadata 里，**不是** devinfo 的简单字节
- 结论：UEFI 里直接写 devinfo 切槽的路，对 VAB 设备**方向可能错了**。
  真正的槽位切换由 ABL 读 super metadata + bootloader 状态决定

### 2. USB / adb / fastboot

`init.recovery.usb.rc` 给了确凿的硬件事实：

| 项 | 值 |
| --- | --- |
| DWC3 控制器 | `a600000.ssusb`（与 live FDT `ssusb@a600000` 一致） |
| fastboot VID/PID | `0x18D1 / 0xD00D` |
| adb VID/PID | `0x18D1 / 0x4E11` |
| MTP VID/PID | `0x2717 / 0xFF40` |
| USB 传输 | configfs + functionfs（内核态） |

**但这套是内核 USB**，无法直接搬到 UEFI。它只证明：这块板的 DWC3 在
Linux 下能起 USB device 模式——**硬件没问题，卡在 UEFI 缺 vendor USB 栈**。

### 3. EDL（9008）

`TW_HAS_EDL_MODE := true` 确认设备支持 EDL。这就是你每次进 9008 切槽的原因。

## 借鉴结论

| 目标 | 借鉴到的结论 |
| --- | --- |
| UEFI 切槽 | VAB 设备，槽位在 super metadata；devinfo 直接写**不是**正路。更可靠是重启到 fastbootd（TWRP 里 `TW_INCLUDE_FASTBOOTD`）让 ABL 处理 |
| UEFI USB | 硬件 DWC3 确认可用（`a600000`），但 UEFI 侧 vendor USB 链会拉回黑屏集 I2C |
| EDL 依赖 | 设备固件层面支持 EDL，这是切槽兜底 |

## 最现实的路径（综合之前所有分析）

1. **UEFI 菜单加"重启到 fastbootd"** —— 让 ABL 读 super metadata 切槽，
   你在 fastbootd 里 `--set-active=a`，**不用进 9008**
2. **内核日志**：ramoops（另一会话）+ 可能的 EUD
3. **UEFI USB fastboot**：作为后续，风险已明确（I2C 黑屏集）

## 需要进一步确认

- `misc` 分区的 BCB：TWRP 的 recovery.fstab 里 `misc` 是 emmc，但我们 dump 的
  misc.img 只有 `command=bootonce-bootloader`，没有 `bootloader_control`。
  VAB 设备的槽位状态可能在 `misc` 之外（`metadata` 或 super）。
