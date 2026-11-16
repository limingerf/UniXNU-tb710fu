# 窗口内日志区交付（B）· 2026-11-16

## 交付

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `out/kernel-log-region/vendor_boot.img` | 100,663,296 | `15846226236b56bd3abb104ae57f45e23307f3ea52321fd03ed153fd2e4e0718` |
| `out/kernel-log-region/vbmeta.img` | 8,000 | `e4cffaf3414a2c22d6c129ed62ac382424f67da8a0dd62aa9ae0ccb73fec63de` |

云 run [43827454902](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43827454902)
success，内核 `4ad08f1b…`，0008 是 `xnu-inputs.json` 第 134/134 项且哈希一致。
这份镜像**同时含 0007 标记 + 0008 日志区**，两个诊断都在。

## 修了什么

`0004` 的 ramoops 写物理 `0xac340000`，比 `gPhysBase=0x8c0000000` 低 32.3 GiB，
在 2 GiB 受管 span 之外，`ml_io_map` 根本描述不了 → 一个字节都写不进去。

`0008` 把默认区改到窗口内：

| 项 | 值 |
| --- | --- |
| 物理基址 | `0x93FF00000` |
| 大小 | 256 KiB |
| 区间 | `[0x93FF00000, 0x93FF40000)` |
| 距窗口顶 | 768 KiB（刻意避开 0007 标记页 `0x93FFFF000`） |

`ramoops=<PA>` 仍是 override，但**只接受 span 内地址**；现役的
`0xac340000` 会被判越界并回退到窗口内默认。本镜像已把那个旧参数删掉，
直接用默认。zone 现在默认启用。

### 机器码确认（不是只看源码）

反汇编 `tb710fu_ramoops_init`：

```
d2a7fe08  movz x8, #0x3ff0, lsl #16
f2c00128  movk x8, #0x9,    lsl #32   → x8 = 0x93ff00000 (LOG_REGION_PA)
f2c0012b  movk x11,#0x9,    lsl #32   → x11 = 0x940000001 (SPAN_END 比较值)
52a00089  movz w9, #0x4, lsl #16      → 0x40000 (LOG_REGION_SIZE)
```

三个常量都在。区域几何 7/7 PASS：在 span 内、16 KiB 对齐、在内核镜像与
head reserve 之上、避开 0007 标记页、`arm_vm_excluded_init` 钳制接受、不触发
reserve panic。设备 `/proc/iomem` 也确认 `8c0000000-9fdccefff : System RAM`。

## 一个如实记录的设计约束

曾考虑用 ADT 的 `darwinboot-memory-excluded` 把该区正式排除，但查清后**放弃**：
`devicetree.py` 只发全零表，`DarwinBoot.c` 要求它全零，并在 `DescribeWindow`
里从最终 UEFI memory map **重算覆写**。也就是说，在「不改固件/官方脚本」范围内，
**预填的 `(base,size)` 无法存活**。

所以 (B) 不引入该表。正确性靠：区域远在 `topOfKernelData+32MiB` 之上，VM 的
free list 从底部增长；且 ramoops 走 `ml_io_map → io_map → pmap_map` 的独立设备
映射，不依赖物理 aperture。若将来要硬性保证「VM 永不分配」，需要固件侧
`AllocateAddress` 保留，那是独立改动。

## 边界

**当前故障启动里这块日志区仍是空的**——`tb710fu_ramoops_init()` 挂在
`arm_vm_init()` **返回之后**，而设备停在该函数内部 stage 47。捕获那个停点靠
**0007 的恒等映射标记**（`0x93ffff000` 两字节），本镜像也保留了
`TB710FU_SWITCH_MARKS=1`。0008 解决的是「地址根本可达」这个独立问题，为
越过切换之后的启动提供持久日志通道。

## 验证

| 层 | 结果 |
| --- | --- |
| 补丁链 0005→0008 串行应用 | PASS |
| 区域几何 / 不重叠 / 钳制 | 7/7 PASS |
| 云内核常量反汇编 | PA / SPAN_END / SIZE 全在 |
| 生产 DarwinBoot 解析器 | PASS |
| 独立 RSA 复算 | vbmeta 与 footer 均 PASS |
| 独立 payload 复核 | 内核/ADT/toml 全对 |

## 刷写与读取

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-log-region\vendor_boot.img'
& $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-log-region\vbmeta.img'
```

重启进 TWRP（Android 常因 `CONFIG_STRICT_DEVMEM` 拦 `/dev/mem`）后：

```bash
# 0007 切换标记
devmem 0x93ffff000 8
# 0008 日志区头部（Linux persistent_ram 头 {sig,start,size}）
dd if=/dev/mem bs=4096 skip=$((0x93ff00000/4096)) count=64 2>/dev/null | od -An -tx1 -N48
```

机器记录：[kernel-log-region-build.json](evidence/kernel-log-region-build.json)，
设计依据：[LOG_REGION_IN_WINDOW.md](LOG_REGION_IN_WINDOW.md)。
