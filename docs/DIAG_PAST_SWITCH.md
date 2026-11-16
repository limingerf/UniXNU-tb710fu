# 切换后探针 · (a3) 交付 · 2026-11-16

目标：让二维码探针在 `set_mmu_ttb_alternate` **之后**仍能出图，从而把停点从
"最后一行是观测边界"推进到"真的停在哪条指令"。

## 结论先行：之前那条最后一行不是故障点

两件事都在本轮用真实数值证实：

1. **新页表下切换路径完全可执行。** 用 T1SZ 软件遍历 `0x8c88f9000` 这张根表
   （该地址由模型精确复现了你设备报的 `ARG0`）：

   | 地址 | 映射 | EL1 可执行 |
   | --- | --- | --- |
   | `_set_mmu_ttb_alternate` | ✅ | ✅ |
   | 返回点 `arm_vm_init+0x11e4` | ✅ | ✅ |
   | `_ExceptionVectorsBase` | ✅ | ✅ |

   **"切换后返回地址不可执行"这条假设被否定。**

2. **探针段根本没有映射。** `__KDIAG` 紧贴 `__BOOTDATA` 尾部，落在
   `__BOOTDATA` 与 `dynamic_memory_begin` 之间的 **212 页空隙**里，逐段核对
   结果是 `named segments overlapping __KDIAG: NONE`。

所以 `XNU enter: vm/set_mmu_ttb_alternate#1` 之所以是最后一行，是因为
**报告器按设计不再返回自己的段**（`prepare_early_report.py` 原文：
"No return into the appended segment after arm_vm_init changes TTBR1"），
而不是内核停在那里。**那是观测边界，不是故障 PC。**

## 补丁

`patches/xnu/0006-tb710fu-diag-segment-rx.patch`，已加入 `patches/xnu/series`。

在 `arm_vm_prot_init()` 里、与其他 ROX 映射并列处（切换之前）加入：

```c
{
    char diag_segment[32];
    if (PE_parse_boot_argn("tb-diag-segment", diag_segment,
        sizeof(diag_segment))) {
        unsigned long diag_size = 0;
        vm_offset_t diag_base = (vm_offset_t)getsegdatafromheader(
            &_mh_execute_header, diag_segment, &diag_size);
        if (diag_base && diag_size) {
            arm_vm_page_granular_ROX(diag_base, diag_size, 0,
                ARM64_PAGE_UNGUARDED);
        }
    }
}
```

### 为什么不用编译宏

要让 `#ifdef` 生效必须改 `scripts/build-xnu-4k-baseline.sh` 把
`CONFIG_XNU_*` 转成 `-D`，而 AGENTS.md 明确禁止编辑固定官方脚本。
（本轮我曾误改该文件，发现后已回退，并逐字节验证与 HEAD 一致：
`490e362d…`。）

因此改成**引导参数门控**：没有 `tb-diag-segment=` 就不映射任何东西。

### 默认启动完全惰性（已验证）

用真实补丁块跑行为矩阵，`PE_parse_boot_argn` 用桩替代：

| 场景 | ROX 映射 | 段查找 |
| --- | --- | --- |
| 默认启动，无该参数 | **0** | **0**（连查找都不做） |
| 参数指向存在的段 | 1 | 1 |
| 参数指向不存在的段 | 0 | 1 |
| 参数指向零长度段 | 0 | 1 |

无参数时**一次映射都不会发生**，`getsegdatafromheader` 都不会被调用。

## 验证

| 项 | 结果 |
| --- | --- |
| 单补丁 `git apply --check` | PASS（上下文从真实源码行生成，非手抄） |
| 与 0005 串行应用 | PASS，两者改动文件不相交（`pmap.h` vs `arm_vm_init.c`） |
| 与 0005 的应用顺序 | 无关（互不重叠） |
| 编译（clang -Wall -Wextra -Werror） | PASS |
| 默认路径行为 | 惰性，映射 0 次（见上表） |

注：`.work/series-test` 不是原始树，它已含 0001–0004，所以"整串从零应用"
无法在本机复现；本轮验证的是 **0005 → 0006 在其之上的串行应用**，这正是构建
实际到达 0006 时的状态。

## 下一步：必须重新 native + prelink

这个补丁改的是 `arm_vm_init.o`，**必须重新编译整个 XNU**，不能只改预链接产物。

流程：

1. 云 run 一次 native → strict prelink（用之前验证过的 `macos-kernel.yml`）；
2. 本机把 `prepare_early_report.py` 套到新内核上生成探针；
3. 打包时在 `unixnu.toml` 的 args 里加 `tb-diag-segment=__KDIAG`；
4. 签名 + 独立验签（流程已就绪）。

## 边界

- 这是**诊断补丁**，不是功能修复。
- 云端构建尚未发起；本轮只完成补丁与本地全部验证。
- 加了参数的那个镜像会比现在多映射一段 RX，其余行为不变。
- 未刷写、未重启、未改分区。
