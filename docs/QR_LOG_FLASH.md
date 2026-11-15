# OneSend 二维码日志固件 · 打包与刷入 · 2026-11-15

## 功能

按官方文档 [log-capture.md §2](../../unixnu-tb710fu-plan/research/unixnu/log-capture.md)
的「扫传」思路，把上次 XNU 的 ramoops console 快照通过 **OneSend 二维码轮播**
传出屏幕，用另一台设备的摄像头接收后即可把日志文本发出来分析。

**已删除**上一版的屏幕文字翻页查看（`ViewLog`/`Font.h`/`DrawLine`）。

### 实现要点

- 数据源不是官方 `AdvancedLogDxe` 的 Advanced Logger 缓冲，而是
  `Tb710fuRamoopsDxe` 在 BDS 之前抓取的 ramoops 快照
  （`0xAC340000 + 0x40000`）。因此**不需要** `gAdvancedLoggerProtocolGuid`，
  **不改动**固件的 DebugLib / FrameBuffer 链——现有能正常显示的 DEBUG 输出
  路径保持不变。
- 完整复用 pinned UniXNU `AdvancedLogDxe` 的 OneSend 线协议：
  `LMS1` 信封（CRC32 + 文件名 `<model>-<date>-<time>.log`）、
  `OS` 帧（720 字节块 + 28 字节头 + 4 字节 CRC）、`QrEncodeData`、
  五档轮播速度（5s→250ms）、音量键调速、Enter/Esc 返回。
- 两个 Tools 插件：`Send XNU log (QR, ECC 0)` 和 `(QR, ECC 16)`。
  当前 ecc0 内核选 **ECC 0**。

## 云构建

run **43728193214** success（源码 `0c26f4c`）。
FD SHA-256 `f0e6b54186244d862a41601dec2ceaea5450d41b8ebd470122799b63e60d3df9`，
3,145,728 B，`lzma_kernel.darwinboot_patched=true`，11 个必需模块。
首次构建 43727231067 因漏 `UefiLib.h`（`EFI_TIMER_PERIOD_MILLISECONDS` 宏）
失败，已修复。

本地验证：host gcc 语法检查干净、ramoops 主机单测 PASS、
FD 独立解析含 RamoopsDxe `69454faf…` 与 DarwinBoot `1b64af87…` GUID。

## 交付（已打包签名）

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| [tb710fu-boot-uefi-logviewer.signed.img](out/log-viewer-qr/tb710fu-boot-uefi-logviewer.signed.img) | 100,663,296 | `72148b8bf03881b4041c93a9a83baafbccdd5a80b9f9c65762a11b09a68b0111` |

BootShim（FD_BASE `0xA7100000`, FD_SIZE `0x300000`）+ FD → gzip + DTB →
mkbootimg v4 → avbtool add_hash_footer（testkey, SHA256_RSA4096）。

**只刷这一个**。`vbmeta-ecc0.img` 里 boot 是 chain 描述符，
公钥与新 boot 签名 key 一致，vbmeta / vendor_boot(ecc0) 都不用动
（两者哈希已复核未变）。

## 刷入

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash boot_b "D:\unixnu-tb710fu-clean\out\log-viewer-qr\tb710fu-boot-uefi-logviewer.signed.img"
& $fb --set-active=b
```

## 使用

1. 用 ecc0 的 vendor_boot 跑一次 XNU，让它把日志写进 ramoops，然后复位
2. 进 UEFI 菜单 → `Tools` → `Send XNU log (QR, ECC 0)`
3. 另一台设备打开 [OneSend 扫传](https://onesend.01mvp.com) 的接收扫描，
   把完整二维码置于画面内，保持直到轮播帧传完
4. 音量加/减调轮播速度（默认 1 帧/秒，5 档 5s→250ms），Enter/电源返回
5. 保存收到的 `TB710FU-<日期>-<时间>.log`，正文核对是否有
   `=== TB710FU XNU boot (UniXNU ramoops) ===` 横幅

## 回滚

上一版屏幕查看器固件：`out/log-viewer/tb710fu-boot-uefi-logviewer.signed.img`
（`3413c36b…`），或完全回到
`out/lzma-keys/tb710fu-boot-uefi-lzma.signed.img`（`6bcc95a3…`）。

## 边界

- ramoops 快照能否跨复位存活仍未实机证明；若头无效，插件状态栏会显示
  「no valid console / empty」，不会改动原始内存。
- 二维码传的是**ramoops 里的 XNU 日志**，不是固件 Advanced Logger 日志
  （那条链本板未启用）。
- 详见 `docs/UEFI_LOG_VIEWER.md`、`docs/RAMOOPS.md`。
