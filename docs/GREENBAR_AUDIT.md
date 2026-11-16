# 绿条停屏审计 · 2026-11-16

用户报告内核启动仍卡在绿条（DarwinBoot 交接带）。本文是对
`out/kernel-pr249-final` 这版交付的独立审计：只用仓库里的实际字节重新推导，
不引用上一轮报告的自证结论。可复现入口：

```powershell
python tools/audit_greenbar_delivery.py --json docs/evidence/greenbar-audit.json
```

## 一、这份交付本身是自洽的

审计先确认交付没被篡改、也没退回到旧状态。全部来自 `out/kernel-pr249-final/*`
的实际读回：

| 检查 | 结果 |
| --- | --- |
| vendor_boot 身份 | 100,663,296 B，`92232f89…`（与记录一致） |
| payload FAT32 可挂载 | 66,512 簇 ≥ 65,525 下限 |
| LZMA 解回内核 | 142,753,792 B，`9daf5064…`（与记录一致） |
| 预链接形态 | MH_FILESET，79 个 fileset 条目，10 个根段 |
| 诊断段 | `__KDIAG` 不存在（确为干净内核） |
| PR249 页池 | 机器码 `movz x2,#22` + `lsl x2,x2,#9` 相邻，确为 22 页 |
| 签名后 vbmeta | 23 条描述符（13 property / 4 hashtree / 3 hash / 3 chain） |

所以“构建产物等于记录、PR249 真的进了机器码”这两点成立。**但它们都不能
证明这版能启动** —— 下面两条才是卡点的证据。

## 二、PR249 容量修复与本机卡点无关（已定量排除）

`0005-native-4k-bootstrap-pool-9g.patch` 把非 Apple 4 KiB 分支的
`BOOTSTRAP_TABLE_SIZE` 从 16 页抬到 22 页。它防护的是
`arm_vm_init()` 里 `#if DEVELOPMENT || DEBUG` 的那段检查：

```c
size_t l1_entries = 1 + ((bytes_mapped + ARM_TT_L1_SIZE - 1) / ARM_TT_L1_SIZE);
size_t pages_used = 2 * (l1_entries + 1);
if (pages_used > BOOTSTRAP_TABLE_SIZE) {
    panic("BOOTSTRAP_TABLE_SIZE too small for memory config");
}
```

用**交付里实际 ADT** 的 `/chosen/darwinboot-memory-size`（`0x80000000`，2 GiB）
代入，`L1` 跨 3 个 1 GiB 区，`pages_used = 8`。旧的 16 页池已经足够。

两个独立佐证：

1. 实机上内核走到了 `set_mmu_ttb_alternate#1`（用户提供的 reporter 4 报告），
   而这段检查在该调用**之前**；它没 panic，就说明 `pages_used <= 16` 成立过。
2. PR249 自己记录的分支也是“2 GiB 窗口所需页数在旧预算内”
   （`docs/KERNEL_PR249.md`）。

因此 PR249 为这块板多留的 24 KiB 永远用不到，**它不是绿条卡点的修复**，
也不能作为“下一个候选就是修好的”的理由。

## 三、真正的缺陷：交付的 ADT 不是 macOS 设备树

这是本轮审计的主要发现。`kernel.macho.adt` 是内核用来匹配平台专家的
设备树侧车；但交付里这一份根节点仍是被审计的硬件 DTS 身份：

| 字段 | 交付值 | macOS 27 profile 声明值 |
| --- | --- | --- |
| `model` | `Qualcomm Technologies, Inc. Pineapple MTP` | `Mac17,11` |
| `compatible[0]` | `qcom,pineapple-mtp` | `K114cAP` |
| `/product` | **0** 个属性 | 18 个 string + 17 个 u32 |
| `/chosen` `osenvironment` | **缺失** | `main` |
| `/chosen` `boot-uuid` | **缺失** | `84581E99-…` |
| `/chosen` `root-matching` | **缺失** | 由 `root_match_volume_uuid` 生成 |
| `/arm-io` 硬件节点 | **0** | （本板 DTS 未导出，见下） |

内核按这份 ADT 找不到任何 macOS 平台专家，也就不会有后续的 MMU/控制台初始化。
这与“绿条之后无任何输出”的实机现象一致。

### 根因

两条叠加，都在 `scripts/unixnu/devicetree.py` 的输入契约上：

1. **缺键直接失败**：`generate()` 里
   ```python
   chip = int(str(options['chip_id']), 16)     # options = payload['adt']
   ```
   只在 `if unique_chip_id:` 分支里执行。只传 `--unique-chip-id` 而不给
   `payload.adt.chip_id`，得到的是 `KeyError: 'chip_id'`（已实测复现）。
