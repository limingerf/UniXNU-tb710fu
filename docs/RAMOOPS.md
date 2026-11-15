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

已验证（host）：

- 设备补丁序列 0001–0004 在**已应用上游补丁的树**上按顺序全部干净应用
- 4 处挂钩落位、2 个新文件生成、括号平衡

未验证：

- 内核能否编译（云构建进行中）
- zone 内容能否在本机复位后存活
- Android 侧 `/sys/fs/pstore/console-ramoops-0` 是否真的出现

## 读取方式（设备复位后）

```bash
adb shell "cat /sys/fs/pstore/console-ramoops-0" | head -100
```

若 pstore 为空，说明 zone 未写入或复位类型不支持保留（参考移植记录：
`ramoops_reset_warm=1` 可尝试 PSCI `SYSTEM_RESET2`/`SYSTEM_WARM_RESET`）。
