# 实时日志通道：本机可行方案 · 2026-11-15

## 需求

内核/加载器日志要能**实时**在 Windows 上读，不用每次回 Android 抓 pstore。

## 本机硬件事实（来自 live FDT 与固件树）

| 通道 | 本机状态 | 依据 |
| --- | --- | --- |
| 外部 UART 引脚 | **无**（平板无外接串口） | 机身无调试口 |
| **EUD（Embedded USB Debug）** | **可用**：`qcom,msm-eud@88e0000`，`status="ok"`，`reg = <0x88e0000 0x2000 0x88e2000 0x1000>` | live FDT:13680 |
| 固件里的 EUD 串口代码 | 存在：`usb_eud_serial`、`usb_eud_is_active` 字符串在 `UsbConfigDxe.efi` | strings 实测 |
| UEFI USB 设备栈 | 齐全：`UsbfnDwc3Dxe`/`UsbDeviceDxe`/`UsbConfigDxe`/`UsbInitDxe` | `Binaries/tb710fu/QcomPkg/Drivers/` |
| EDK2 USB CDC/网络类 | 齐全：`UsbCdcEcm`、`UsbCdcNcm`、`UsbRndis` | `Mu_Basecore/MdeModulePkg/Bus/Usb/UsbNetwork/` |
| 屏幕 DEBUG | 已在用：SimpleFbDxe 把 UEFI/UniXNU/DarwinBoot 输出画到面板 | 已见 `DARWINBOOT:` 行 |
| ramoops | 可用：`ramoops_region{reg=<0xac300000 0x100000>; console-size=<0x40000>}`，控制台区 `0xac340000` | live FDT |

## 四条路的真实成本

### 1. EUD 串口（最接近"插上就能看"）

高通 EUD 通过 USB 暴露一个调试串口，主机侧装 USB 驱动后即成一个 COM 口。

- **覆盖范围**：取决于谁在用 EUD。它出现在 `UsbConfigDxe` 里，说明 UEFI 侧已有胶水；
  XNU 侧**没有**任何 EUD 驱动。
- **成本**：把 `SerialPortDxe`（或 EUD 串口实例）加进 DSC/FDF，并确认 EUD 的
  USB 端点在 device mode 下被枚举。属固件配置级改动，比写驱动小。
- **限制**：即使通了，也只覆盖 firmware/loader；**内核日志仍然到不了**。

### 2. UEFI USB CDC-ACM（主机看到 COM 口，实时）

- **成本**：写一个 UEFI CDC-ACM 或复用 `UsbCdcEcm` + 网络栈，再把 DxeDebugLib
  绑到该通道。中等固件工作量。
- **限制**：同样只覆盖 firmware/loader。

### 3. XNU 侧 USB（内核日志实时）

- **成本**：移植 XNU 的 USB 栈 + CDC/ACM/ECM 驱动。与 UFS 同量级的独立驱动移植。
- **收益**：内核日志实时可读——这才是终极目标。

### 4. ramoops（非实时，但覆盖最早的 panic）

- **成本**：把参考移植的 `tb710fu-ramoops-console.patch`（457 行）按 macOS 27
  的 xnu-12377 重写。参考补丁**无法直接应用**（`arm_init.c`、
  `serial_console.c`、`conf/files` 三处冲突，已实测）。
- **收益**：内核从最早阶段起的 console 都能留下，重启后可读；不需要任何驱动。

## 关键判断

**当前阻塞的本质不是"没有日志"，而是"内核可能已经走了很远，我们不知道"。**

- firmware/loader 段：**面板 DEBUG 已经够用**（我们靠它定位了 FAT 几何、
  pool 断言、LZMA 解压）。
- kernel 段：面板和 EUD/CDC 都帮不上，除非内核自己有 console 驱动。

所以若要继续往内核里推进（平台专家、根卷、UFS），**必须二选一**：

| 方案 | 时间到第一份内核日志 | 后续便利性 |
| --- | --- | --- |
| ramoops 补丁移植 | 中等（一次 457 行级移植） | 每次重启可读，早期 panic 也能抓 |
| XNU USB CDC | 大（驱动移植） | 实时，长期最优 |

## 建议顺序

1. **先做 ramoops**：它是唯一能在"内核没走到 console 初始化"时仍留下证据的通道，
   而且移植量与参考实现有明确对照。
2. ramoops 打通、能看到内核停在哪之后，再决定是否需要 XNU USB 换来实时性。

## 附：为什么屏幕停在 LockCapsuleInterface 不能归因于缺根卷

`DarwinBoot.c` 的执行顺序是：

```
L1127  gBS->ExitBootServices (mImage, Key)     ← LockCapsuleInterface 在此打印
L1130  DarwinEnter (...)                        ← 之后才跳进内核
```

`LockCapsuleInterface` 是 `ExitBootServices` 事件组回调，**在跳内核之前**。
根卷匹配发生在 XNU 内部、交接之后，两者的时间顺序不成立此因果。

但**根卷确实是真缺口**：macOS 27 profile 要求
`[payload.adt] root_match_volume_uuid = "84581E99-85BE-4957-AC53-2947E887B3E4"`，
而我们的 ADT 里**没有** `root-matching` 属性。即使内核跑起来，也无法匹配根卷。
这一条要修，但它不是当前停屏的原因。
