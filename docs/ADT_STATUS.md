# ADT 侧车：已生成 · 2026-11-15

## 结果

`kernel.macho.adt`：11,536 字节，
SHA-256 `4c56fb2a23d9177aa6b5820d3aba46ca66db90f8168bb6520bdd598ba097bc7b`。

由**本机真实 live FDT** 派生，逐项补齐 UniXNU `generate()` 的硬性要求。

## 派生过程（每一步的失败与修复）

起点：设备 `/sys/firmware/fdt`（980,994 字节，`dd`+`adb pull` 完整取回，
SHA-256 `bba7ae3636a43ac4c3506327212ed8378a9f043b092c45883d592124c4bc557e`）。

| 步骤 | `generate()` 的报错 | 修复 |
| --- | --- | --- |
| 1 | `ADT handoff requires one contiguous DRAM range` | live FDT 的 `/memory` 有 16 个 bank；`dtc` 反编译后把 reg 换成参考板 DTS 记载的最大连续 run `0x8C0000000 + 0x140000000`（该 run 本来就在 reg 列表里） |
| 2 | `cpus: timebase-frequency requires one cell` | 固件 FDT 省略了它（Linux 从 arch timer 推导）；补 `19200000`（SM8650 计数频率，与 UniXNU `sm8650.dtsi` 一致） |
| 3 | `cpu-map: CPU affinity must fit a u32` | `/cpus/cpu-map` 是 Linux 拓扑节点、两 cell reg；删除该节点 |
| 4 | `Multiple CPUs require a PSCI method` | 固件 FDT 无顶层 `/psci`；按 UniXNU `sm8650.dtsi` 补 `compatible = "arm,psci-1.0"`、`method = "smc"` |
| 5 | — | **成功**，产出 11,536 字节 ADT |

## ADT 内容核对

从字节解析（`devicetree.py:13` 的 node 布局：`<II prop_count, child_count>` +
每属性 `<32s key, u32 len, data>`）：

| 属性 | 值 | 依据 |
| --- | --- | --- |
| `compatible` | `qcom,pineapple-mtp` … | 本机 live FDT 根节点 |
| `unique-chip-id` | `0x30c18713` | 设备 serial 817989395 |
| `dram-base` | `0x8c0000000` | 参考板 DTS 最大连续 DDR run |
| `dram-size` | `0x140000000`（5 GiB） | 同上 |
| `darwinboot-memory-size` | `0x80000000`（2 GiB） | `[payload.adt] owned_memory_size` |
| `random-seed` | 256 B | xnu-12377 要求 4×64 |
| `nvram-bank-size` | `0x2000` | UniXNU 默认 |

根节点 `device-tree`，7 个子节点（含 `chosen`、`cpus`、`arm-io`）。

## 配置参数（本次采用）

| 参数 | 值 | 来源 |
| --- | --- | --- |
| `chip_id` | `6050`（Mac17,11 T6050） | 用户确认 + 官方 macOS 27 文档 |
| `owned_memory_size` | `0x80000000` | 参考移植同机型值 |
| `unique_chip_id` | `0x30c18713` | 设备 `/sys/devices/soc0/serial_number` |

## payload 布局（已验证读回一致）

```
EFI/UniXNU/unixnu.toml            411 B
EFI/UniXNU/Payloads/kernel.macho        24,073,889 B  ← 实为 LZMA 流
EFI/UniXNU/Payloads/kernel.macho.adt        11,536 B
```

内核以 `kernel.macho` 命名（toml 的 `path` 指向它，DarwinBoot 的 `.adt`
侧车也因此解析为 `kernel.macho.adt`），但内容是 LZMA 压缩流——
DarwinBoot 补丁的 `IsLzmaKernel()` 探测 0x5D 头后解压。

## 首次实机结果：菜单没有 xnu（已定位并修复）

第一次刷入后菜单只有内置动作（Return / Restart / Power off / Tools），没有
`xnu` 条目。原因不是内核也不是 ADT，而是 **payload 卷本身挂载失败**。

### 根因：FAT32 簇数低于固件下限

| 卷 | 簇数 | FAT 类型 | 结果 |
| --- | --- | --- | --- |
| 25 MiB（首次） | **50,380** | FAT32 | **EnhancedFatDxe 拒绝挂载** |
| 33 MiB（修复） | **66,512** | FAT32 | 可挂载 |

`mformat -F` 强制 FAT32，但 25 MiB 只产生 50,380 簇，低于 FAT32 的 65,525
下限。固件拒绝挂载 → UniXNU 读不到 `unixnu.toml` → 回退内置配置。屏幕上
表现为"没有 xnu 条目"，与"内核缺失"无法区分。

### 修复

payload 重建为 **33 MiB FAT32（66,512 簇）**，仍在 81 MiB 预算内
（余量 48.1 MiB）。toml / LZMA 内核 / ADT 三份文件读回全部逐字节一致。

新增 `tools/check_payload_fat.py`：从 BPB 重算簇数，按 EnhancedFatDxe 的
FAT 类型规则判定，可选断言指定 8.3 目录项存在。已双向验证——33 MiB 通过，
25 MiB 报出确切原因。

## 交付三件套

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `out/lzma-keys/tb710fu-boot-uefi-lzma.signed.img` | 100,663,296 | `6b034728de91db493fa3d406e214ce41f47b314a224f88ab41d389206f81d278` |
| `out/lzma-keys/vendor_boot.signed.img` | 100,663,296 | `4e296d5fc077ee02f4d173d676dc86669ca65f7eb419f1acf9131872cbf4e5bc` |
| `out/lzma-keys/vbmeta.img` | 8,000 | `1e621fc579226849569701a8011a3bb3107130a9ac7ce31840a011f68670da31` |

vbmeta 的 vendor_boot digest 已交叉校验：`ee686b39…` 与重算值一致。

### 刷入前审计（14 项全 PASS）

- boot.img：magic / v4 / ramdisk=0 / 96 MiB / AVB footer，内嵌 FD
  `62136857…` 与 CI run 43701443770 完全一致
- vendor_boot：magic / v4 page=4096 / ramdisk 34,603,008 @0x2400000 / 字节一致
- payload FAT：66,512 簇 / 三个目录项齐备
- vbmeta：23 条描述符 / testkey / SHA256_RSA4096

## 仍未验证

- ADT 由**host 生成**，未经 XNU 实际消费。`/arm-io` 里的硬件节点是否被
  macOS 27 平台专家全部接受，只有实机能证明。
- `dram-base/size` 取参考板记载的连续 run，未与本机 UEFI 实际交接窗口比对。
- 内核解压到 RAM 后的 4 GiB 窗口约束、EL1 PAC、真实 UFS 均未验。
