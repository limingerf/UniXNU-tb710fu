# UEFI 日志查看器固件 · 打包与刷入 · 2026-11-15

## 这是什么

gpt 编译的固件（云 run 43722058056，源码 `825a8560`），在现有 LZMA
DarwinBoot 基础上新增 `Tb710fuRamoopsDxe`：UEFI 启动时把
`0xAC340000+0x40000` 的 ramoops console 快照复制到 BootServices 池，
菜单 `Tools` 里可直接翻页查看上次内核日志，不用回 Android 抓 pstore。

FD SHA-256：`6a6ff288ddf48d1576cff991fb99b6feb90b4298c442f26eaf3442ee05a60f1e`
（与 `docs/UEFI_LOG_VIEWER.md` 及 CI 报告一致）。

## 本次交付（打包 + 签名）

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| [tb710fu-boot-uefi-logviewer.signed.img](out/log-viewer/tb710fu-boot-uefi-logviewer.signed.img) | 100,663,296 | `3413c36b9a43e8b640e02e2535f9848c5d1845f2776e65bff84270cec9a96608` |

打包：BootShim.AARCH64（`FD_BASE=0xA7100000`, `FD_SIZE=0x300000`）+ 新 FD
→ gzip + tb710fu.dtb → `mkbootimg.py --header_version 4 --pagesize 4096`。
签名：`avbtool add_hash_footer`，testkey_rsa4096，`SHA256_RSA4096`，
salt `a9f8c34f…`，rollback `1736035200`。

### 校验（全部通过）

- boot.img 解出的 gzip 内核 == shim+FD 逐字节相等，unused_data == DTB
- 内嵌 FD `6a6ff288…` 与 CI 产物一致
- FD 独立解析：两个真实 FV（53 + 2 个 FFS 文件），
  DarwinBoot `1b64af87…` 与 Tb710fuRamoopsDxe `69454faf…` 均在卷中
- AVB footer 公钥 SHA-1 `2597c218…` == vbmeta 的 boot chain key

## 为什么只刷一个镜像

`vbmeta-ecc0.img` 里 **boot 是 chain 描述符**（不是 hash）：
`Partition Name: boot` / `Public key (sha1): 2597c218…`。
只要新 boot.img 用同一 testkey 自签名，vbmeta 无需改动。

vendor_boot 仍用 ecc0 版（`836ca997…`），`ramoops_ecc=0` 参数保持不变，
日志查看器固件的「Last XNU log (ramoops ECC 0)」正对应它。
`vbmeta-ecc0.img`（`3992de82…`）里的 vendor_boot digest `3cb25dab…` 与
该 vendor_boot 匹配。

**这两个文件本次未改动，也不用刷。**

## 刷入（设备需处于 fastbootd）

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash boot_b "D:\unixnu-tb710fu-clean\out\log-viewer\tb710fu-boot-uefi-logviewer.signed.img"
& $fb --set-active=b
```

建议先确认当前 `vendor_boot_b` / `vbmeta_b` 就是 ecc0 那两个文件
（若不是，一并刷回）：
```
& $fb flash vendor_boot_b "D:\unixnu-tb710fu-clean\out\lzma-keys\vendor_boot-ecc0.signed.img"
& $fb flash vbmeta_b      "D:\unixnu-tb710fu-clean\out\lzma-keys\vbmeta-ecc0.img"
```

## 验收

1. 进 UEFI 菜单（B 槽）
2. 选 `Tools` → `Last XNU log (ramoops ECC 0)`
3. 从最后一页开始，音量加/减翻页，电源返回
4. 核对正文是否有 XNU 横幅 `=== TB710FU XNU boot (UniXNU ramoops) ===`
   —— **只有横幅能证明是本次 XNU 日志**；仅"头有效"可能来自之前的 Linux

## 回滚

刷回 `out/lzma-keys/tb710fu-boot-uefi-lzma.signed.img`（`6bcc95a3…`）即可。
