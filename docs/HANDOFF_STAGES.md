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

## 构建与安装输入

云 [run43729209898](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43729209898)
成功，构建源 `0ea7095d5d99a8d66b4ff8b77bbdf9268e6cfd52`。
FD SHA-256：`5722252b40e37d6c94e252c8daeddd9f3499b5483bca44f0cba93c795fb78a71`。
本地复验 11 个模块 GUID、22 个 overlay 哈希、两个实际 FV，部署 Entry.S 与
本地已审查/汇编的源码哈希完全一致。

`tools/repack_handoff_boot.py` 从已知正常的 log-viewer boot 载体复制 BootShim、
DTB 和 Android v4 header，仅替换 FD 并调整 kernel_size。原有 4096 字节全零
GKI signature 槽原样保留；非空 GKI 签名会被拒绝，避免保留失效签名。
相同 FD 输入能逐字节复现原始未签名载体，新 FD 的 gzip/DTB 读回也通过。

已生成 `out/handoff-stages/tb710fu-boot-handoff.unsigned.img`，659456 字节，
SHA-256 `113e0792a4166969a42e7e62b0ad518b2164ca1f34ba9c9e3e84192a90fa2b51`。
它尚需 boot AVB 签名。已核对现有载体的 RSA 签名及内容 hash，以及现有
vbmeta-ecc0 的 boot chain 公钥匹配；沿用该链的公钥和 rollback 参数后只更新 boot_b，
不需要修改 vendor_boot、根卷或槽位选择。

本轮尚未签名或刷入。安装后仍选原来的 XNU 项，等停屏稳定，记录顶部色带的
最后颜色；不需要为了取这次结果先长按电源。阶段标记最终为白色时，再用独立
XNU 蓝/紫入口探针细分内核最初阶段，不能把白色当作内核成功运行。

机器记录：`docs/evidence/uefi-handoff-stages-build.json`。
