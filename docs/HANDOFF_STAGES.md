# 绿条之后的加载器检查点 · 2026-11-15

用户实测目前只有原有绿条。该条位于 DarwinEnter 开头，不能区分后续缓存维护、
MMU 关闭、TLB 维护或跳转。本变体把**同一条顶部色带**依次重画，记录最后完成的步骤。

| 最后颜色 | 已完成的步骤 | 随后的待查区间 |
| --- | --- | --- |
| 绿 | 进入 DarwinEnter，ExitBootServices 已返回 | 数据缓存维护循环/屏障 |
| 橙 | 数据缓存维护循环及 DSB | 指令缓存失效/屏障 |
| 黄 | 指令缓存维护及 ISB | 关闭 MMU 的 SCTLR 写入/后续取指 |
| 青 | SCTLR 写入及 ISB | TLB 维护/屏障 |
| 白 | TLB 维护及屏障 | 下一条内核跳转及最早期内核执行 |

这里只能把停点缩小到区间，不能仅凭颜色宣布某条指令就是根因；应先确认测试镜像身份。
原来的固件只有绿条，必须安装本变体才会产生橙/黄/青/白。
这些标记与单独的蓝/紫 XNU 入口探针不同；本变体无需改 vendor_boot 中的内核。

来源固定为 `UniXNU/DarwinBoot@5243c68ec332a227fe3680cd5da6d5a0e127035c` 的
`DarwinBootPkg/Driver/Entry.S`，通过显式
`firmware/patches/0004-darwinboot-handoff-stages.patch` 应用。
保留原缓存/MMU/TLB 操作顺序，只添加 framebuffer 写入、相应屏障和零尺寸保护。
`x12/x13` 保存 framebuffer PA/长度，标记仅使用 `x14-x16` 和条件标志，
`x0` 的 boot_args 与 `x1` 的内核入口保持不变；不依赖栈、UEFI 服务或重启后内存保留。
不改根卷、槽位、ramoops、内核或安全策略。

验证：固定源补丁应用检查通过，Clang AARCH64 ELF 与 Windows COFF 汇编通过，
逐段反汇编核对四个标记均在预期操作之后、最终仍为 `br x1`。
CI 的 LZMA 固件构建应用该补丁并记录部署后的 Entry.S 哈希与阶段表，
实际 FD/模块及云 run 另行记录。实机行为未验证。
