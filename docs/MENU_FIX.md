# UEFI 菜单修复与内核容量分析 · 2026-11-15

## 本轮交付：UEFI 菜单增加 Restart / Power off

槽 b 三件套已重新构建并签名。相对之前 `D:\avbtool-all` 的版本，**只改了 vendor_boot 的 payload FAT**
（`unixnu.toml` 增加 `reset` / `shutdown` 动作项），boot_b 不变。

| 文件 | 字节 | SHA-256 | 说明 |
| --- | --- | --- | --- |
| `vendor_boot.signed.img` | 100,663,296 | `e1a8029a334811a48aa080b276791e64622fab690c5af263c47bc9c267ebb514` | 64 MiB payload FAT + 原厂 dtb/vbmeta + AVB 签名 |
| `vbmeta.img` | 8,000 | `436b9e20e0a6ee5a0bb92b4afecf19fe60e049228ed8c55b1e1235391ef6c015` | 完整 23 描述符，仅替换 vendor_boot hash |
| `boot.img` | 100,663,296 | `af406b14675f3833104313f6fa91ebe956a1ac7c5dcb766c0efa44b8e0a55dab` | 不变（`D:\avbtool-all\boot.img`） |

### vbmeta 完整性

从之前可工作的 `D:\avbtool-all\vbmeta.img` 导出全部描述符，**只替换 vendor_boot 的 hash 描述符**，
其余原样保留（用 `--include_descriptors_from_json` 重建）：

| 描述符类型 | 数量 | 分区 |
| --- | --- | --- |
| hash | 3 | dtbo / init_boot / **vendor_boot（新）** |
| hashtree | 4 | odm / system_dlkm / vendor / vendor_dlkm |
| chain | 3 | boot@3 / recovery@1 / vbmeta_system@2 |
| prop | 13 | 原厂 build props |

vendor_boot 的 hash 交叉校验：vbmeta 记录 `9b149d6e…`，按 salt+镜像重算一致。

> 先前一次错误尝试只写了 boot chain + 3 个 hash（4,160 B），会丢掉 odm/vendor 等 hashtree 保护和
> recovery/vbmeta_system chain，已废弃。最终产物是完整的 8,000 字节版本。

### payload 菜单项

`EFI/UniXNU/unixnu.toml`：

| id | title | action |
| --- | --- | --- |
| xnu | UniXNU | （内核条目，`Payloads/kernel.macho` 当前为空） |
| reset | Restart | `reset` → PSCI 复位 |
| shutdown | Power off | `shutdown` |
| return | Return to firmware | `exit` → 交回 ABL |

### 刷入

```
fastboot flash vendor_boot_b out\menu-fix\vendor_boot.signed.img
fastboot flash vbmeta_b       out\menu-fix\vbmeta.img
```

boot_b 保持现状（已签名的 UEFI 载体）。

### 返回安卓的实际机制（重要）

`reset` 只是 PSCI 复位，复位后 ABL 仍从槽 b 启动 UEFI，**不能直接回安卓**。
本机 `misc.img` 实测 `bootloader_message.command = "bootonce-bootloader"`，且**不含**
`bootloader_control` 结构（magic `0x42414342` 不存在），槽位状态不由 misc 决定。

返回安卓只有两条路径：

1. 物理冷启动 → 进 fastbootd → `fastboot --set-active=a`（**临时切换，不是永久**）。
2. 槽 b 连续启动失败 7 次后 ABL 自动回槽 a。实测 `slot-retry-count:b` 已到 6。

## 内核容量：根本矛盾（实测数据）

| 项 | 值 | 来源 |
| --- | --- | --- |
| macOS 27 预链接内核 | 142,753,792 B（136.1 MiB） | `docs/evidence/prelinked-build.json` |
| gzip 后 | 36,564,132 B（34.9 MiB） | 本轮实测 |
| vendor_boot 分区 | 96 MiB | fastboot `getvar` |
| vendor_boot ramdisk 可用 | 85,024,768 B（81.1 MiB） | 扣除 dtb 3,243,613 + table + bootconfig + vbmeta 11,838,208 |
| payload FAT 需求 | ≥143 MiB | 内核/0.95 |

**payload FAT 中内核不压缩**：DarwinBoot 从 FAT 读 `kernel.macho` 是原始 Mach-O，`payload.py` 原样拷贝，
没有解压步骤。此前"压缩内核"失败的根因就在这里。

## 官方架构对照

- 官方 sheng board.toml：macOS 27.3 override 用 `payload.size_mib = 200`，
  注释写明"the macOS kernel collection (137 MiB), the Data seed (20 MiB) and the GPU addon"。
- 官方 sheng 用 `fastboot boot`（219,283,456 B 镜像直接 RAM 启动），**不写分区**。
- 本机 ABL 不支持 `fastboot boot`：实测返回 `FAILED (remote: 'unknown command')`。

## 关于改 LK/ABL 加 fastboot boot

参考 `MengWanYu/iqoo-z8-lk-uefi`（MTK MT6896）的方法在 Qualcomm 上不可行：

| 项 | iQOO Z8 (MTK) | TB710FU (Qualcomm) |
| --- | --- | --- |
| 签名 | CERT2，社区可重签 | QTI secboot，无私钥 |
| 分区 | `lk_a` 16 MiB | `abl_a` 1 MiB |
| 验签者 | LK 自身 | PBL（不可改） |
| 救砖 | BROM/FlashTool | EDL 9008 + firehose |

## 下一步：UEFI 启用 UFSDxe

`DXE.inc` 中 `UFSDxe` 在 `TB710FU_VENDOR_HW == 1` gate 内，当前为 0（未启用）。
UFSDxe 运行时依赖（从 PE 字符串分析）：HWIO Protocol、NpaProtocol、DTBExtn、Clock。

2026-11-11 全开导致面板黑屏。建议最小启用：`HWIODxe + NpaDxe + ClockDxe + RpmhDxe + UFSDxe`，
关闭 `DALSYSDxe / VcsDxe / GpiDxe / SPIDxe / I2CDxe / SdccDxe / FeatureEnablerDxe`。

启用后内核可从 `unixnu` 分区读，摆脱 81 MiB ramdisk 限制。
