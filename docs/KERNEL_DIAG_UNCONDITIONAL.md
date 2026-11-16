# 无参数段映射交付 · 2026-11-16

上一版把段映射挂在 `tb-diag-segment=` 引导参数上。**设备始终没打印
`vm/set_mmu_ttb#1`**，说明那个参数没到达内核，映射静默地从未执行。

这一版把参数依赖彻底删掉。

## 交付

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `out/kernel-diag-unconditional/vendor_boot.img` | 100,663,296 | `67b19c55f27f785232910ba533637805ef878b799bfee861c96b2a12e3b1f1d6` |
| `out/kernel-diag-unconditional/vbmeta.img` | 8,000 | `759bd40defd7c932a59e989789a1763f7efb5cc3e7428fb6ff3ccea689a7006d` |

## 先证明确实是参数没到

反汇编上一版内核的门控，编译器生成的守卫是**正确**的：

```
0xfffffff00bb89298  bl   PE_parse_boot_argn      ← 参数名在 x0
0xfffffff00bb8929c  34000200  cbz w0, +0x40       ← 返回 0 就跳过整块
0xfffffff00bb892b4  bl   getsegdatafromheader
0xfffffff00bb892d8  bl   arm_vm_page_granular_prot
```

门控写法没问题，所以唯一可能失败的原因就是**参数没进来**。
与其继续追参数在哪丢失，直接改变做法。

## 改法与安全边界

```c
unsigned long diag_size = 0;
vm_offset_t diag_base = (vm_offset_t)getsegdatafromheader(
    &_mh_execute_header, "__KDIAG", &diag_size);
if (diag_base && diag_size) {
    arm_vm_page_granular_ROX(diag_base, diag_size, 0, ARM64_PAGE_UNGUARDED);
}
```

**安全性来自内核自身**：`getsegdatafromheader()` 对不存在的段名返回 `NULL` 并把
`size` 置 0，而代码要求两者都非零。所以没有探针段的普通内核**一个字节都不映射**。

### 五种情形全部实测（`compile_check_v3.py`）

| 情形 | 映射次数 | 结论 |
| --- | --- | --- |
| 普通内核，无 `__KDIAG` | **0** | 行为完全不变 |
| 有 `__KDIAG` | **1** | 映射 `0xfffffff00f828000 + 0xcc000` |
| 有 `__KDIAG` 但 vmaddr 为 0 | 0 | 安全 |
| 有 `__KDIAG` 但 size 为 0 | 0 | 安全 |
| 只有别的段 | 0 | 安全 |

## 云构建

[run 43813385537](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43813385537)
success，head `fbe75be`，artifact `555e2055…` **与 GitHub digest 一致**。

新内核 `6c71e39c`（142,753,792 B），验证补丁以无参数形式编入：

| 检查 | 结果 |
| --- | --- |
| `tb-diag-segment` 字符串 | **0 处**（门控已彻底移除） |
| `__KDIAG` 字符串 | 1 处 |
| `_arm_vm_prot_init` 内 `getsegdatafromheader` 调用 | 25 处 |

## 探针与 payload

- 探针内核 `adf688f5`，追踪点 **25 个**，最后一个是 `vm/set_mmu_ttb#1`
- payload `ce4ff0a4`，FAT 66,512 簇
- ADT `f4161ff9`，**从成品 payload 里直接读出**确认：`model=Mac17,11`、
  `compatible=K114cAP`、`/product` 35 项、`/chosen` 三项齐全
- **引导参数已清理**：`tb-diag-segment` 已移除，新内核不依赖它

## 验证

| 层 | 结果 |
| --- | --- |
| 签名自报 | PASS，22 个其它描述符与公钥不变 |
| **独立 RSA 复算**（不调 avbtool） | vbmeta 与 footer 内嵌**均 PASS** |
| 独立 payload 复核 | 内核/ADT/toml 全对 |
| 生产 DarwinBoot 解析器 | PASS |
| 成品 ADT 身份 | 直接解出 `Mac17,11` |

## 刷写

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-diag-unconditional\vendor_boot.img'
& $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-diag-unconditional\vbmeta.img'
```

选原来的 XNU 项。

## 这次怎么判读

引导参数**已不再是变量**，所以只剩两种结果：

| 现象 | 含义 |
| --- | --- |
| 出现 `vm/set_mmu_ttb#1` | **TTBR1 切换成功**，内核越过了切换点继续执行 —— 这是第一次能观测到 |
| 仍停在 `set_mmu_ttb_alternate#1` | 映射执行了但没让段可执行，指向 `arm_vm_page_granular_ROX` 对这段空隙的处理，而不是参数问题 |

## 边界

- 诊断镜像，不是功能修复，也不是硬件验收。
- 探针追加 835,584 B、改写 25 处调用点，布局位移可预期。
- 未刷写、未重启、未改分区。

机器记录：[kernel-diag-unconditional-build.json](evidence/kernel-diag-unconditional-build.json)。
