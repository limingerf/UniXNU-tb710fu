# 切换后探针交付 · 2026-11-16

第一份**内核本身带 0006** 的可刷镜像。前几次上机用的都是旧内核，所以
`set_mmu_ttb_alternate#1` 之后什么都看不到——那是探针段没有映射，不是内核停了。

## 交付

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `out/kernel-diag-past-switch/vendor_boot.img` | 100,663,296 | `2c35ccc4ce015c15f2d5949397d058f9c7e467abb396d6c71dc88909b506a260` |
| `out/kernel-diag-past-switch/vbmeta.img` | 8,000 | `dc03f3b394a1d9c67d160b048dd89f1e91228a11f5e867c91be4dc1544403083` |

## 云构建

[run 43806716346](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43806716346)
success，`stage=prelink`，head `af96168`。

| 项 | 值 |
| --- | --- |
| artifact | `tb710fu-prelink-43806716346`，37,726,567 B |
| 下载哈希 | `32820c75…` **等于 GitHub digest** |
| 内核 | 142,753,792 B，`6adc7e15…` |
| 0006 在 `xnu-inputs.json` | `85a04e1c…`，与仓库文件**一致**，第 132/132 项 |

## 0006 真的编进去了（不是只在清单里）

“补丁列在输入里”和“补丁被编译”是两件事，所以我查了实际字节：

| 检查 | 结果 |
| --- | --- |
| `tb-diag-segment` 字符串 | 新内核 **1** 处，基线 **0** 处 |
| `_arm_vm_prot_init` 引用该字符串 | 新内核 **1** 处，基线 **0** 处 |
| `_arm_vm_prot_init` 首处差异 | 距函数入口 `+0x14` |
| 切换点位置 | `…#a694` → `…#a71c`（移动 `0x88`） |

### 一个我特意查过、容易误判的现象

**内核总字节数完全没变**（仍 142,753,792 B）。若只看大小，会误判成“构建用了
缓存、补丁没进去”。实际是：

- **7,297,495 字节不同**
- **5,650 个符号位移**（`+0x10`，正是新增 16 字节字符串所致）
- 增长被各段既有填充余量吸收了

## 探针：第一次越过切换点

| 项 | 值 |
| --- | --- |
| 探针内核 | `91b078f4…`（143,589,376 B） |
| 追踪点 | **25 个**（原 24 个 + 新增 1 个） |
| 新增点 | `vm/set_mmu_ttb#1` @ `…#a730`，在切换点 `…#a71c` **之后** |
| 位置 | 25 个中的最后一个 |
| 报文 | 25 个 enter + 25 个 return，与追踪点一一对应 |

**为什么现在才敢放到切换之后**：`prepare_early_report.py` 原本有一条硬限制
「切换后不得进入附加段」，因为段没有映射。0006 在 `arm_vm_prot_init`（切换之前）
按 `tb-diag-segment=` 把段映射成 RX，这条限制才有条件放开。工具里加了
`--trace-past-switch`，必须显式开启，默认行为不变。

## 两侧对齐检查

参数写错一个字母就会静默失效，和上一张图表现完全一样，所以逐项核对：

- `unixnu.toml` 传入：`tb-diag-segment=__KDIAG`
- 内核里有 `__KDIAG` 段（`0xfffffff00f828000`，835,584 B，位于文件末尾）
- 内核里有 `tb-diag-segment` 字符串

三者一致。

## 验证

| 层 | 结果 |
| --- | --- |
| 签名脚本自报 | PASS，22 个其它描述符与公钥不变 |
| **独立 RSA 复算**（不调 avbtool） | vbmeta 与 footer 内嵌**两处均 PASS** |
| 独立 payload 复核 | 内核 `91b078f4…`、ADT `f4161ff9…`、toml `2a5d399d…` 全对 |
| 生产 DarwinBoot 解析器 | 控制组与探针**均 PASS** |

## 刷写

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-diag-past-switch\vendor_boot.img'
& $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-diag-past-switch\vbmeta.img'
```

保留 `boot_b`。刷后选**原来的 XNU 项**（不是 Export VM checkpoint）。

## 判读

追踪顺序共 25 点，末尾三个是：

```
vm/qsort#1
vm/set_mmu_ttb_alternate#1     ← 以前只能到这里
vm/set_mmu_ttb#1               ← 现在能看到的第一个"切换之后"
```

| 最后显示 | 含义 |
| --- | --- |
| `vm/set_mmu_ttb_alternate#1` | 连 `set_mmu_ttb` 都没进去 → 切换本身挂了（说明 0006 的映射没生效，或切换真的失败） |
| `vm/set_mmu_ttb#1` | **切换成功**，停点在 `set_mmu_ttb` 或之后 |

这一条就能把「切换失败」和「切换后失败」彻底分开——之前所有版本都做不到。

## 并行线 (b)：已证不可行

ADT 窗口调整**无法**让 ramoops 地址可达：

| 方案 | 结论 |
| --- | --- |
| excluded 加 ramoops 地址 | **不可能**。解析器把条目裁到 span 内，地址在 span 之下 → 可表达字节数 **0** |
| 调大窗口 | **不可能**。span 起点比 ramoops 高 32.3 GiB，加长够不到自己下方 |
| 窗口下移 | 等于重做整个移植 |

可行替代是**在 span 内部留日志区**（有 1879.9 MiB 余量），但写入时机仍受限于 VM
就绪，拿不到 `arm_vm_init` 内部停点——那正是 0006 探针负责的部分。

## 边界

- 诊断镜像，不是功能修复，也不是硬件验收。
- 探针追加 835,584 B、改写 25 处调用点，布局位移是可预期的。
- 切换后那个追踪点**只有在带 `tb-diag-segment=__KDIAG` 启动时才安全**，本图已包含。
- 本轮未刷写、未重启、未改分区。

机器记录：[kernel-diag-past-switch-build.json](evidence/kernel-diag-past-switch-build.json)。
