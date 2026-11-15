# UEFI 启动 TWRP：完整方案与现状 · 2026-11-15

> 后续核对（2026-11-15）：用户确认当前 A 槽可启动 Android/TWRP，已从 TWRP
> 只读提取当前 A 槽启动输入。`boot_a` 中确有 37,474,368 字节 Linux 内核，
> 带 AARCH64 EFI stub；下文“缺少 Linux 内核”“必须重新编译”“唯一可行路径”
> 仅反映旧备份分析，已被当前输入证据取代。见
> [当前 A 槽输入](evidence/android-oneshot-a-inputs.json) 和
> [当前实现范围](UEFI_LOG_VIEWER.md)。主线 Linux 也不能直接替代 GKI 运行既有 TWRP 模块。

## 核心因果（已实机确认）

TWRP 的 recovery 分区**不含内核**（`BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE := true`），
启动时**复用 boot 分区的内核**。

boot 分区被刷成 UEFI 固件后：
```
boot 被 UEFI 覆盖（PE 头 MZ@，非 Linux 内核）
  → 原厂 Linux 内核丢失
  → TWRP 复用 boot 内核 → 起不来
  → 只能靠 9008 EDL 硬切槽
```

## boot-chain 备份穷举结果（2026-11-15）

| 分区 | 内核字段 | 类型 |
| --- | --- | --- |
| boot_a / boot_b | `MZ@` | PE（UEFI 固件，非内核） |
| recovery_a / recovery_b | kernel=0 | TWRP ramdisk（79.4 MiB，LZ4+cpio），无内核 |
| init_boot_a / init_boot_b | kernel=0 | 空 |
| abl_a / uefi_a / uefi_b | ELF | ABL / UEFI（uefi_a 是 ARM32 ELF + Tiano 压缩） |
| vendor_boot* / dtbo* / vbmeta* | 数据 | 非内核 |

**结论：原厂 Linux 内核在 boot-chain 里已不存在**（刷 UEFI 时覆盖，备份也是之后做的）。

## 目标与唯一可行路径

目标：UEFI 菜单像 TWRP 一样用 adb/fastbootd。

**唯一可行路径**（绕开 UEFI vendor USB 灰屏链）：

```
UEFI 菜单 (UniXNU type="linux")
  → Linux 内核 (需从 tb710fu-linux 编译，开 CONFIG_EFI_STUB + DWC3 + UFS)
  → TWRP ramdisk (现成，recovery_a.img，79.4 MiB)
  → Linux DWC3 自动起 USB → adbd + fastbootd
```

### 技术确认

- UniXNU 原生支持 `type="linux"` 条目：`path`（内核）、`initrd`（ramdisk，
  经 LoadFile2 发布）、`dtb`（`"firmware"` 或文件）、`args`（内核命令行）
- 官方文档样例（`docs/porting/device-tree.md:254`）：
  ```toml
  [[entry]]
  type = "linux"
  path = "Payloads/Image"
  dtb = "DeviceTree/board.dtb"
  ```
- initrd 经 `UniXNUInstallInitrd` 走 LoadFile2 协议，Linux ≥5.8 的 EFI stub 支持

### 缺失的唯一一块：Linux 内核

| 组件 | 现状 |
| --- | --- |
| 内核源码树 | **未 clone**（`tb710fu-linux` 约 1-2 GB） |
| 交叉编译器 | ✅ `aarch64-linux-gnu-gcc 14.2.0` |
| 设备 DTS | ✅ `sm8650.dtsi` + `sm8650-lenovo-tb710fu.dts`（已下载） |
| TWRP ramdisk | ✅ `recovery_a.img`（79.4 MiB，LZ4+cpio） |

## 下一步选择

1. **clone tb710fu-linux 并编译**（一次性约 1-2 GB 下载 + 内核编译），产出
   带 EFI stub 的 `Image`，与现成 TWRP ramdisk 组合成 `type="linux"` 载荷
2. 验证：UEFI 菜单多一个 TWRP 项，选中 → Linux 起 → adb/fastbootd 可用

## 未决问题

- `tb710fu-linux` 主线的 `.config` 是否已开 `CONFIG_EFI_STUB`（主线 defconfig
  通常默认开，但要确认该树有 TB710FU 的 defconfig 或需手动配）
- payload 容量：Linux Image ~30 MiB + TWRP ramdisk 79.4 MiB = ~110 MiB，超过
  vendor_boot 81 MiB 可用空间 → 需要把 TWRP 放进 UFS 分区（unixnu 分区），
  或压缩内核/精简 ramdisk
- DTB：live FDT（980 KB）可用，或用主线 DTS 编译
