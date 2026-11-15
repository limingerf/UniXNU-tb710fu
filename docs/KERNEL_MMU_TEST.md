# XNU 开启 MMU 后与 C 入口诊断

用户已报告上一版出现白＋蓝＋紫三条：XNU 入口和开启 MMU 前的探针确实执行。
此前代码没有后续显示标记，不能仅凭停在这三个颜色就断定 MMU 开启失败。
这一结果同时表明启动汇编已走过 TCR/TTBR/MAIR 设置和页表构建逻辑，
但不证明这些寄存器或页表内容正确。

## 本次检查点

保留顶部白条、第二条蓝色、第三条紫色，再增加：

- 第四条 **青色**：执行到 SCTLR_EL1 写入、ISB 及启动值比较之后。
- 第五条 **黄色**：已经进入 `_arm_init` 的 KVA 入口探针，原始第一条 PACIBSP 尚待重放。

这仍是诊断副本，未宣称修复内核。青条不证明所有页表都正确，黄条也不证明
C 初始化完成。若停在新增颜色，仍需考虑探针恢复/返回路径和之后的原始代码。

## 测试对

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `out/kernel-mmu-test/vendor_boot.img` | 100663296 | `1704e2c42fad3ebbb0e18c9f0148b5aa3dc54c1bdfa2e09ca43973f3c177a156` |
| `out/kernel-mmu-test/vbmeta.img` | 8000 | `30d6b6ad662604dd3ec9425ed81d1df2abaa7c1eac51d4d29fc952031933f677` |

保留当前会出白条的 boot_b。在 fastbootd 中刷入本次两份文件：

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-mmu-test\vendor_boot.img'
& $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-mmu-test\vbmeta.img'
```

选 XNU 前开始录像，记录是否新增青条、黄条，或是否出现新的文字/重启行为。
这些条带在原来三条的**下方**，不是把原来的白条改色。无需先刷根卷。
本探针不新增自动复位，原有 ramoops 启动参数不变。

## 实现与验证范围

`tools/prepare_mmu_probe.py` 使用固定 UniXNU
`0c1bac3d51a3009dca1708b15a5198f4b43d057a` 官方入口工具的 Mach-O 解析、
符号解析、跳转编码和汇编函数；官方脚本逐字节核对后只读加载，未编辑 fetched cache。
新代码属于板级诊断，明确记录其不同于官方基础蓝/紫探针的行为。

输入仍是原始 `cc690c9a…` 预链接内核，不叠加上一轮二进制修改。
核对入口、SCTLR/ISB、CPACR 前指令及 arm_init PACIBSP；四个不重叠的代码洞
必须全部为空，最终 357 个变更字节仅落在四处跳转和四个守卫填充区内。

开启 MMU 后 framebuffer 未必在启动页表中，因此新增检查点短暂关闭
SCTLR.M/C/I，以已知的物理代码别名和现有 GOP 地址写屏，再恢复读取到的 SCTLR。
C 入口探针先从 KVA 进入物理代码别名，恢复后返回 KVA，重放原始 PACIBSP 并继续。
它们不用栈，不修改页表或正常内存，仅使用调用者临时寄存器；保留 boot_args、
启动基址寄存器及 SP。这些点位于第一次 C prologue 之前，此前开启缓存后的原始
启动路径是寄存器操作。**不能把这个方法直接用于已经写栈或有脏数据的后续函数。**

短暂切换控制寄存器本身会扰动启动，可能影响被观察行为，因此不能把带探针的
进展当作永久修复；最终修正需移除探针后重新验证。

验证包括：Clang 汇编、指令守卫/变更范围检查、固定 DarwinBoot 生产解析器直接
解析实际 Mach-O、LZMA 与 raw 流往返、FAT 几何与 kernel 读回、ADT/配置哈希不变。
Unicorn 2.1.4 执行实际汇编机器码，检查物理/KVA 路径、boot_args/SP/基址保留、
仅目标 framebuffer 条带写入，以及控制寄存器值恢复。**模拟器显式模拟 SCTLR
读写并跳过原 PACIBSP，未验证实机 MMU、缓存或 PAC 行为。**

沿用用户已有仅本地签名授权。签名、vendor_boot hash 与 vbmeta 引用匹配；
vbmeta 的完整 header、公钥、其他 22 个 raw descriptor 和次序逐字节不变。
未执行设备命令，未新编 native/prelink；原始内核不变，实机结果待测试。
机器记录见 [kernel-mmu-test-build.json](evidence/kernel-mmu-test-build.json)。

回到上一轮蓝/紫探针时，成对使用 `out/kernel-entry-test/vendor_boot.img`
（`93c81e7d…`）和 `out/kernel-entry-test/vbmeta.img`（`58aad870…`）。
