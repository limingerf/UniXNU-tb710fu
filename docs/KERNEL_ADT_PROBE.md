# ADT + 二维码探针交付 · 2026-11-16

用你设备上**已经验证过**的 OneSend/QR 链路，挂到 PR249 内核上，一次上机看清
`arm_vm_init` 走到哪一步。同时包含 ADT 身份修复。

## 为什么 ramoops 一直是空的（已定位，不是故障）

`patches/xnu/0004-tb710fu-ramoops-console.patch` 在 `arm_init` 里是这么排的：

```c
arm_vm_init(xmaxmem, args);   /* 第 586 行：卡点在这里面 */
tb710fu_ramoops_init();       /* 第 16 行：init 在它返回之后才执行 */
```

而 init 之前，`tb710fu_ramoops_putc()` 只做这件事：

```c
if (zone != NULL) { ramoops_zone_putc(c); }
else if (early_len < sizeof(early)) { early[early_len++] = c; }   /* 4096 B 内存缓冲 */
```

`zone` 是 NULL，所以**一个字节都没写进持久区**，只进了 RAM 里的 `early` 缓冲，
而这个缓冲要等 init 成功才会被 flush 出去。

**结论**：卡点在 `arm_vm_init` 内部 → init 从未执行 → ramoops 区从未被写。
空 pstore 是**必然结果**，不能用来判断停在哪。

## 交付

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `out/kernel-adt-probe/vendor_boot.img` | 100,663,296 | `394fdd82a7c4aa9055bada25a3349728ad25120cc8007e6946820ec10c5e9aa3` |
| `out/kernel-adt-probe/vbmeta.img` | 8,000 | `366332ed4819d519d81bb2a3e8def56bf6cebb82656b01365ada26c7a7c1975f` |

这一份 = **PR249 内核 + macOS 身份 ADT + 二维码探针**。

## 改动范围（逐条可查）

| 项 | 值 |
| --- | --- |
| 探针内核 | `b30f2191…`（143,589,376 B） |
| 追加段 | `__KDIAG` @ `0xfffffff00f828000`，835,584 B，位于文件末尾 |
| 共享前缀变更 | **403 字节 / 26 个区块**，最大区块 120 B |
| 重写的 BL 调用点 | 24 个，与 `call_trace` 逐条对应 |
| 原有 10 个段 | 名称/vm/大小**全未变** |
| 主动停机 | **无**（`checkpoint_installed: false`，继续原调用序列） |
| ADT | `f4161ff9…`（macOS 身份，未变） |
| `unixnu.toml` | 未变 |

## 保真回归：先证明我没改坏原行为

放宽 `prepare_early_report.py` 的内核哈希白名单时，我先用它**重新处理旧的
`cc690c9a`**，要求输出与历史 reporter 逐字节一致：

```
cc690c9a -> f7a425a7ef9c08635618891ba665dba8b93c0712ddfdc79b47cf1bc9f74c6425
历史 reporter 产物 -> 同一哈希 ✓
```

所以放宽限制没有改变原有行为，改的只是“接受哪些输入”，并且现在必须用
`--expect-kernel-sha256` 显式点名。

## 验证

- **生产 DarwinBoot 解析器**：对原内核与探针内核**都 PASS**
- **签名**：22 个其它描述符与公钥不变
- **独立 RSA 复算**（不调 avbtool）：vbmeta 与 footer 内嵌两处均 PASS
- **payload 独立复核**：解回 `b30f2191…`，ADT/toml 哈希与预期一致

## 刷写

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-adt-probe\vendor_boot.img'
& $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-adt-probe\vbmeta.img'
```

保留 `boot_b` 不动。刷后选**原来的 XNU 项**（不要选 Export VM checkpoint，
那一项才是故意暂停的变体）。

## 怎么读结果

屏幕顶部会出现阶段文字 + 二维码，用 OneSend 扫。**最后成功显示的那一条就是
最后跑到的位置**，它有四种可能的落点：

| 最后显示 | 含义 |
| --- | --- |
| `XNU enter: vm/early_random` | 卡在进入 `arm_vm_init` 的第一小步 |
| `XNU enter: vm/vm_set_page_size` 之类 | 卡在该函数，页表构造中段 |
| `XNU enter: vm/_arm_vm_prot_init` | 页表保护初始化 |
| `XNU enter: vm/set_mmu_ttb_alternate#1` | `arm_vm_init` 所有被跟踪调用都进去了，停点在 TTBR1 切换处或其后 |

注意：`arm_vm_init` 内共 10 个跟踪点、`arm_init` 内 14 个，编号越大越靠后。

## 边界

- 这是**诊断镜像**，不是功能修复，也不是硬件验收。
- 追加段 + 24 处 BL 改写会扰动布局；与原版对比时要记得这一点。
- ADT 身份修复包含在内，但它对停点的影响**仍未证明**。
- 本轮未刷写、未重启、未改分区。

机器记录：[kernel-adt-probe-build.json](evidence/kernel-adt-probe-build.json)。
