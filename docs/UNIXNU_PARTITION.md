# unixnu 分区：切分、格式化、写入内核

## 本机的现实约束（必须先读）

官方 `scripts/dual_sys/partition.py` 只接受**单层或直接**的 userdata 加密映射。
本机 `device-facts.md` 实测：

```
fileencryption=aes-256-xts:aes-256-cts:v2+inlinecrypt_optimized+wrappedkey_v0
keydirectory=/metadata/vold/metadata_encryption
metadata_encryption=aes-256-xts:wrappedkey_v0
```

这是**多层 + 元数据 + 内联加密**，属于工具明确拒绝的情形
（`dual-system-partition.md`："遇到 … 多层映射 … 时停止"）。

并且 `Total free space is 0 sectors` —— GPT 无空闲区，不能直接加分区。

**因此路径 A（保留数据缩容）在本机很可能不成立。** 需要用户裁决：

| 路径 | 做法 | 代价 |
| --- | --- | --- |
| A 保留数据缩容 | `dual_sys plan/apply` 缩 F2FS 尾部 | 加密映射不满足，工具会拒绝；且需 TWRP 内核支持 `F2FS_IOC_RESIZE_FS` |
| **B 清空重建** | 备份 → 擦除 userdata → 重建 GPT 加 `unixnu` | 丢失 Android 用户数据（需先决定是否还要保留 Android） |

用户此前表示「清空 userdata 重新划分」，即**路径 B**。

## 路径 B 步骤（清空重建）

> 会删除安卓用户数据。执行前确认 Android 数据已备份或不需保留。

### 1. 进入 TWRP（或具备 root 的 recovery）

```powershell
& adb reboot recovery
& adb devices -l
```

### 2. 记录现状（只读）

```powershell
python scripts/dual_sys/partition.py inspect --adb "C:\Program Files (x86)\platform-tools\adb.exe"
```

保存输出：GPT 几何、`userdata` 起止 LBA、加密映射类型。

### 3. 擦除 userdata 并重建 GPT

清空重建不需要 F2FS 缩容，直接：

- 保留 `userdata` 起始 LBA 不变
- 把 `userdata` 的结束 LBA 前移，腾出 `unixnu`
- 新建 `unixnu` 分区项（类型用 `0x8300` Linux，或保持 `FFFF` 通用）

推荐容量：**4 GiB**（内核 137 MiB + 未来 payload/ADT/日志，留足余量）。

> 本仓库**不提供**自动改 GPT 的脚本。清空重建属于设备写操作，需人工确认后
> 用 `sgdisk`/`parted` 在 recovery 内执行，并**先备份两份 GPT**
> （`sgdisk --backup`）。

### 4. 把 FAT 镜像写进 unixnu

已预构建：`out/unixnu-part/unixnu-payload-probe.img`（224 MiB FAT32，
含 `EFI/UniXNU/unixnu.toml` + `Payloads/kernel.macho`，内核读回哈希
`c601ca5c…` 已验证）。

```powershell
# 通过 fastbootd 直接写 unixnu（大小需 >= 224 MiB）
& fastboot flash unixnu "D:\unixnu-tb710fu-clean\out\unixnu-part\unixnu-payload-probe.img"
```

或 recovery 内 `dd`：

```sh
adb push unixnu-payload-probe.img /tmp/
adb shell 'dd if=/tmp/unixnu-payload-probe.img of=/dev/block/by-name/unixnu bs=1M'
```

### 5. 验证

回到 UEFI（槽 b），菜单应出现 `UniXNU (kernel on UFS)`，
并可从 UFS 启动 137 MiB 内核。

UEFI DEBUG 屏幕应能看到：

```
Installed Fat filesystem
config: \EFI\UniXNU\unixnu.toml
```

## 前置依赖

| 项 | 状态 |
| --- | --- |
| UFS 块设备在 UEFI 可见 | **未验证**（run 43692304879 候选待实机） |
| `unixnu` 分区存在 | 未创建 |
| FAT 镜像含内核 | **已完成**：224 MiB FAT32，114,233 簇（超过 EnhancedFatDxe 的 65,525 下限），内核读回 `c601ca5c…` |
| 切分工具离线测试 | 12 项通过；`f2fs-shrink` 自检 PASS；`twrp.sh` 语法 OK |
| `aarch64-linux-gnu-gcc` | **已安装** |
| 设备端 `f2fs-shrink` | **已构建**：`out/f2fs-shrink-aarch64`，848,760 B，AArch64 全静态（无 PT_INTERP），SHA-256 `98322e3273268ee79aeb79047fd7fd46f54e45966ac3b2ac9c7ca6d775b75090` |
| TWRP + root + 可解密 /data | 待确认 |

## 顺序建议

1. 先刷 UFS 候选，确认 UFS 块设备发布（`docs/FLASH_UFS.md`）
2. UFS 可见后再切分区；否则切了也没人读
3. 分区建好、FAT 写入后，再验内核从 UFS 启动
