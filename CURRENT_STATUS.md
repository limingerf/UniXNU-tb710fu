# Current status · 2026-11-15

这是当前工程的单一状态入口。编译、静态加载检查和实机运行分开记录。

| 检查点 | 结果 | 证据 |
| --- | --- | --- |
| macOS arm64e 原生内核编译/链接 | 通过 | 首轮43615291360；补齐真实stack导出后，原生输入SHA-256为57047aaca43bc2d907dab64fd54c0062a16d3f370e8c5c8220b2302e58622589 |
| 官方macOS严格预链接 | 通过 | mapping_complete=true，missing={}，stubbed={}，allow_missing=false |
| 最终fileset字节/结构/闭包 | 通过 | [验证run43620431387](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43620431387)：86个组件、79个可执行成员 |
| 同版本DarwinBoot生产解析器 | 通过 | 同一run对实际142,753,792字节产物执行官方Test/test-kernel-parser.py |
| UEFI输入修复云编译 | 通过 | [run43636909923](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43636909923)：AARCH64 DEBUG CLANGPDB，主机测试及10个必需模块通过 |
| FD本地独立复验 | 通过 | 3,145,728字节、两个真实FV、10个必需模块GUID及14个overlay文件哈希全部匹配 |
| UEFI实体按键修复候选 | 云编译及主机测试通过，实机待验 | Tb710fuKeyInputDxe：SPMI v7状态读取→ConIn；GCC/Clang和ASan/UBSan通过，实机未验 |
| 实机启动/PAC/GOP/UFS/用户态 | 未验收 | 当前任务未运行设备命令、刷写或更改分区 |

## 产物

- 预链接内核：验证run的artifact `tb710fu-verified-prelink-43620431387`。
  SHA-256：`c601ca5ceecfd1536f05634aa71ef700440d167cd2a4dbcd0a06e3a45400e131`。
- UEFI按键修复候选：run43636909923的artifact `tb710fu-uefi-43636909923`，文件 `SILICIUM_UEFI.fd`。
  SHA-256：`4fc321fb77ebad6f63970ae617228eef6be3bf98d98af5cfd4497ea51cd85b9b`。
- 私有Actions产物当前保留三天；源码、固定版本与哈希记录长期入库。
- 本机已保存首轮native、FD与报告至 `.local/`；完整预链接内核交付在上述私有artifact。

机器可读记录：`docs/evidence/native-build.json`、`prelinked-build.json`、`uefi-build.json`、`uefi-input-build.json`。先前UEFI run43618658596及其FD哈希保留于uefi-build.json，不覆盖历史证据。

## 历史红色run的原因

第一轮严格预链接发现真实缺失的 `unixnu_kernel_stack_shift`，已采用官方sheng的真实
栈/guard-page对齐实现修正。随后43618288536成功完成原生构建和官方预链接，但当时
包装检查器把合法 `MH_FILESET=12` 错判为 `MH_EXECUTE=2`，该历史run因此仍为红色。

检查器现已区分根fileset与嵌入原生内核，以及无代码组件和可执行成员。最终验证run
复用同一个产物，核验实际字节、严格报告、map冲突与同版本生产加载器；未为修检查器
重复编译内核，未启用allow-missing或加入桩函数。主构建workflow已接入正确检查及生产解析器。

## UEFI范围

固定官方base加版本化overlay，保留40项参考内存区域；移除旧外机RAW/DXE依赖，
修正SimpleFb查找、边界、清理和错误返回。模型索引0符合固定host工具的整数契约，
当前native板型路径不读取该宏。UEFI开发策略与Android ABL/AVB为不同层。

高位DDR来自同型号参考作者记录；当前设备live FDT、实际scanout stride/格式和所有权
仍需证据。源码检查和编译通过不构成物理内存可用或显示正常的证明。

## UEFI按键修复 · 2026-11-15

用户报告构建的UEFI实体按键无响应；此前overlay没有读取PMIC三键的输入提供者。
已在独立板级overlay加入Tb710fuKeyInputDxe，逐文件来源SHA-256见
[输入源码记录](docs/evidence/uefi-input-sources.json)，设计和验证边界见
[输入审查](docs/UEFI_INPUT.md)。音量加/减映射上/下，电源映射Enter。
新增驱动同时进入DSC/FDF，CI在编译前跑主机模型并核对真实压缩FV中的第10个必需模块。
本轮未访问设备；源码与主机验证不能证明实际按键恢复。云run43636909923及FD哈希已记录于`docs/evidence/uefi-input-build.json`。

## UFS 启用候选 · 2026-11-15

macOS 27 内核 137 MiB 无法放进 `vendor_boot` 的 81 MiB ramdisk 可用空间，
且本机 ABL 不支持 `fastboot boot`（实测 `FAILED (remote: 'unknown command')`）。
因此把内核改放到 `unixnu` 分区，由 UEFI 的 UFS 栈读取。

源码级确认（`Config.c:1547-1563`、`Config.c:1132`）：UniXNU 遍历所有已发布的
SimpleFileSystem 卷寻找 `unixnu.toml`，菜单项路径绑定配置所在卷，所以内核与
配置同卷即可。详见 [内核从UFS加载](docs/KERNEL_FROM_UFS.md)。

新增 `TB710FU_ENABLE_UFS`（默认 0）。为 1 时加入 8 个 stock Qualcomm DXE
（SmemDxe/HWIODxe/CmdDbDxe/PwrUtilsDxe/NpaDxe/RpmhDxe/ClockDxe/UFSDxe），
明确排除 2026-11-11 黑屏集（DALSYSDxe/VcsDxe/DisplayDxe/GpiDxe/SPIDxe/
I2CDxe/SdccDxe/PIL/Tz）。`check-board.py` 双向校验白名单与排除集，
默认构建经 `strip_ufs_gate` 剥离后仍报 PASS（40 区、10 模块、0 vendor 引用）。

云构建 run43692304879 success，FD `dfca596a…`，FVMAIN 由 51 增至 60 个 FFS
文件，8 个 UFS 模块在真实嵌套 FFS 中逐一核对 GUID。boot.img 与 vbmeta 已用
testkey 签名，vbmeta 描述符结构（3 hash + 4 hashtree + 3 chain + 13 prop）
与原厂一致。产物与判据见 [UFS构建记录](docs/UFS_BUILD.md)。

含内核的 `unixnu` 分区 FAT 镜像已预构建：224 MiB，内核读回 SHA-256
`c601ca5c…` 与输入一致，剩余 91 MiB。

**未验收**：UFS 块设备是否实际发布、`unixnu` 分区切分、内核能否从该卷启动，
均需实机证据。vendor DXE 来自 OnePlus Ace 5（giulia）替代集，TB710FU 自身
`uefi_a` 卷未能提取（FV 非可遍历 FFS 布局），板级 PHY/时钟存在风险。

## 下一阶段

先补本机DT与最小启动/日志输入，再做UEFI→DarwinBoot→单核EL1/PAC验证。真实UFS、
独立UnixNU存储、根策略、Data/cryptex与launchd随后接入；GPU和桌面后置。当前产物是
通过云构建与静态加载器检查的候选，并非已验收的双系统安装包。
