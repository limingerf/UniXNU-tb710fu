# UEFI 做成 TWRP 式能力的路线 · 2026-11-15

## 需求

槽 b 进 UEFI 后，希望能像 TWRP 一样：切换 slot a/b、重启到各模式、能调试。
实际动机：槽 b 的 fastboot 模式不切 a 槽就进不了 recovery。

## 关键结论

### 切槽的本质（2026-11-15 更正：不是 devinfo，是 GPT 分区属性）

**更正**：早前假设槽位在 `devinfo`（4 KiB）分区里，经查证 AOSP
`hardware/qcom/bootctrl/boot_control.cpp`（官方高通 bootctrl 源码）后推翻。

真实机制：

```c
// boot_control.cpp 的 set_active_boot_slot()：
// 1. 遍历 pt_list 里所有 A/B 分区（boot/system/vendor/vendor_boot/...）
// 2. 对每个分区读 GPT 项，取 AB_FLAG_OFFSET 处的属性字段
// 3. 目标槽所有分区项：attributes |= AB_PARTITION_ATTR_SLOT_ACTIVE
//    另一槽所有分区项：attributes &= ~AB_PARTITION_ATTR_SLOT_ACTIVE
// 4. gpt_disk_update_crc() + gpt_disk_commit() 写回主 GPT + 备份 GPT
// 5. UFS 设备额外：gpt_utils_set_xbl_boot_partition(NORMAL_BOOT/BACKUP_BOOT)
//    即切 UFS 的 boot LUN（0=主 boot LUN，1=备份 boot LUN）
```

**槽位状态 = GPT 分区表项 Attributes 字段（offset +48，8 字节）里的
`AB_PARTITION_ATTR_SLOT_ACTIVE` 位（bit 0）**，不是 `devinfo` 分区，也不是
`misc` 的 bootloader_control。

- `misc.img` 实测：`command = "bootonce-bootloader"`，无 bootloader_control
  （magic `0x42414342` 不存在）—— 与上述结论一致，misc 只用于 bootonce 命令。
- 槽位后缀由 ABL 通过 cmdline `androidboot.slot_suffix` 或 DT
  `/firmware/android/slot_suffix` 传给内核。

官方头文件 `EmbeddedPkg/Include/Protocol/GblEfiBootControl.h` 定义了标准接口
（`GetSlotCount` / `GetSlotInfo` / `GetCurrentSlot` / **`SetActiveSlot`** /
`GetOneShotBootMode`），但**整个固件里没有任何实现者**。要切槽必须自己实现 DXE。

### 前置条件

切槽要改 GPT 分区表项（主表 + 备份表），要改 GPT 就要读写块设备，块设备要
**UFS 驱动**。当前 `TB710FU_VENDOR_HW = 0`，UEFI 没有 UFS，看不到任何分区。

另外 UFS 设备还要切 **boot LUN**（`set_xbl_boot_partition`），这个在 UFS 设备
上可能才是决定"下次从哪个槽的 xbl/abl/boot 启动"的关键，且必须由 UFS 驱动
发出对应的 UFS 命令（或写 RPMB/boot LUN 配置）。

**更正后的切槽实现要点**（比"写 devinfo"更精确，也更像标准 UEFI 做法）：

1. UFS BlockIo 可读 → 找到承载 A/B 分区的 GPT 盘（sde，见 device-facts）
2. 读主 GPT + 备份 GPT，定位每个 A/B 分区的分区项
3. 改 Attributes 字段的 `AB_PARTITION_ATTR_SLOT_ACTIVE` 位（bit 0）
4. 重算 GPT 头 CRC + 分区项 CRC，写回主表 + 备份表
5. UFS 侧切 boot LUN（NORMAL_BOOT ↔ BACKUP_BOOT）
6. 冷重启，ABL 按新 active slot 加载对应 boot

## 已交付：UEFI 菜单修复

见 [MENU_FIX.md](MENU_FIX.md)。`vendor_boot.signed.img` + `vbmeta.img` 已刷入槽 b，
菜单现在有 `xnu` / `Restart` / `Power off` / `Return to firmware`。

## 本轮进行中：UFS 最小启用

### 改了什么

`tb710fu.dsc` 新增 `TB710FU_ENABLE_UFS`（默认 0）。为 1 时加入 8 个 stock Qualcomm DXE：

| 模块 | 作用 |
| --- | --- |
| `SmemDxe` | 共享内存，UFS 初始化用的 SoC 身份 |
| `HWIODxe` | HWIO 寄存器访问 |
| `CmdDbDxe` | command DB（RPMh 依赖） |
| `PwrUtilsDxe` | 电源工具 |
| `NpaDxe` | NPA 时钟投票 |
| `RpmhDxe` | RPMh 时钟/电源请求 |
| `ClockDxe` | UFS 核心与 PHY 时钟 |
| `UFSDxe` | UFS 主机控制器驱动 |

