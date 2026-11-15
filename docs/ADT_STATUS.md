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

## 交付三件套

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `out/lzma-keys/tb710fu-boot-uefi-lzma.signed.img` | 100,663,296 | `6b034728de91db493fa3d406e214ce41f47b314a224f88ab41d389206f81d278` |
| `out/lzma-keys/vendor_boot.signed.img` | 100,663,296 | `de52de5da94e8bbc4edd2889e87face950ba308a4dda6614a14703a88d1e2635` |
| `out/lzma-keys/vbmeta.img` | 8,000 | `f9dfebe25a289a8505d11573a5b55d62489c46d43ed9138720ae48cc5d2c84a6` |

vbmeta 的 vendor_boot digest 已交叉校验：`d0751fe8…` 与重算值一致。

## 仍未验证

- ADT 由**host 生成**，未经 XNU 实际消费。`/arm-io` 里的硬件节点是否被
  macOS 27 平台专家全部接受，只有实机能证明。
- `dram-base/size` 取参考板记载的连续 run，未与本机 UEFI 实际交接窗口比对。
- 内核解压到 RAM 后的 4 GiB 窗口约束、EL1 PAC、真实 UFS 均未验。
