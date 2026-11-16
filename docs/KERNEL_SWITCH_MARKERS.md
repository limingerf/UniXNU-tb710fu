# TTBR1 切换内标记交付（A）· 2026-11-16

这一份定义了**在 MSR TTBR1_EL1 前后各写一个物理字节**的诊断方法，并验证了补丁机器码。
仓库没有对应的 Android/TWRP 物理标记读回；不能把二维码的阶段号当成这两个字节的实测值。

## 交付

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `out/kernel-switch-markers/vendor_boot.img` | 100,663,296 | `fafb1f967ec1eb0813dc7b6af4fa4a1e77f73876f88390458e7a483d6be51b69` |
| `out/kernel-switch-markers/vbmeta.img` | 8,000 | `b64e33bc852b06e62b4bdc6a12b23cfb1ad803bb43d43f1fbda55eb1223d2669` |

云 run [43819437775](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43819437775)
success，内核 `860834c5…`，artifact 哈希 `ae175f5e…` 与 GitHub digest 一致；
0007 是 `xnu-inputs.json` 第 133/133 项，哈希与仓库一致。

## 标记设计与证据边界

静态设计链条（不是设备观察）：

- helper 写 mark0 / mark1 的指令分别在 TTBR1 写入前后；机器码序列已在内核文件中核对。
- 新根表下返回地址、异常向量和实际页表描述符没有设备读回。
- ⇒ 本交付本身**不能判定**切换函数是否返回。

而这段函数只有 4 条指令，**全部通过 TTBR1 取指**：

```asm
DSB SY
MSR TTBR1_EL1, x0      ; x0 = ARG0 = 新页表
ISB                    ; 让新表对下一条取指生效
RET
```

ISB 之后第一次取指就由新表翻译。如果新表不描述这些代码页，可能在返回前后异常；
这个假设需要物理标记读回或阶段 48/之后的证据验证，不能仅凭 QR 阶段 47 判定。

设计假设是切换只写 TTBR1、不动 TTBR0，因此可经低半区 V=P 映射写标记；但标记地址
`0x93ffff000` 的实际 TTBR0 可达性和实体写入结果尚未读回。普通 C 高半区状态依赖新表，
所以标记仍用字面量汇编固定写入次序。

## 标记设计

物理地址 **`0x93ffff000`** —— 2 GiB 窗口 `0x8c0000000..0x940000000` 的最后一页，
在 `topOfKernelData` 和新页表之上，不会被任何内核分配占用。

| 字节 | 值 | 写入时机 |
| --- | --- | --- |
| `[0x93ffff000]` | `0x01` | MSR TTBR1_EL1 **之前** |
| `[0x93ffff001]` | `0x02` | ISB **之后** |

用一个**字面量汇编块**实现。之前用 C 写两个 volatile 字节，clang 编译成了
`sturb w9, [x8, #-1]`（写到了前一页）并把常量提升到屏障之前——标记完全没夹住
MSR。改为固定汇编后，aarch64 反汇编 8/8 通过。

### 云构建内核里的真实机器码

`probed` 函数被内联，无独立符号；按 `movz/movk` 立即数定位到
`__TEXT_EXEC` 的 `0xfffffff00bb8a6fc`：

```
movz w8, #0xf000
movk x8, #0x3fff, lsl #16      ┐
movk x8, #0x9,    lsl #32      ┘ x8 = 0x93ffff000
movz w9, #1
strb w9, [x8, #0]              ← mark0
dsb sy
msr TTBR1_EL1, x0
isb
movz w9, #2
strb w9, [x8, #1]              ← mark1
b    (回 arm_vm_init)
```

顺序、地址、两条存储都在。

## 门控

`TB710FU_SWITCH_MARKS=1` 才走标记版；普通启动仍执行原来的 4 条指令。
本镜像的 `unixnu.toml` 已带这个参数。

## 验证

| 层 | 结果 |
| --- | --- |
| 补丁链 0001..0007 串行应用 | PASS（0006/0007 同文件但行区间不重叠） |
| stub aarch64 编译 + 反汇编 | 8/8 PASS |
| 云内核中内联机器码顺序 | 已逐条核对 |
| 生产 DarwinBoot 解析器 | PASS |
| 独立 RSA 复算（不调 avbtool） | vbmeta 与 footer 均 PASS |
| 独立 payload 复核 | 内核/ADT/toml 全对 |

## 刷写

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-switch-markers\vendor_boot.img'
& $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-switch-markers\vbmeta.img'
```

## 预期判读（尚无物理读回）

```bash
dd if=/dev/mem bs=1 skip=$((0x93ffff000)) count=2 | od -An -tx1
```

| 读回 | 结论 |
| --- | --- |
| `01 00` | 进入了 stub，但 **MSR/ISB 没完成** → 新页表内容是问题 |
| `01 02` | **切换成功**完成，停点在更后面 |

这些是未来读取到对应字节后的解释，不是当前设备结论。QR8 的阶段 47 在 helper 调用前；
QR8 日志没有包含物理标记值或后续阶段。
| `00 00` | 标记 stub 没被调用（参数没生效） |

这是**物理内存读**，不是内核日志，不依赖 pstore 或 USB。

## 边界

- 诊断镜像，不是功能修复，也不是硬件验收。
- 探针追加 835,584 B、改写 25 处调用点。
- 标记实验只负责把停点夹到一条指令，**本身不修问题**。
- 本轮未刷写、未重启、未改分区。

机器记录：[kernel-switch-markers-build.json](evidence/kernel-switch-markers-build.json)。