2. **没有 profile 就没有身份**：`adt.root` / `adt.chosen` / `adt.product`
   全部来自合并后的配置。少了 macOS 27 profile 的 `[adt]` 块，生成器不会
   报错，只会安静地输出一份 Qualcomm 身份的 ADT —— 也就是本次交付的这一份。

`board.toml` 原来既没有 `[payload.adt] chip_id`，也没有 `owned_memory_size`，
却仍能“构建成功”，因为**所有既有门禁都是拿新 ADT 比旧 ADT 的哈希**
（`package_kernel_diagnostic.py`、`ci_kernel` 的 packaging 报告），
自证一致，从不检查“这份 ADT 像不像一台 Mac”。

### 为什么 /arm-io 是空的（未修，需板级证据）

`devicetree.generate()` 只导出带 `unixnu,adt-name` 属性的节点：

```python
hardware = [e for e in entries if 'unixnu,adt-name' in e.properties and e.enabled()]
```

本板用的 `soc` 侧 DTS 没有任何 `unixnu,adt-name`，所以 `/arm-io` 没有子节点。
这是**尚未完成**的板级移植工作（要从本机 live FDT 派生带 ADT 名称的硬件节点），
不是本轮能凭静态证据补齐的。审计工具把这一项标成 `INFO` 而不是 `PASS`，
避免用“已通过”掩盖缺口。

## 四、已落地的修复

1. **`board.toml` 补 `[payload.adt]`**（本仓库唯一改动的输入）：
   ```toml
   [payload.adt]
   chip_id = "0x6050"
   owned_memory_size = "0x80000000"
   ```
2. **新增门禁 `tools/check_adt_identity.py`**：直接对 ADT 断言
   `model == Mac17,11`、`compatible[0] == K114cAP`、`/product` 非空、
   `/chosen` 具备 `osenvironment`/`boot-uuid`/`apfs-preboot-uuid`/`root-matching`，
   同时确认内存窗口、`chip-id`、`unique-chip-id` 没在修补身份时丢失。
3. **接进打包路径 `tools/package_kernel_diagnostic.py`**：ADT 身份不合格就拒绝
   打包成 boot 镜像，堵住“把旧 ADT 顺带进下一版”的通道。

验证（同一份 `board.toml` + 同一份 macOS 27 profile + 同一份
`.local/adt/adt-input4.dtb`，走真实的配置合并再调用固定生成器）：

| 输入 | 结果 |
| --- | --- |
| 交付的 `kernel.macho.adt` | **7 项身份检查失败**（门禁正确地拒绝） |
| 修补后重新生成 | 14,212 B，`model=Mac17,11`，`compatible=K114cAP`，`/product` 35 项，`/chosen` 23 键，**9 项全 PASS** |

旧 11,536 B 与新 14,212 B 的差就是被丢掉的那部分身份（`/product`、
`/chosen` 的 macOS 键、`root-matching`）。

## 五、边界：这份修复尚未上机

- 本轮**没有**重新 native/prelink、没有云构建、没有签名、没有刷写。
  仓库内没有可用的 GitHub 凭据（`tools/github_admin.py` 需要
  `GH_TOKEN`/`GITHUB_TOKEN` 或 `--env-file`，当前均不存在），
  按 AGENTS.md 也不得把凭据写进仓库。
- 新 ADT 只证明**生成器会正确产出身份**，不代表内核已接受；
  平台专家匹配要实机验证。
- 修好 ADT 后，内核会真正进入平台初始化，届时的卡点可能与现在不同——
  这属于下一阶段，不能用本次结论预判。
- 另有一条独立缺口：`0004-tb710fu-ramoops-console.patch` 把
  `tb710fu_ramoops_init()` 放在 `arm_vm_init()` **返回之后**，而当前卡点就在
  `arm_vm_init` 内部，所以 ramoops/pstore 在本阶段拿不到任何日志。
  要观测本阶段，需要用已有的核内报告器（QR/OneSend）路径，而不是等 pstore。

## 六、下一步（按顺序，都需实机）

1. 用修补后的 `board.toml` 在云上重建 ADT 与 payload，跑
   `tools/check_adt_identity.py` 确认 PASS。
2. 刷入后观察停点是否从“绿条无输出”推进到平台初始化/控制台初始化；
   若推进，说明身份匹配生效。
3. 若仍停住，带报告器变体定位；不要再用 PR249 类容量改动解释该停点。
4. `/arm-io` 的硬件节点导出是独立的板级移植任务，与身份修复分开验收。
