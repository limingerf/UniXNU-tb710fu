# LZMA 压缩内核路线：可行性核实（2026-11-15）

## 用户的线索

另一台已上板 macOS 27.3 的移植设备把内核压在 `vendor_boot_a` 里：

- 内核 `kernel.macho` 解压后约 136 MiB（142,688,256 字节）
- 压缩成 `kernel.lzma` 约 24 MiB
- 整张 `vendor_boot_a` 64 MiB，含原厂 ramdisk、设备树、UniXNU 负载
- 启动时 UniXNU 读 FAT 负载，**XnuLz** 解压内核，DarwinBoot 交给 XNU

## 本机数据（实测）

| 项 | 值 |
| --- | --- |
| 我们的 `kernel.macho` | 142,753,792 字节 |
| 对方的内核 | 142,688,256 字节 |
| 差值 | 65,536 字节（64 KiB）→ **几乎同一份构建** |
| 我们内核 LZMA 后 | **23,633,923 字节（22.5 MiB）** |
| 对方报告 | 约 24 MiB → **吻合** |
| vendor_boot ramdisk 可用 | 81.1 MiB |

**22.5 MiB 远小于 81 MiB**，容量问题因此有解。

## 结论：容量问题已解决

用户线索与实测吻合：两台设备的内核几乎同一份构建（差 64 KiB），
LZMA 压缩后都是 ~23 MiB。

### 已产出的压缩包

`out/lzma-kernel/kernel-ufs.lzma`

| 项 | 值 |
| --- | --- |
| 压缩后 | **24,073,889 字节（23.0 MiB）** |
| 原始内核 | 142,753,792 字节（136.1 MiB） |
| SHA-256 | `7d6c8f539914e0f7a1798c2eaf7c36c83c3100a19d4d9092f0ec8e13cb0500c1` |
| 属性字节 | `0x5d`（lc=3 lp=0 pb=2，EDK II 期望值） |
| 字典 | 8 MiB |
| 大小字段 | 142,753,792（**已修正**，见下） |
| 往返解压 | SHA-256 与原始内核完全一致 |

### 字典大小的权衡（实测）

| 字典 | 压缩后 | 比率 |
| --- | --- | --- |
| 64 KiB | 27,750,137 | 19.4% |
| 1 MiB | 25,315,801 | 17.7% |
| **8 MiB** | **24,073,889** | **16.9%** |
| 64 MiB | 23,633,923 | 16.6% |

选 8 MiB：比 64 MiB 字典只多 0.4 MiB，但 `LzmaUefiDecompress` 的 scratch
需求少 56 MiB（scratch 大致等于字典大小，要从 boot services 内存分配）。

### 必须修正的坑

`xz --format=lzma` 把 8 字节未压缩大小写成 `0xFFFFFFFFFFFFFFFF`（"未知"）。
这是合法 LZMA，但 EDK II 的 `LzmaUefiDecompressGetInfo` 会把它当成目标大小，
分配出一个不可能满足的缓冲区。**必须回填真实大小**，本轮已做（脚本
会 pack 结构体第 5 字节偏移的 u64）。

### payload 容量核算

| 项 | 字节 |
| --- | --- |
| `kernel-ufs.lzma` | 24,073,889 |
| `unixnu.toml` | 411 |
| `kernel.macho.adt` | ~13,488 |
| 合计 | ~24,087,788（23.0 MiB） |
| vendor_boot ramdisk 预算 | 85,024,768（81.1 MiB） |
| **余量** | **~58 MiB** |
| 建议 FAT 卷 | 25 MiB |

对比原始 137 MiB 内核装不进 81 MiB 的局面，现在余量充足。

## 上游核实结果

| 检查 | 结果 |
| --- | --- |
| UniXNU `scripts/`、`package/` 里 LZMA 支持 | **无**（只有 `extract-kdk-support.py` 解码 KDK 时用 lzma） |
| `drivers/DarwinBoot` 里 LZMA | **无** |
| DarwinBoot 如何读内核 | `DarwinBoot.c:871` `ReadFile(Root, Request->KernelPath, KERNEL_LIMIT, ...)`，直接按 Mach-O 解析 |
| 上游可移动载入/打包先例 | `make-gpu-addon.py` 用 **zlib** 压缩 addon 文件；`packed_data_ramdisk` 用 `DBRDISK1` 区段表压缩 Data seed |