全部 `DEPEX TRUE`，顺序由 `APRIORI.inc` 固定：SMEM→HWIO→CmdDb→PwrUtils→NPA→RPMh→Clock→UFS。

### 明确排除（2026-11-11 黑屏集）

`DALSYSDxe`、`VcsDxe`、`DisplayDxe`、`GpiDxe`、`SPIDxe`、`I2CDxe`、`SdccDxe`、
`CPRDxe`、`PIL/PILProxy`、`TzDxe`、`FeatureEnablerDxe`、`RscDxe`。
`Tools/check-board.py` 在 gate=1 时强制校验白名单和这个排除集。

### 默认构建未变

gate=0 时 `check-board.py` 会剥掉 `!if $(TB710FU_ENABLE_UFS) == 1` 块再判定基线，
所以审计过的卷、40 区内存表、10 个必需模块**完全没动**（已双向验证）。

### CI

- `tools/ci_firmware.py --ufs`：`unixnu fetch` 初始化 `Binaries` submodule →
  把上游 `Binaries/giulia` 复制成 `Binaries/tb710fu`（板级命名）→ 校验 8 个 INF →
  部署副本翻转 gate → 在真实嵌套 FFS 里校验 8 个链模块
- `.github/workflows/uefi-ufs.yml`：独立 workflow，不与基线冲突
- `scripts/check-tb710fu-firmware.py`：仅当 `UNIXNU_ENABLE_UFS=1` 时放行 dsc 这一处差异
- `board.toml`：`submodules` 加 `Binaries`；`packages` 加 `{uefi}/Binaries/tb710fu`

### 为什么是"复制成 Binaries/tb710fu"

上游 `Project-Silicium/Device-Binaries@a1501e0c` 里**没有 `tb710fu` 目录**
（只有 `giulia`、`alioth` 等机型）。本地的 `Binaries/tb710fu` 是手工从 giulia
复制改名而来的，从未进入任何 git。

直接让 DSC 引用 `Binaries/giulia` 会让"TB710FU 的固件使用 OnePlus Ace 5 的
目录名"，语义错位。因此 CI 在构建 checkout 内把它复制成板级名字，DSC/FDF 和
`check-board.py` 全部读 `Binaries/tb710fu`。该副本只存在于构建目录，不提交。

## 重大限制：驱动来源

`Binaries/tb710fu/README.md` 原文：

> The files are the published SM8650 OnePlus Ace 5 (giulia) modules from
> Project-Silicium/Device-Binaries @9f4dbfd7 ... **This board's own UEFI volume
> has not been extracted yet.** Modules that encode board-specific data (panel
> configuration, charger/battery tables, touch firmware names) are therefore the
> Ace 5 ones.

即：**这些 vendor DXE 是 OnePlus Ace 5 的，不是 TB710FU 的**。

UFS 主机控制器是 SM8650 共用块，控制器访问路径可能通用；**板级 PHY 校准和时钟树是风险点**。

### 提取 TB710FU 原厂驱动：本轮尝试失败

`uefi_a.img`（5 MiB）分析结果：

| 项 | 值 |
| --- | --- |
| 容器 | ELF64，`phdr[1]` 加载到 `0xa7000000`，大小 `0x2c3000` |
| FV | 起始 `0x1000`，`_FVH` 在 `0x1028`，`FvLength=0x2c3000`，`hdr_len=0x48` |
| FV 头校验 | 不通过（`0x9a02`） |
| FFS 遍历 | 失败，无有效 FFS 链 |
| `UFSDxe`/`HWIODxe` GUID | 均未出现 |
| 熵分布 | `0x50000` 之后 7.95-7.99（压缩/加密），`0x40000` 有 FF 空洞 |
| 明文痕迹 | `0x50000` 有 `uefiplat.cfg` 的 UTF-16 名和 `[Config]` 内容 |

FV 里 `0x50000` 处可见明文（UI section 的 `fiplat.cfg`），但整体不是可遍历的 FFS 布局，
需要 UEFITool 级解析器才能提取。**本轮未完成提取**，因此驱动替换无法进行。

## 下一步（按依赖顺序）

1. **等 UFS 构建产物**，刷入验证：菜单是否还在？能否看到 UFS 分区？
   - 黑屏 → 逐个关 ClockDxe/RpmhDxe 排查
   - 菜单在但无分区 → 查 UFSDxe 日志，可能需要 PHY 参数
2. UFS 可见后，写一个 DXE 应用实现 `GblEfiBootControl` 的 `SetActiveSlot`：
   读 `devinfo` → 解析 slot 元数据 → 写回 → 重启
3. 再做 USB 调试通道（`UsbfnDwc3Dxe` + `UsbInitDxe` + `UsbConfigDxe` 全套都在）
4. 长期：用 UEFITool 提取 TB710FU 自己的 UFSDxe，替换 giulia 版本
