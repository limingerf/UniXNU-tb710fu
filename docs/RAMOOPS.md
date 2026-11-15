# 内核日志通道：ramoops · 2026-11-15

## 为什么需要

交接后没有 UEFI 显示驱动，内核 printf 到不了面板。屏幕停在
`LockCapsuleInterface`（那是 `ExitBootServices` 的 DEBUG 输出）之后就没有任何
可读信息。每验证一次都要回 Android 抓 pstore，迭代成本高。

ramoops 把内核 console 镜像进 Android 已经预留的 `ramoops_region`，重启后
Android 可读 `/sys/fs/pstore/console-ramoops-0`。

## 本机 ramoops 布局（live FDT 实测）

```
ramoops_region {
    compatible = "ramoops";
    reg          = <0x00 0xac300000 0x00 0x100000>;   /* 1 MiB */
    record-size  = <0x40000>;   /* dmesg，从 0xac300000 起 */
    console-size = <0x40000>;   /* console，从 0xac340000 起 */
    pmsg-size    = <0x80000>;
    ecc-size     = <0x10>;
};
```

所以内核应使用 **`ramoops=0xac340000`**（console 区起点），而不是 region 基址。

## 补丁内容（`patches/xnu/0004-tb710fu-ramoops-console.patch`）

从参考移植（iPadOS 17）移植，**按本项目真实基线重新生成**。

### 基线为什么重要

macOS 27 profile 在设备补丁之前会先应用 **55 个上游 xnu-12377 补丁**，其中
第一个 `zzz-boot-progress.patch` 已经给 `arm_init.c` 加了
`#include <kern/boot_progress.h>`。所以：

- 基于**裸 feed** 生成的补丁 → 应用失败（实测）
- 基于**已应用上游补丁的树**（`xnu-work` @ `ac9718fb`）→ 干净应用

### 改动

| 文件 | 改动 |
| --- | --- |
| `osfmk/kern/tb710fu_ramoops.c`（新增 357 行） | Linux `persistent_ram` console zone 实现：12 字节头 `{sig,start,size}`、文本环、GF(2^8) Reed-Solomon 校验（poly 0x11d）；boot-args `ramoops` / `ramoops_size` / `ramoops_ecc` / `ramoops_panic_delay` / `ramoops_reset_after` |
| `osfmk/kern/tb710fu_ramoops.h`（新增 8 行） | 三个对外函数 |
| `osfmk/arm/arm_init.c` | `arm_vm_init()` 之后调用 `tb710fu_ramoops_init()`（macOS 27 此处无 `boot_progress_stage`，锚点即该调用本身） |
| `osfmk/console/serial_console.c` | `_cnputc()` 里把每个字符（跳过 CR）镜像进 zone |
| `osfmk/kern/debug.c` | `panic_spin_forever()` 与 `kdp_machine_reboot_type()` 在 panic 路径 flush 并 PSCI 复位 |
| `osfmk/conf/files` | 把新文件编进 osfmk |

## 启动参数

```
-v serial=3 ramoops=0xac340000 ramoops_reset_after=120
```

- `ramoops=0xac340000`：console 区物理地址（必须，否则功能关闭）
- `ramoops_reset_after=120`：早期启动 120 秒后自动复位，回到 fastboot/Android
  取日志。设 0 则不自动复位（需手动断电）
- `ramoops_panic_delay=15`：panic 后停留 15 秒再复位（默认）

## 验证状态

已验证（host + 云构建）：

- 设备补丁序列 0001–0004 在**已应用上游补丁的树**上按顺序全部干净应用
- 4 处挂钩落位、2 个新文件生成、括号平衡
- **内核编译通过**：native run 43709117408（success，`e3bb75c`），
  构建日志含 `kernel: patch 0004-tb710fu-ramoops-console.patch`
- **严格预链接通过**：prelink run 43710067378（success，`49ce1fa`），
  复用上述 native 产物，产出 142,753,792 字节 fileset
- 预链接产物字节内可检索到 `tb710fu_ramoops`、`=== TB710FU XNU boot`、
  `ramoops_reset_after`、`psci_system_reset`，证明补丁未被链接优化掉
- 交付镜像 14 项刷入前审计全 PASS（见下）

未验证（**实机**）：

- zone 内容能否在本机复位后存活
- Android 侧 `/sys/fs/pstore/console-ramoops-0` 是否真的出现
- 内核到底停在哪一步（这正是本次要查的）

实机首轮结果：pstore 为空，发现 ECC 参数不一致并统一；空日志的确切原因仍待原始内存快照验证。
TWRP 读侧能力本身已实测具备。

## 首次实机：pstore 为空，发现 ECC 参数不一致

刷入带 ramoops 的三件套、跑过 XNU 后进 TWRP，`/sys/fs/pstore/` 却是空的。
用户指出「换了安卓 GKI 内核、TWRP 复用该内核」是同类原因，实测证实：

**TWRP 内核具备完整读侧能力**（不是驱动缺失）：

