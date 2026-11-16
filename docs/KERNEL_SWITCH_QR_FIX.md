# TTBR1 停点诊断修正 · 2026-11-16

## 审计结论

屏幕文字 `XNU returned: vm/qsort#1` 是早期报告器在 `_qsort` 返回后写出的阶段文本，不表示 qsort 失败。它是阶段 46。切换标记版启用 `TB710FU_SWITCH_MARKS=1` 后走 0007 的 `set_mmu_ttb_alternate_probed()` 分支；报告器原来替换的是另一条 stock `set_mmu_ttb_alternate()` 调用，因此不会进入原 stage 47 钩子。这个镜像在切换前停止更新文字并没有二维码，是控制流设计的结果，不能单独判定 TTBR1 是否切换成功。

截至本审计，仓库记录的那张屏幕反馈属于 A 版 `out/kernel-switch-markers`。B 版 `out/kernel-log-region` 是较新的签出镜像，但证据文件没有设备刷入哈希，也没有标记字节读回。用户又报告最新构建仍不能进入内核；目前缺少能把该次启动绑定到某个镜像的读回证据，所以真实硬件停点仍未确定。

`bmk-keyeast/UniXNU-tb710fu` 的 main HEAD `23235249d71604f8a6f743680ef8fd1003fe7189` 是 iPadOS 17.7.11 / XNU 10063.141.1 项目。它没有当前 XNU 12377 `set_mmu_ttb_alternate` 路径或 QR 报告器。可参考它的 ramoops 格式测试和早期分阶段探针；其 ramoops 初始化在 `arm_vm_init()` 返回后，地址 `0xac340000` 又在本项目的 2 GiB 窗口之外，因此不能直接解决这里的停点。

## 修正

更新 `patches/xnu/0006-tb710fu-diag-segment-rx.patch`，让它在两段同时存在、连续且 16 KiB 对齐时成对映射 `__KDIAG` RX 与 `__KDATRW` RW-NX。新增 `patches/xnu/0009-tb710fu-switch-helper-noinline.patch`，将 0007 的标记 helper 声明为 `noinline`，供 post-link 工具定位真实调用。报告器 Mach-O 拆成 RX 代码段与 RW-NX 状态/栈段；构建时检查段几何和 PIE 重定位目标。

`tools/prepare_early_report.py --trace-switch-probe --trace-past-switch` 会包住真正的标记 helper：阶段 47 在调用前显示二维码，阶段 48 仅在 helper 返回后显示，阶段 49 是切换后的首个 `_set_mmu_ttb` 调用。阶段 48 说明 helper 已越过 TTBR1 写入、ISB 和 post-ISB 标记存储并返回；阶段 49 进一步说明内核执行进入切换后的下一调用。缺少阶段 48 时，阶段 47 的旧二维码只表示已到达切换前，不能当作故障 PC。

## 本地验证

- 更新后的 0006 与 0007–0009 按顺序应用到隔离 XNU 源码树通过。
- Reporter、原 QR encoder 和入口汇编以 AArch64 freestanding 方式编译通过；主机测试覆盖阶段 47/48/49 的帧、CRC、轮播和有界堆。
- Unicorn AArch64 模型对三个阶段均验证 QR READY、正常路径返回、屏幕写范围受限，以及临时 TTBR0/TCR 恢复。
- 固定 OneSend receiver `39e95ce59df2f2dc38d2556dd4239cec6a0ca35f` 对三阶段四种序号帧均能单帧还原精确报告文件。
- 用 0008 内核生成的分段探针为 32768 B RX + 802816 B RW-NX，24 个运行时重定位都指向 RW-NX 段；其双段 Mach-O 经固定 DarwinBoot `5243c68ec332a227fe3680cd5da6d5a0e127035c` 的生产解析器通过。此项只验证报告器布局，不代表含 0009 的 XNU 已重编。

## 边界与下一步

当前底层 TTBR1 故障原因还没有被证明或修复。下一步必须把更新后的 0006 与 0009 纳入官方 macOS runner 的 native 构建和严格 prelink，验证实际内核保留了 helper 符号、调用点、两种段权限和标记机器码，再生成签名候选。二维码/OneSend 的本地模型不能证明实体相机采集；本审计未刷机、重启或改分区。
