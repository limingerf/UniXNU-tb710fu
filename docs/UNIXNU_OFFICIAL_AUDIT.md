# 官方调试与 ARM64e 流程对照 · 2026-11-16

审阅了 UniXNU 官方[调试与日志](https://unixnu.com/zh/guide/debugging)、[ARM64e 移植](https://unixnu.com/zh/porting/arm64e)及[设备目录](https://unixnu.com/zh/devices/)页面，并对照仓库中的 `board.toml`、`porting.toml`、`upstream.lock.json`、XNU 补丁顺序及当前启动记录。项目锁定的 UniXNU 源码为 `0c1bac3d51a3009dca1708b15a5198f4b43d057a`；该提交的本地调试与 ARM64e 文档和线上页面核对一致。

## 当前停点

`XNU returned: vm/qsort#1` 是 `_qsort` 返回后的阶段文字，不是 qsort 崩溃。用户先后提供 QR5 和 QR8 阶段 47 读回；最新标签 `src-045bfc7c-qr8` 与候选报告器一致，`ARG0=0x8c88fd000`。旧报告文字 `marker helper entry` 不准确：包装器在 helper 的 `BL` 前画码，已更正为 `before marker helper call`。读数证明运行到切换调用点，且 ARG0 中的 TTBR1 根表 PA 位于设备 2 GiB 管理窗口内；它不证明 helper 已运行、TTBR1 已切换，也不是故障 PC。缺少实际刷入镜像哈希，因此不能确认这次启动使用了哪个签名包。

run `43844823907` 已用更新后的 0006/0009 在 macOS 26 runner 完成 arm64e native 编译、严格 prelink 和固定 DarwinBoot 生产解析器。当前 QR8 报告器增加阶段 47/48、切换后 `_set_mmu_ttb` 与 `pmap_bootstrap` 的入口/返回，以及抢占安全恢复后的阶段 54 和 `arm_vm_init` 返回后的 ramoops 入口/返回（55/56）。阶段 47 只证明到达 helper 调用点；48 才证明 helper 返回；55 证明 `arm_vm_init` 已返回。抢占关闭期间不画 QR，正常轮播四帧约 2 秒后继续。

## 调试流程

官方指南要求每个启动证据带构建标签、镜像哈希、最后确认的检查点和来源版本；它建议 `boot-progress=1/2` framebuffer 诊断、`panic_screen`、官方 `entry_probe_reset_*` 阶段探针、串口及持久化日志。这里有两条不同的串口路径：`serial=3` 选择 XNU 的 UART 控制台；UniXNU/DarwinBoot 固件输出则写入第一个 `EFI_SERIAL_IO_PROTOCOL`。两者都要求板级 UART/串口通道实际存在，设置启动参数本身不会创建端口。设备当前没有可用的外接端口，因此本诊断改用屏幕 QR 与可从 TWRP 读取的物理标记。USB 调试终端依赖 giulia 的 CDC 命令协议和设备驱动，不能通过替换 VID/PID 在 TB710FU 上启用。

官方 OneSend 日志来源描述的是 UEFI Advanced Logger。这里的 XNU OneSend QR 是独立诊断实现；它把当前阶段报告编码成固定格式帧，不冒充完整 XNU console。用户提供的 `src-045bfc7c-qr5` 对应阶段47诊断内核。最新候选标签为 `src-045bfc7c-qr8`。`ramoops` 区域虽已移到管理窗口内，但 XNU 在 `arm_vm_init` 返回后才初始化它，不能记录该函数内部停点；QR8 加入紧邻其入口/返回的报告。完整输入、注入后 kernel、LZMA 和 boot image 哈希保存在证据记录中。

## ARM64e 门槛

官方 ARM64e 页面目前只把 sheng / SM8550 列为受支持设备，并要求逐一确认：每个启用的 CPU 具备地址和通用 PAuth 能力；EL1 可写五组密钥寄存器且访问不陷入；PAC 指令在 EL0/EL1 不陷入；PACIA/AUTIA 往返成功；虚拟机监控器对 `HCR_EL2.APK/API` 的行为符合要求；使用原生 4 KiB 一级翻译；SoC 已有支持记录。

本项目的 `porting.toml` 已将 CPU 区域标为 `planned`，并明确前次 Android `paca/pacg` 与 XNU 屏幕上的 ID 寄存器读取不构成逐核 EL1 PAC/SMP 证明。这个状态符合官方门槛；当前 macOS 27/arm64e 产物是实验性 bring-up 候选，不是受支持设备声明。若设备报告到达阶段 56（`arm_vm_init` 已返回且 ramoops 初始化也返回），下一项工作再按官方清单完成主核 PAC 密钥写入、PACIA/AUTIA 和 EL2 陷入检查，然后逐核验证 affinity、timer/IPI 与 SMP。

## 相近设备

| 项目 | 官方记录 | 与当前停点的关系 |
|---|---|---|
| [caihong / OnePlus Pad Pro](https://unixnu.com/zh/devices/caihong) | SM8650，iPadOS 17.7.11/XNU 10063；记录已过内核入口、MMU、kext、BSD 和 `launchd`，使用 `panic_screen`、`serial=3` 与恢复 ramdisk `rd=md0` | 有价值的同 SoC 分阶段调试参考；XNU 比当前 12377 旧，板级 PMGR、ADT、启动链不同 |
| [houji / Xiaomi 14](https://unixnu.com/zh/devices/houji) | SM8650，Darwin 22/XNU 8796.141.3；八核、原生 UFS/HFSX；Recovery 可读取持久日志，但维护者注明极早期停机可能尚未写盘 | 说明后期日志可走独立恢复通道；不能覆盖当前 `arm_vm_init` 内部停点 |
| [giulia / OnePlus Ace 5](https://unixnu.com/zh/devices/giulia) | SM8650，Darwin 22，CPU0 启动及该板专用 USB CDC 日志已验证 | 协议与驱动属于 giulia；不能将其 USB 终端直接用于 TB710FU |
| [meizu21](https://unixnu.com/zh/devices/meizu21) | 官方设备页仍列出支持待建立；另有 [XiaoChen8371/unixnu-meizu21](https://github.com/XiaoChen8371/unixnu-meizu21) 的 SM8650 Darwin 22 transition 仓库 | 该仓库保留 Darwin22 kernel/loader/UFS，`kernel_rebuilt=false`，没有原生 UniXNU UFS 或 USB 调试驱动；可参考其保护根卷和回滚流程，不能作为 XNU 12377 驱动来源 |
| [旧 TB710FU 移植](https://unixnu.com/zh/devices/tb710fu) | iPadOS 17.7.11、arm64/XNU 10063；其设备镜像当时尚未上机 | 同机型历史可参考，但无 macOS 27/arm64e 切换路径；旧 `0xac340000` ramoops 地址在当前 2 GiB 管理窗口之外 |

这些项目没有提供当前 XNU 12377 `set_mmu_ttb_alternate` 的直接修复。没有把其旧 XNU、USB 协议、SoC 私有日志布局或板级代码搬入本项目。

## 验证与边界

run `43844823907` 的 GitHub artifact SHA-256 为 `8063bc02fc5d405cbb7e20524a21e05a844c8937495b69b77e8dedc3597a33cd`；prelinked kernel SHA-256 为 `045bfc7cfad9ed2539a586043dd43211263099734f4978abd8092c213b7d7d95`，native 输入 SHA-256 为 `670f2576332d20e8f9615f2f9d84a08a2313fc68d995e8d38af6ffeeb970236a`。严格报告为 `mapping_complete=true`、`missing={}`、`stubbed={}`、`allow_missing=false`，生产 DarwinBoot 解析器通过。独立本地复核器原先把 prelink 最终 SHA 错当 native 输入 SHA，已修正为分别校验两个记录。

XNU 输入清单中的 0006–0009 哈希与当前补丁一致。实际 helper 位于 `0xfffffff00bb8b360`，调用点位于 `0xfffffff00bb8a7b4`；机器码确认 mark0 → DSB → `MSR TTBR1_EL1` → ISB → mark1 → RET 的顺序。QR8 报告器注入后 kernel SHA-256 为 `e2f4afb554a5bf1f90fd1f0ce8a347deb0928310b124f2f7056fd31b944ca617`，RX 段 32 KiB，RW-NX 段 784 KiB，24 个 PIE 重定位目标在 RW-NX 段内。九个 AArch64 模型画面逐一由 ZXing 解出 752 字节帧；固定 OneSend receiver 对九阶段各四序号帧均验证成功。模型不是实机 MMU/相机证明。

这些是编译、静态文件、指令模型、二维码和传输协议验证。用户已提供 QR8 阶段 47 的 OneSend 报告文本（文件 SHA-256 `b141c41e…`），但无设备刷入哈希，且没有阶段 48 及后续报告或 PAC 硬件验证。底层启动故障尚未判定或修复。

QR8 诊断镜像已在本地签出，`vendor_boot.img` SHA-256 为 `b5274a747ac23d31f9bb9648738076fe78d12f333ffc9852d9927592562f5280`，匹配的 `vbmeta.img` 为 `88dd4989330fad6bc9f0e6edeca38ee9996081bc2691c842ea2412e9c4af991b`。独立 RSA 复算、payload 读回、原 vbmeta header/公钥和另外 22 个描述符检查通过；仍未刷写或重启。机器证据见 [kernel-switch-qr-build.json](evidence/kernel-switch-qr-build.json)。
