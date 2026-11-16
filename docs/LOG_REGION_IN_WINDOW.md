# 窗口内日志区（B）· 2026-11-16

## 问题

`patches/xnu/0004` 的 ramoops 控制台写物理 `0xac340000`。但内核的受管内存
span 是 `[0x8c0000000, 0x940000000)`（2 GiB），`0xac340000` 比 `gPhysBase`
低 **32.3 GiB**。`ml_io_map` 无法描述它，所以一个字节都写不进去——这正是每次
pstore 都空的原因（另一个原因是 0004 的 init 挂在 `arm_vm_init` 返回之后，见边界）。

## 选定区域

| 项 | 值 |
| --- | --- |
| 物理基址 | `0x93FF00000` |
| 大小 | `0x40000`（256 KiB） |
| 区间 | `[0x93FF00000, 0x93FF40000)` |
| 对齐 | 16 KiB |
| 距窗口顶 | 768 KiB |

刻意放在**窗口顶下方 1 MiB**，是为了避开 0007 的恒等标记页 `0x93FFFF000`
（窗口最后一页）。设备 `/proc/iomem` 证实 `8c0000000-9fdccefff : System RAM`，
该地址是真实 DRAM。

### 不重叠核对（`verify_0008_region.py`，7/7 PASS）

| 占用者 | 位置 | 结论 |
| --- | --- | --- |
| 内核镜像 | 物理约 `0x8c0000000..0x8c37dc000` | 日志区远在其上 |
| boot args/DT/ramdisk + 32 MiB head reserve | ≤ `0x8ca8f8000` | 日志区远在其上 |
| `__KDIAG` 段 | 物理约 `0x8c8824000..0x8c88f0000` | 日志区远在其上 |
| 0007 标记页 | `0x93ffff000` | 日志区末端 `0x93ff40000` < 它 |
| `arm_vm_excluded_init` 钳制 | base/end 不变，base<end | 接受 |
| head-reserve panic 检查 | `0x93ff00000` > `topOfKernelData+32MiB` | 不触发 |

## 方案：改 0004 默认 PA（不新增 ADT 属性）

`patches/xnu/0008-tb710fu-ramoops-in-window.patch` 只改
`osfmk/kern/tb710fu_ramoops.c` 的 `tb710fu_ramoops_init()`：

```c
uint64_t pa = 0x93ff00000ULL;      /* 新的默认 */
uint64_t arg_pa = 0;
uint32_t size = 0x40000, ecc = 16;

/* ramoops=<PA> 仍是 override，但只接受 span 内的值 */
if (PE_parse_boot_argn("ramoops", &arg_pa, sizeof(arg_pa)) &&
    arg_pa >= 0x8c0000000ULL &&
    arg_pa + size <= 0x940000000ULL) {
    pa = arg_pa;
}
```

现役 boot args 里的 `ramoops=0xac340000` 会**被判为越界并回退到窗口内默认**，
不会再静默选一个不可达地址。zone 现在默认启用（原来不带 `ramoops=` 就禁用）；
`ramoops_size/ecc/panic_delay/reset_after/reset_warm/selftest` 行为不变。

**为什么不选"新增 ADT 属性"**：那需要改生成器配置 + 在 ramoops 里调
`SecureDTLookupEntry/SecureDTGetProperty`，侵入更大，且与「不改官方脚本」冲突。
0008 只动 `ramoops.c`，而 0005/0006/0007 动 `arm_vm_init.c`，文件集正交，
天然能在 0007 之后干净应用。

## 关于 excluded 表（如实记录，未采用）

`devicetree.py` 的 `owned_memory_excluded_ranges=N` 只发 **全零 16×N 字节表**，
`DarwinBoot.c` 的 `AdtNode` 要求它全零，并在 `DescribeWindow` 里从最终 UEFI
memory map **重算覆写**整张表。也就是说，在「device patch + board.toml，不改
固件/官方脚本」的范围内，**无法把固定的 `(base,size)` 预填进 ADT 并让它存活**。

因此 (B) 不引入 `owned_memory_excluded_ranges`。正确性靠两点：

1. 该区在窗口顶部，远高于 `topOfKernelData + ARM_VM_EXCLUDED_RESERVE(2GiB)=32MiB`
   的启动分配区；VM 的页数组/free list 从底部增长，正常启动不会触及它。
2. ramoops 走 `ml_io_map → io_map → pmap_map/pmap_map_bd` 的**独立设备映射**
   （`osfmk/arm/io_map.c:67-116`），不依赖物理 aperture，也不要求该页在 free list 里。

若将来需要严格保证「VM 永不分配该区」，必须配套固件侧保留（DXE 用
`AllocateAddress` 占住 256 KiB 并标为非 conventional），那是独立的固件改动。

## 边界（必须说清）

`tb710fu_ramoops_init()` 由 0004 挂在 `arm_init` 里 **`arm_vm_init()` 返回之后**。
较早的 guard9/QR8 启动停在 `arm_vm_init` 内部 stage 47，因此当时该函数
不会执行。guard11 后来的 stage56 报告显示调用已返回，但还没有读取日志区
头部来确认初始化及数据写入成功。

捕获该停点靠的是 **0007 的恒等映射标记**（`0x93ffff000` 两个字节）。
0008 解决的是「地址根本不可达」这个独立问题，为**越过切换之后的启动**以及后续
正常启动提供持久日志通道。两者互补，不互相替代。

## 验证

| 项 | 结果 |
| --- | --- |
| 0008 在 0004 之后 `git apply --check` / apply | PASS |
| 0005→0006→0007→0008 补丁链（真实源码树） | 见提交记录 |
| 区域几何/不重叠/钳制 | `verify_0008_region.py` 7/7 PASS |
| 窗口内地址是真实 DRAM | 设备 `/proc/iomem` 已确认 |
| 云构建 + 签名 + 独立验签 | 见 `docs/evidence/kernel-log-region-build.json` |

## 读取

重启进 Android/TWRP 后（若 `CONFIG_STRICT_DEVMEM` 拦截，用 TWRP）：

```bash
# 256 KiB 日志区首部（Linux persistent_ram 头 {sig,start,size}）
dd if=/dev/mem bs=4096 skip=$((0x93ff00000/4096)) count=64 2>/dev/null | od -An -tx1 -N48
```

当前 UEFI QR 查看器仍读取旧的 `0xac340000` 地址，不能用它验证 guard11 的
`0x93ff00000` 区域；见 [UEFI 查看器地址说明](UEFI_LOG_VIEWER.md)。

不再是 pstore 节点，而是直接读物理内存；区首 12 字节是 Linux 兼容的
`{sig=0x43474244, start, size}` 头。

未刷写、未重启、未改设备分区。
