# UFS 启用版本 · 2026-11-15

## 交付物

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `out/ufs-keys/tb710fu-boot-uefi-ufs.signed.img` | 100,663,296 | `61677cb4d2e0c90c1757f97044899e69c882e88a55a3fab661443ea5b4351b6e` |
| `out/ufs-keys/vbmeta.img` | 8,000 | `99b197c4031ea576f4926952ba200bf41289cba35d4ca30dc21c3252b4c40856` |

vendor_boot_b 沿用 menu-fix 版本（`e1a8029a…`），boot_b 换成上面这个。

## 云构建证据

run **43692304879**（commit `50ba12c`），success。

FD：`dfca596a6a665d523beba0d25c623a6e396b4ccc9bb185b27f096a6adb743a36`，
3,145,728 字节，AARCH64 DEBUG CLANGPDB。

FVMAIN 从 51 个 FFS 文件增长到 **60 个**（+9）。

### 8 个 UFS 链模块在真实嵌套 FFS 中验证

| 模块 | FILE_GUID |
| --- | --- |
| SmemDxe | `f541d663-4a48-40aa-aabf-ff158ccae34c` |
| HWIODxe | `af9763a2-033b-4109-8e17-56a98d380c92` |
| CmdDbDxe | `aba01ff8-2ccb-4e12-8b2e-cd3f4a742993` |
| PwrUtilsDxe | `0401b830-bff9-44a2-a5f6-95734a33c017` |
| NpaDxe | `cb29f4d1-7f37-4692-a416-93e82e219711` |
| RpmhDxe | `cb29f4d1-7f37-4692-a416-93e82e219766` |
| ClockDxe | `4db5dea6-5302-4d1a-8a82-677a683b0d29` |
| **UFSDxe** | `0d35cd8e-97ea-4f9a-96af-0f0d89f76567` |

原有 10 个必需模块全部保留。`vendor_binary_inputs: true`、`hardware_boot_verified: false`。

## 镜像签名

| 项 | boot.img | vbmeta |
| --- | --- | --- |
| 算法 | SHA256_RSA4096 | SHA256_RSA4096 |
| 公钥 SHA-1 | `2597c218…`（testkey） | `2597c218…` |
| rollback | 1736035200 | 0 |
| 描述符 | boot hash | 3 hash + 4 hashtree + 3 chain + 13 prop |

vbmeta 描述符与原厂结构完全一致；boot 由 `chain@3` 覆盖，因此新 boot 镜像自带的
vbmeta 由同一 testkey 签名即可，vbmeta 本身无需 hash 描述符。

## 刷入

```
fastboot flash boot_b   out\ufs-keys\tb710fu-boot-uefi-ufs.signed.img
fastboot flash vbmeta_b out\ufs-keys\vbmeta.img
```

vendor_boot_b 若已是 menu-fix 版本则不用再刷。

## 实机判据

- **成功迹象**：UEFI 菜单出现，且比之前多出 UFS 相关 DEBUG 行
  （`UFS INQUIRY`、`UFS Boot LUN` 之类，取决于 UFSDxe 的打印）。
- **失败模式 A（黑屏）**：ClockDxe 或 RpmhDxe 与板级时钟树冲突。
  下一步：把 ClockDxe 从链里去掉再试（NpaDxe 可能仍能提供足够服务）。
- **失败模式 B（菜单在但无分区）**：UFSDxe 的 PHY 校准参数不匹配 TB710FU。
  这是 giulia 替代集的已知风险，需要 TB710FU 自己的 UFSDxe。
- **失败模式 C（挂死/复位循环）**：槽 b 失败 7 次后 ABL 自动回槽 a。

回滚：`fastboot --set-active=a`，或用
`D:\avbtool-all\backup_20261115_142158\` 里的原始三件套刷回槽 b。

## 未解决：驱动来源

`Binaries/tb710fu` 在 CI 中由 `Binaries/giulia` 复制而来
（`Project-Silicium/Device-Binaries@a1501e0c` 没有 tb710fu 目录）。
这些是 OnePlus Ace 5 的模块，UFS 控制器虽为 SM8650 共用块，
但板级 PHY 校准和时钟树可能与 TB710FU 不同。

提取 TB710FU 原厂 `uefi_a` 驱动的尝试未成功：该 FV 不是可遍历的 FFS 布局
（详见 `docs/UEFI_TWRP_PLAN.md`），需要 UEFITool 级解析器。
