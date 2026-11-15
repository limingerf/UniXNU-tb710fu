# XNU 实机进入内核 · 2026-11-15

## 结论

**TB710FU 在实机上执行到了 XNU 内核，处于 EL1。** 这是本工程第一次到达该
阶段：整条链 UEFI → UniXNU → payload FAT → LZMA 解压 → ADT 校验 →
窗口分配 → XNU EL1 全部通过。

## 屏幕证据（用户拍摄）

| 屏幕内容 | 含义 |
| --- | --- |
| 顶部**绿色横带** | DarwinBoot 交接标记，进入内核前的所有阶段都已过 |
| `Current EL EL1` | 内核在 EL1 执行，UEFI 的 EL2 已成功降级 |
| `TCR_EL1 0x80000003510` | 内核自己配置了 MMU 翻译控制 |
| `ID_AA64MMFR0_EL1 0x1100000000111121` | 读 CPU ID；`Parange 2`（44-bit PA）、`4 KiB granule supported` |
| `ID_AA64ISAR1_EL1` / `ID_AA64ISAR2_EL1` | PAC 能力查询，即 EL1/PAC 验证点 |
| `Darwin Kernel Version 27.3.0` | XNU 内核版本横幅 |

## 达成的阶段（按工程约定逐项）

| 检查点 | 状态 |
| --- | --- |
| UEFI 固件启动、菜单显示 | ✅ 已验证 |
| payload FAT 挂载、`unixnu.toml` 解析 | ✅ `config: \EFI\UniXNU\unixnu.toml` |
| 菜单列出 xnu 条目 | ✅ `1 UniXNU (LZMA kernel)  xnu` |
| DarwinBoot LZMA 解压 | ✅ 交接前无错误，进入 EL1 |
| ADT 校验通过 | ✅ 没有 `EFI_COMPROMISED_DATA` |
| 内核窗口分配 | ✅ 绿色横带画出 |
| **XNU 在 EL1 执行** | ✅ `Current EL EL1` + 版本横幅 |
| 平台专家匹配 | **未验证**（下一步） |
| UFS / 根卷 / launchd / 桌面 | **未验证** |

## 修复链回顾（每一步都是实机暴露的）

| # | 症状 | 根因 | 修复 |
| --- | --- | --- | --- |
| 1 | 菜单无 xnu | 25 MiB payload 是 FAT32 但仅 50,380 簇 < 65,525 下限，固件拒挂载 | 33 MiB FAT32（66,512 簇）；新增 `tools/check_payload_fat.py` |
| 2 | 选 xnu 卡死 `ASSERT [DxeCore] Pool.c(750) Head->Signature == 'pdh'` | 用 `AllocatePages` 分配内核缓冲，DarwinBoot 用 `FreePool` 释放 | `ExpandLzmaKernel` 改用 `AllocatePool`，错误路径同样用 `FreePool` |
| 3 | 缺 `.adt` 会直接 `goto Fail` | clean board 无 `[payload]` 配置 | 从本机 live FDT 派生 ADT（补 memory 单范围、timebase、删 cpu-map、加 psci） |

## 当前产物

| 文件 | SHA-256 |
| --- | --- |
| `out/lzma-keys/tb710fu-boot-uefi-lzma.signed.img` | `6bcc95a31c5bf070fc84958531414689f7809d254879ab041a92fff063f8f2e4` |
| `out/lzma-keys/vendor_boot.signed.img` | `4e296d5fc077ee02f4d173d676dc86669ca65f7eb419f1acf9131872cbf4e5bc` |
| `out/lzma-keys/vbmeta.img` | `1e621fc579226849569701a8011a3bb3107130a9ac7ce31840a011f68670da31` |

固件 FD：`f6c0514e58312f733c83acbfd30b00462df8429853da008d65fc50c3fdccd24e`
（云 run 43706338266，`darwinboot_patched=true`）。

## 下一阶段（按最早失败点推进）

1. **确认内核停在哪里**。屏幕右侧文字被裁切，需要完整屏幕内容或
   `vc-text=1` 下滚动查看；当前还不知道是停在平台专家匹配、panic，
   还是继续到根卷。
2. **ramoops / pstore**：参考移植把内核控制台镜像进 `ramoops_zone`
   （本机 live FDT 里有 `ramoops@ac300000`），可在回 Android 后取日志，
   比拍照可靠。
3. 之后才是 UFS、根卷、Data/cryptex、launchd。

## 边界

本文只声称 **XNU 在 EL1 执行并打印版本横幅**。以下都**没有**证据：
平台专家匹配成功、`/cpus`/`/arm-io`/`/defaults` 被接受、根卷挂载、
用户态启动、显示服务、GPU。

## 补充：停屏位置 `LockCapsuleInterface` 的含义

后续一次启动停在屏幕显示 `...LockCapsuleInterface, Outputs: ...`。这个字符串
**不是内核输出**，来自 UEFI：

- `MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleService.c:410`
  `LockCapsuleInterface()` 是 `ExitBootServices` 事件组的回调；
- 它调用 `SECURITY_LOCK_REPORT_EVENT("Lock Capsule Interface", SOFTWARE_LOCK)`；
- DEBUG 构建把该事件画到屏幕。

所以这条输出证明的正是：**固件走到了 ExitBootServices 通知，DarwinBoot 的
交接已完成、控制权已交给 XNU**。它是正常关机路径的一部分，不是错误。

内核之后没有可读输出，是因为交接后没有 UEFI 显示驱动，内核的 printf
到不了这块 framebuffer。**绿条之后屏幕无字，对这个配置是预期行为**，
不能据此判定内核已经挂掉。

### 要看清内核走多远，需要内核侧日志

按信息量排序：

1. **ramoops 控制台**（推荐）。本机 live FDT 声明了
   `ramoops_region { reg = <0xac300000 0x100000>; console-size = <0x40000>; }`，
   控制台区在 `0xac340000`。内核把 console 镜像进该区后，重启回 Android
   可用 `/sys/fs/pstore/console-ramoops-0` 读取完整内核日志。
   不依赖任何显示驱动，早期 panic 也能留下。
2. `vc-text=1` 内核参数：让内核文本走面板控制台。只在
   `PE_init_console` 之后有效。
3. `panic_screen` / 入口探针：在 framebuffer 上画阶段色条，用二分定位
   执行停点。

### 当前缺口

参考移植 `bmk-keyeast/UniXNU-tb710fu` 有
`patches/xnu/tb710fu-ramoops-console.patch`（457 行：新增
`osfmk/kern/tb710fu_ramoops.{c,h}`，改 `arm_init.c`、`osfmk/conf/files`、
`serial_console.c`、`kern/debug.c`）。

**该补丁面向 iPadOS 17 的 XNU，无法应用到本工程 macOS 27 的 xnu-12377**
（`git apply --check` 在 `arm_init.c`、`serial_console.c`、`conf/files`
三处失败）。要拿到内核日志，需要按 macOS 27 源码重写该补丁。

这是一次独立的内核补丁移植工作，不是改参数能完成的。
