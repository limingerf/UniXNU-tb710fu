# TTBR1 停点诊断修正 · 2026-11-16

## 审计结论

旧屏幕文字 `XNU returned: vm/qsort#1` 是 `_qsort` 返回后的阶段文字，不表示 qsort 崩溃；它是阶段 46。用户先后提供 QR5、QR8 的阶段 47 报告；最新标签 `src-045bfc7c-qr8` 与最终报告器一致，报告包含 `ARG0=0x8c88fd000`。这确认执行到包住 `set_mmu_ttb_alternate_probed()` 的包装器调用点。报告文字曾写成 `marker helper entry`，但机器码显示 QR 实际在 `BL` 前绘制；已改成准确的 `before marker helper call`。因此阶段 47 不证明 helper 已执行、TTBR1 已写入，也不是故障 PC。该根表 PA 位于本机 2 GiB 管理窗口 `0x8c0000000..0x940000000` 内。报告没有设备分区镜像哈希，不能确认使用了哪个签名包。

阶段 48 只有在真实 helper 返回后才生成，阶段 49/50 包围切换后的首个 `_set_mmu_ttb` 调用。新版扩展到阶段 56，并覆盖 pmap 和 `arm_vm_init` 返回后的 ramoops 初始化；这些仍是检查点，不会把停留在调用前的帧伪装成异常现场。

`bmk-keyeast/UniXNU-tb710fu` 的 main HEAD `23235249d71604f8a6f743680ef8fd1003fe7189` 是 iPadOS 17.7.11 / XNU 10063.141.1 项目。它没有当前 XNU 12377 `set_mmu_ttb_alternate` 路径或 QR 报告器。可参考它的 ramoops 格式测试和早期分阶段探针；其 ramoops 初始化在 `arm_vm_init()` 返回后，地址 `0xac340000` 又在本项目的 2 GiB 窗口之外，因此不能直接解决这里的停点。

## 修正

更新 `patches/xnu/0006-tb710fu-diag-segment-rx.patch`，让它在两段同时存在、连续且 16 KiB 对齐时成对映射 `__KDIAG` RX 与 `__KDATRW` RW-NX。新增 `patches/xnu/0009-tb710fu-switch-helper-noinline.patch`，将 0007 的标记 helper 声明为 `noinline`，供 post-link 工具定位真实调用。报告器 Mach-O 拆成 RX 代码段与 RW-NX 状态/栈段；构建时检查段几何和 PIE 重定位目标。

`tools/prepare_early_report.py --trace-switch-probe --trace-past-switch` 按下列顺序绘制阶段 QR：

- 47/48：标记 helper 调用前 / helper 返回后。
- 49/50：切换后的 `_set_mmu_ttb` 入口 / 返回。
- 51/52：`pmap_bootstrap` 入口 / 返回。
- 54：`__enable_preemption` 返回，表示关闭抢占期间的页表映射和尾部处理已完成。
- 55/56：`arm_vm_init` 返回后的 `tb710fu_ramoops_init` 入口 / 返回。

抢占关闭期间不绘制二维码，以免为诊断等待而延长该临界区。每个正常检查点轮播四个序号帧，约两秒后继续；每个序号携带同一份报告，任意单帧都可完成 OneSend 文件还原。阶段 47 只表示即将调用 helper；阶段 48 才表示 TTBR1 写入、ISB 和 post-ISB 标记存储已执行并返回。

## 验证结果

- 更新后的 0006 与 0007–0009 按顺序应用到隔离 XNU 源码树通过。
- 官方 macOS runner run `43844823907` 成功完成 native arm64e、strict prelink 和固定 DarwinBoot 生产解析器。native 输入 SHA-256 为 `670f2576332d20e8f9615f2f9d84a08a2313fc68d995e8d38af6ffeeb970236a`；prelinked kernel SHA-256 为 `045bfc7cfad9ed2539a586043dd43211263099734f4978abd8092c213b7d7d95`；严格报告无 missing/stubbed，artifact SHA-256 `8063bc02fc5d405cbb7e20524a21e05a844c8937495b69b77e8dedc3597a33cd`。
- QR8 报告器、原 QR encoder 和入口汇编编译通过；主机测试覆盖新增阶段文本、帧、CRC、四序号轮播和有界堆。
- Unicorn AArch64 模型对九个可见阶段（47–52、54–56）均验证 QR READY、四帧后正常返回、屏幕写范围受限，以及临时 TTBR0/TCR 恢复；在 3200×1080 模拟面板上，ZXing 对九张最终 QR8 像素图逐张解出对应 752 字节帧。
- 固定 OneSend receiver `39e95ce59df2f2dc38d2556dd4239cec6a0ca35f` 对 QR8 最终 AArch64 模型生成的九个阶段四序号帧逐阶段验证；任意单帧可还原准确文件。
- 云内核中的 helper 位于 `0xfffffff00bb8b360`，真实调用点位于 `0xfffffff00bb8a7b4`；逐字节确认 BTI、mark0、DSB、MSR、ISB、mark1、RET 顺序。QR8 报告器注入后 kernel SHA-256 为 `e2f4afb554a5bf1f90fd1f0ce8a347deb0928310b124f2f7056fd31b944ca617`，RX 段 32 KiB、RW-NX 段 784 KiB，24 个运行时重定位都写入 RW-NX。实际 Mach-O 经固定 DarwinBoot `5243c68ec332a227fe3680cd5da6d5a0e127035c` 生产解析器通过。

## 签名诊断候选

基于已签名的窗口内 ramoops 镜像重新打包，只替换 payload；ADT、`unixnu.toml`、默认启动项和其他 vbmeta 描述符保持不变。QR8 候选位于 `out/kernel-switch-qr8-label-signed/`：`vendor_boot.img` SHA-256 `b5274a747ac23d31f9bb9648738076fe78d12f333ffc9852d9927592562f5280`，匹配 `vbmeta.img` SHA-256 `88dd4989330fad6bc9f0e6edeca38ee9996081bc2691c842ea2412e9c4af991b`。AVB 自检、独立 RSA-4096 复算、payload/kernel/ADT/TOML 读回、vbmeta header/公钥和 22 个未改 raw descriptor 检查通过。完整输入和结果见 [机器证据](evidence/kernel-switch-qr-build.json)。

## 边界与下一步

当前底层 TTBR1 故障原因还没有被证明或修复。用户已提供 QR8 阶段 47 报告文本，但没有设备分区镜像哈希、阶段 48 或后续读数；候选的完整硬件行为仍未验证。模型和屏幕内容不能证明硬件页表切换成功。