| 检查 | 值 |
| --- | --- |
| 内核 | `6.1.177-android14-11-g4056c236e4c84-ab10021739-4k` |
| pstore 挂载 | `pstore on /sys/fs/pstore type pstore (rw)` |
| 内核配置 | `CONFIG_PSTORE=y` / `CONFIG_PSTORE_RAM=y` / `CONFIG_PSTORE_CONSOLE=y` |
| 模块 | `/sys/module/ramoops`、`/sys/module/pstore` 均存在 |
| `/sys/module/pstore/parameters/backend` | `ramoops` |
| live FDT | `ramoops_region 0xac300000+0x100000`，`console-size=0x40000` |
| `/proc/iomem` | `ac300000-ac3fffff : reserved` |

**关键差异在 `/sys/module/ramoops/parameters/`**：

| 参数 | GKI 实际值 | 我们 XNU 写入的值 |
| --- | --- | --- |
| `ecc` | **0** | **16**（`ramoops_ecc=16`） |
| `mem_address` | `0xAC300000` | console 子区 `0xAC340000` ✅ |
| `mem_size` | `0x100000` | `0x40000`（单个 console zone）✅ |
| `record_size` | `0x40000` | — |
| `console_size` | `0x40000` | `0x40000` ✅ |
| `pmsg_size` | `0x80000` | — |

FDT 里虽写了 `ecc-size = <0x10>`，但**这个 GKI 内核运行时 `ecc=0`**。
XNU 按 ecc=16 生成带 Reed-Solomon 校验的 zone，读取方 ecc=0 会采用不同
容量和环绕布局，所以应统一参数。但参考 Linux `persistent_ram` 在 ecc=0 时
会跳过 ECC 初始化及头纠错，不能据此断定“头校验失败清零”就是空 pstore 的根因。
是否已写入、是否被复位/ABL/后续内核覆盖，仍需原始区域头和正文证据；参考
mainline 源码也不能替代实际 GKI 6.1.177 对应源码。

地址语义核对无误：Linux 的 console prz 在 `mem_address + record_size`
即 `0xAC340000`，与 `ramoops=0xac340000` 一致；`ramoops_size=0x40000`
与 `console_size` 一致。补丁的 `RAMOOPS_SIG = 0x43474244`（"DBGC"）也与
Linux `PERSISTENT_RAM_SIG` 相同。`ecc=0` 时补丁跳过全部 parity 分支，
`data_size = size - HDR_BYTES`，布局字节兼容。

### 修复

只改一个启动参数：`ramoops_ecc=16` → **`ramoops_ecc=0`**，与 GKI 一致。

## 交付三件套（ecc=0，当前有效）

> 旧的 `vendor_boot.signed.img` / `vbmeta.img` 因 AVB GUI 占用文件句柄而无法
> 覆盖，故 ecc=0 版使用新文件名。两者内容不同，刷的时候别混用。

| 文件 | SHA-256 | 说明 |
| --- | --- | --- |
| `out/lzma-keys/tb710fu-boot-uefi-lzma.signed.img` | `6bcc95a31c5bf070fc84958531414689f7809d254879ab041a92fff063f8f2e4` | 未变，无需刷（FD `f6c0514e…`） |
| `out/lzma-keys/vendor_boot-ecc0.signed.img` | `836ca997d766dedfef0531f7283026b443d2227302148fdd107852e25354df15` | **刷这个** |
| `out/lzma-keys/vbmeta-ecc0.img` | `3992de82a3a03584b0e03c43bd66060ef93684122bb8867c893ed6c45ea85331` | **刷这个** |

vbmeta 的 vendor_boot digest `3cb25dab…` 与重算值一致；
ecc=0 镜像集 14 项审计全 PASS（含 `ramoops_ecc=0` 参数断言）。

内核产物链：

| 项 | 值 |
| --- | --- |
| 预链接 `kernel.macho` | 142,753,792 B，SHA-256 `cc690c9a0bde84c49e9cebccc288f393393d16da56620d580108f0a62692b31c` |
| LZMA 压缩流 | 24,054,955 B，SHA-256 `e16e07260e940e88cb2da34a7bda67454a256925d4c1e11821417ed05390a2bb` |
| 解压往返 | SHA-256 与源文件一致，大小字段已回填 142,753,792 |
| payload | 34,603,008 B，FAT32 66,512 簇，三文件读回逐字节 MATCH |

**只需刷两个镜像**（boot_b 未变）：

```
fastboot flash vendor_boot_b out\lzma-keys\vendor_boot-ecc0.signed.img
fastboot flash vbmeta_b      out\lzma-keys\vbmeta-ecc0.img
fastboot --set-active=b
```

启动后 `ramoops_reset_after=120` 会在 120 秒自动复位；若内核 panic，
`ramoops_panic_delay=15` 在 15 秒后复位。复位后立即读日志，别让 Android
把它覆盖。

## 读取方式（设备复位后）

```bash
adb shell "cat /sys/fs/pstore/console-ramoops-0" | head -100
```

若 pstore 为空，说明 zone 未写入或复位类型不支持保留（参考移植记录：
`ramoops_reset_warm=1` 可尝试 PSCI `SYSTEM_RESET2`/`SYSTEM_WARM_RESET`）。
