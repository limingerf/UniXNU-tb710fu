# UEFI 做成 TWRP 式能力的路线 · 2026-11-15

## 需求

槽 b 进 UEFI 后，希望能像 TWRP 一样：切换 slot a/b、重启到各模式、能调试。
实际动机：槽 b 的 fastboot 模式不切 a 槽就进不了 recovery。

## 关键结论

### 切槽的本质

`misc.img` 实测：`bootloader_message.command = "bootonce-bootloader"`，
**没有 `bootloader_control` 结构**（magic `0x42414342` 不存在）。
槽位状态由 ABL/XBL 管理，存在 `devinfo`（4 KiB）分区里。

官方头文件 `EmbeddedPkg/Include/Protocol/GblEfiBootControl.h` 定义了标准接口
（`GetSlotCount` / `GetSlotInfo` / `GetCurrentSlot` / **`SetActiveSlot`** /
`GetOneShotBootMode`），但**整个固件里没有任何实现者**。要切槽必须自己实现 DXE。

### 前置条件

切槽要写 `devinfo`，写 `devinfo` 要块设备，块设备要 **UFS 驱动**。
当前 `TB710FU_VENDOR_HW = 0`，UEFI 没有 UFS，看不到任何分区。

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

- `tools/ci_firmware.py --ufs`：部署副本翻转 gate + 在真实嵌套 FFS 里校验 8 个链模块
- `.github/workflows/uefi-ufs.yml`：独立 workflow，不与基线冲突
- `scripts/check-tb710fu-firmware.py`：仅当 `UNIXNU_ENABLE_UFS=1` 时放行 dsc 这一处差异

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
