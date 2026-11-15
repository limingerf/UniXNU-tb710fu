# LZMA 内核方案 · 刷入与验证

## 原理

137 MiB 的 macOS 27 内核 LZMA 压成 23 MiB，仍走 vendor_boot 的 payload
ramdisk。DarwinBoot 已打补丁（`0002-darwinboot-lzma-kernel.patch`）：
`ReadFile` 后探测 0x5D LZMA 头，命中则解压到 RAM 再 `ParseKernel`。

## 产物（out/lzma-keys/）

| 文件 | 字节 | SHA-256 | 内容 |
| --- | --- | --- | --- |
| `tb710fu-boot-uefi-lzma.signed.img` | 100,663,296 | `6b034728de91db493fa3d406e214ce41f47b314a224f88ab41d389206f81d278` | 带 LZMA 版 DarwinBoot 的固件 |
| `vendor_boot.signed.img` | 100,663,296 | `4fa06e48b420cf850b8d8e5947602819c25a23cf563986046f4783020d74d843` | 25 MiB payload（含 `kernel-ufs.lzma`） |
| `vbmeta.img` | 8,000 | `e4923cdc38f71e67299b2f7351ba5e04320bd3aad8e81e1339a8f5de8c8d4a26` | 完整描述符，vendor_boot hash 已更新 |

payload FAT 内容（`out/lzma-kernel/payload-lzma.img`）：
`EFI/UniXNU/unixnu.toml` + `EFI/UniXNU/Payloads/kernel-ufs.lzma`
（24,073,889 B，8 MiB 字典，大小字段 142,753,792）。

## 云构建证据

run **43701443770**（commit `9f535e1`），success。
FD `6213685732a506c67845b4e248966029e7db3c60a060eaedbeab3d3dc7e3587c`。
`lzma_kernel.enabled=true, darwinboot_patched=true`。

## 刷入（fastbootd）

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash boot_b        "D:\unixnu-tb710fu-clean\out\lzma-keys\tb710fu-boot-uefi-lzma.signed.img"
& $fb flash vendor_boot_b "D:\unixnu-tb710fu-clean\out\lzma-keys\vendor_boot.signed.img"
& $fb flash vbmeta_b      "D:\unixnu-tb710fu-clean\out\lzma-keys\vbmeta.img"
& $fb --set-active=b
```

## 预期

UEFI 菜单出现 `UniXNU (LZMA kernel)` 项；选中后 DarwinBoot 打印：

```
DARWINBOOT: LZMA kernel 24073889 -> 142753792 bytes
```

然后走原有 `DARWINBOOT:` 交接。若无此行使绿带/报错，说明解压分支未命中
（检查 payload 里是否真是 `kernel-ufs.lzma` 而非 `kernel.macho`）。

## 回滚

```powershell
& $fb --set-active=a
```

或刷回 `D:\avbtool-all\backup_20261115_142158\` 的原始三件套。

## 边界

- DarwinBoot 补丁是设备私有改动，上游无 LZMA 内核路径。
- 编译+链接+入卷已云验证，**未实机**。解压到 RAM、内核窗口 4 GiB 约束、
  实际跳转 XNU 均需实机证据。