**结论：XnuLz 不是上游组件**，是那个移植项目自写的 DXE/库（或对
`UniXNURamDiskDxe` 的私有改动，类似 r11 设备把 LZMA payload 交给
`UniXNURamDiskDxe` 展开的做法，见
`docs/zh/devices/r11-appleipodtouch4.md:450`）。

## 因此复现需要什么

1. **LZMA 解压实现**（DXE 侧）。**EDK II 自带且基线已在用**：
   `MdeModulePkg/Library/LzmaCustomDecompressLib`，在
   `Silicon/Silicium/SiliciumPkg/SiliciumPkg.dsc.inc:385` 已作为 NULL 库注册。
   所以解码能力**已经在固件里**，不需要新引入库。

   可用 API（`LzmaDecompressLibInternal.h`）：

   ```c
   RETURN_STATUS EFIAPI LzmaUefiDecompressGetInfo(
     IN  CONST VOID *Source, IN UINT32 SourceSize,
     OUT UINT32 *DestinationSize, OUT UINT32 *ScratchSize);

   RETURN_STATUS EFIAPI LzmaUefiDecompress(
     IN CONST VOID *Source, IN UINTN SourceSize,
     IN OUT VOID *Destination, IN OUT VOID *Scratch);
   ```

2. **解压时机**：在 `kernel.macho` 被 DarwinBoot 读取**之前**把
   `kernel-ufs.lzma` 展开到内存。两种可行位置：
   - UniXNU 菜单项加载前：新增一个"展开到内存卷"的步骤
   - `UniXNURamDiskDxe` 里：照 r11 的做法，把压缩流展开进内核窗口
3. **容量核算**：解压产物 137 MiB **不能写回 FAT**（加压缩包 23 MiB 会超），
   必须**直接解压到 RAM**（r11 正是如此）。

## 与 UFS 路线的对比

| 项 | LZMA 压内核 | UFS 分区 |
| --- | --- | --- |
| 需要分区改动 | 不需要 | 需要切 unixnu |
| 需要新固件组件 | **需要**（解压 + 交付路径） | 已有候选（UFS 栈） |
| 容量 | 23.0 MiB，余量 58 MiB | 不受限 |
| 上游现成 | 解码库已在，**交付路径要自写** | 部分（UFSDxe 来自 giulia） |
| 当前阻塞 | 解压实现 | 实机 UFS 是否发布 |

## 状态与下一步

已完成：

1. `kernel-ufs.lzma` 产出并验证（23.0 MiB，往返 SHA-256 一致，
   大小字段已修正为 EDK II 可读的 142,753,792）
2. 确认 `LzmaCustomDecompressLib` 在 pinned 树可用且**基线已在注册**
3. payload 容量核算（23.0 MiB 内容 / 25 MiB FAT，余量 58 MiB）

待做（需要写固件代码，属设备私有改动）：

4. 实现"解压到 RAM 再交给 DarwinBoot"。最小可行改动点：
   - 新增一个 DXE，读 payload FAT 里的 `kernel-ufs.lzma`，
     用 `LzmaUefiDecompressGetInfo` 取目标大小，`AllocatePages` 一块
     137 MiB 的 **MaxAddress-constrained** 缓冲，解压进去
   - 把解压结果以 `LoadFile`/protocol 或直接把路径交给 DarwinBoot
     （DarwinBoot 现在只接受 FAT 上的 `kernel.macho` 文件路径，
     所以要给它一个"内存里的镜像"入口）
5. 明确边界：这不是上游能力。上游 DarwinBoot 只做
   `ReadFile(Root, Request->KernelPath, KERNEL_LIMIT, ...)` 后按 Mach-O 解析，
   **没有任何解压代码**。声称复现成功前必须有实机证据。

## 关于 XnuLz 的结论

搜索上游 UniXNU、`drivers/DarwinBoot`、`scripts/`、`package/` 均无 "XnuLz"
这个名字，也没有 kernel 的 LZMA 加载路径。可以确定：

- **XnuLz 是那台移植设备项目自有的组件**，不在上游公开代码里
- 上游唯一相近的先例是 r11 设备把 LZMA payload 交给
  `UniXNURamDiskDxe` 展开（`docs/zh/devices/r11-appleipodtouch4.md:450`），
  也是该设备私有改动
- 我们能复用**解码库**（已在固件里），但**交付路径必须自己写**

这一条不能含糊：把 `kernel-ufs.lzma` 放进 payload 只是准备了原料，
在没有解压组件之前，DarwinBoot 会把压缩流当 Mach-O 解析并失败。
