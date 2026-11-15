# 刷入与判断：UFS 候选

设备需处于 **fastbootd**（`fastboot devices` 能看到 `HA2HZAW2`）。

## 第一步：刷 UFS 版本

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'

# 先确认在 fastbootd
& $fb devices

# boot_b：含 UFS 栈的固件
& $fb flash boot_b   "D:\unixnu-tb710fu-clean\out\ufs-keys\tb710fu-boot-uefi-ufs.signed.img"
# vbmeta_b：匹配的描述符集
& $fb flash vbmeta_b "D:\unixnu-tb710fu-clean\out\ufs-keys\vbmeta.img"

# vendor_boot_b 若已是 menu-fix 版本（e1a8029a…）则跳过
& $fb flash vendor_boot_b "D:\unixnu-tb710fu-clean\out\menu-fix\vendor_boot.signed.img"

& $fb --set-active=b
```

## 第二步：观察屏幕（DEBUG 固件直接画屏）

### 情况 A — 菜单出现，多出 UFS 行

UEFI DEBUG 输出会出现类似：

```
UFS INQUIRY ID: <厂商> <型号>
UFS Boot LUN: <n>
Installed Fat filesystem
```

→ UFS 已发布。下一步：切 `unixnu` 分区，把
`out/unixnu-part/unixnu-payload-probe.img` 的内容放进去。

### 情况 B — 黑屏（背光亮但无显示）

ClockDxe 或 RpmhDxe 与板级时钟树冲突。

排查顺序：
1. 从 `Include/DXE.inc` 和 `APRIORI.inc` 删掉 `ClockDxe`，重新构建
2. 仍黑屏则再删 `RpmhDxe`

### 情况 C — 菜单在，但无任何 UFS 行

UFSDxe 没有发布块设备。屏幕可能有：

```
ufs_get_device_info failed <code>
ufs_set_speed failed with return value <n>
```

→ PHY 校准或时钟参数不匹配。这是 giulia 替代集的预期风险，
需要 TB710FU 自己的 UFSDxe（`uefi_a` 提取，尚未完成）。

### 情况 D — 挂死或反复重启

槽 b 连续失败 7 次后 ABL 自动回槽 a。

## 回滚

```powershell
& $fb --set-active=a
```

或用原始三件套刷回槽 b：

```powershell
& $fb flash boot_b        "D:\avbtool-all\backup_20261115_142158\boot.img"
& $fb flash vendor_boot_b "D:\avbtool-all\backup_20261115_142158\vendor_boot.img"
& $fb flash vbmeta_b      "D:\avbtool-all\backup_20261115_142158\vbmeta.img"
```

## 报告要点

请告诉我：

1. 屏幕是否点亮、菜单是否出现
2. 有无 `UFS`、`INQUIRY`、`LUN`、`Fat` 字样，原文摘录
3. 是否自动重启（几次后可回 Android）

据此决定下一步：切 `unixnu` 分区、调整 UFS 链，或转向其他方案。
