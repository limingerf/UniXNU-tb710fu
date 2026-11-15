# Current status · 2026-11-15

> 最新实机反馈（2026-11-16）：普通路径到 vm/set_mmu_ttb_alternate#1，
> 有静态 QR 但 OneSend 未收件。实际接收源码重放证明单个最终帧可完成，
> 也确认先扫较早阶段会锁定旧会话。本版只发送最后一份固定报告，500ms
> 轮播，并新增明确暂停的持续导出菜单；普通启动约8秒后继续。
> 轮播/收件的新版实机待验，原内核卡点未确认。见 [轮播导出](docs/KERNEL_ANIMATED_QR.md)。

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

## LZMA 压缩内核候选 · 2026-11-15

UFS 分区路线需要先切分区、再验 UFS 驱动。另一条更直接的路：把 137 MiB 内核
LZMA 压成 23 MiB，仍走 vendor_boot 的 payload ramdisk，由 DarwinBoot 解压后
交给 XNU。这是那台已上板 macOS 27 设备用的办法（用户提供线索）。

- 实测：`kernel-ufs.lzma` 24,073,889 B（8 MiB 字典），往返 SHA-256 与
  `c601ca5c…` 一致；大小字段已回填（`xz --format=lzma` 会写 0xFFFFFFFF，
  EDK II 需要真实值）。payload 25 MiB，余量 58 MiB。见
  [LZMA 内核](docs/LZMA_KERNEL.md)。
- 解码器：`MdeModulePkg LzmaCustomDecompressLib`（NULL 库，scratch 固定
  64 KiB）。**不能用 `EFI_DECOMPRESS_PROTOCOL`**——DXE core 用
  UefiDecompressLib（Tiano/EFI 压缩）实现它，不是 LZMA。
- `firmware/patches/0002-darwinboot-lzma-kernel.patch` 让 DarwinBoot 在
  `ReadFile` 后探测 0x5D 头，命中则 `LzmaUefiDecompress` 展开后交给
  `ParseKernel`；普通 Mach-O 不受影响。
- `tools/ci_firmware.py --lzma` + `.github/workflows/uefi-lzma.yml` 在云端
  构建该变体，并在 FD 里核对 DarwinBoot GUID。

**实机结果（2026-11-15）**：XNU 已在内核里执行，EL1。屏幕显示绿色交接横带、
`Current EL EL1`、非零 `TCR_EL1`、`ID_AA64MMFR0/ISAR1/ISAR2` 读取
（Parange 2、4 KiB granule、PAC 能力位）与 `Darwin Kernel Version 27.3.0`
横幅。整条链 UEFI → UniXNU → payload → LZMA 解压 → ADT → 窗口 → XNU 全通。
证据与阶段边界见 [XNU 实机进入内核](docs/KERNEL_BOOT.md)。

到达该阶段前修复了三个只有实机才暴露的问题：

| 症状 | 根因 | 修复 |
| --- | --- | --- |
| 菜单无 xnu | 25 MiB payload FAT32 仅 50,380 簇 < 65,525 下限，固件拒挂载 | 33 MiB FAT32（66,512 簇）；新增 `tools/check_payload_fat.py` 双向校验 |
| 选 xnu 卡死 `ASSERT [DxeCore] Pool.c(750)` | `AllocatePages` 分配、DarwinBoot 用 `FreePool` 释放 | 改用 `AllocatePool` + `FreePool` |
| 缺 `.adt` 直接失败 | clean board 无 `[payload]` 配置 | 从本机 live FDT 派生 ADT（补单范围 memory/timebase/psci，删 cpu-map） |

**未验证**：平台专家匹配、`/cpus` 与 `/arm-io` 是否被接受、根卷挂载、
用户态/launchd、显示服务、GPU、真实 UFS。

关键修正记录：DarwinBoot 是 UniXNU 的 gitlink，`defconfig` 不初始化子模块，
需 `--lzma` 显式 `git submodule update --init drivers/DarwinBoot`；`NULL|`
库链接只能写在 DSC 组件 override 块，写进 INF 会崩 BaseTools；且
`Binaries` 不能进 `[firmware.submodules]`，否则 build firmware 重复 init 与
materialised 目录冲突。

## 内核日志通道：ramoops · 2026-11-15

内核在 EL1 执行后屏幕无输出（交接后无 UEFI 显示驱动）。为拿到内核日志，
按 macOS 27 的真实补丁基线移植了参考移植的 ramoops console 补丁
（`patches/xnu/0004-tb710fu-ramoops-console.patch`）。

关键：macOS 27 profile 在设备补丁前先应用 55 个上游 xnu-12377 补丁，
补丁必须基于**已应用的树**（`xnu-work` @ `ac9718fb`）生成；基于裸 feed
生成的版本应用失败（实测）。已验证 0001–0004 整个设备序列按顺序干净应用。

云构建：native run 43709117408 success、严格 prelink run 43710067378
success。预链接 `kernel.macho` 142,753,792 B（SHA-256 `cc690c9a…`），
字节内确认含 `tb710fu_ramoops` 等符号。LZMA 压缩 24,054,955 B 往返一致。
启动参数带 `ramoops=0xac340000 ramoops_reset_after=120`。

交付三件套已重建，14 项刷入前审计全 PASS；只需刷 vendor_boot_b
（`72d8577c…`）与 vbmeta_b（`c210d711…`），boot_b（`6bcc95a3…`）未变。
详见 [ramoops 文档](docs/RAMOOPS.md)。

**未验证**：zone 能否跨复位存活、Android 侧 pstore 是否出现、内核实际停点。

补记：TWRP 实测 `pstore` 已挂载、`CONFIG_PSTORE_RAM/CONSOLE=y`、
`/sys/module/ramoops` 与 `/sys/module/pstore` 存在、`backend=ramoops`；
但 `/sys/module/ramoops/parameters/ecc = 0`，而旧 XNU 参数写
`ramoops_ecc=16`，GKI 按 ecc=0 解析带 RS 校验的 zone 会判无效并清零，
这就是首轮 pstore 为空的原因。已改 `ramoops_ecc=0` 重建并推送
（`fa0d3a2`），交付文件 `vendor_boot-ecc0.signed.img`（`836ca997…`）与
`vbmeta-ecc0.img`（`3992de82…`），14 项审计全 PASS。实机复读仍待验证。

## macOS 27 根卷（userdata）构建 · 2026-11-15

按官方 `unixnu build userdata` 落地根卷镜像，云 run **43724852936 success**。

- `board.toml` 补 `[userdata]` 实测 GPT 几何：`first_lba 5105064`、
  `last_lba 61390842`、`lba_bytes 4096`（214.71 GiB 容量上限，
  `sgdisk --print /dev/block/sda` 实测，userdata 为第 14 且最后一个分区）
- profile 自带 `[userdata] windows`（OS/SystemOS/App）与
  `[payload.adt] root_match_volume_uuid = 84581E99-…`
- 远程 IPSW 25.3 GiB，实测支持 HTTP range；只读三个成员约 13 GiB
  （OS 10.62 GiB AEA、SystemOS 2.31 GiB AEA、App 0.05 GiB）
- macOS runner 上 `ipsw 3.1.748` + `/usr/bin/aea` + `hdiutil` 完成 AEA 解密

**产出 `userdata-root.img`：17,466,130,432 B（16.27 GiB，远小于 214 GiB
分区）**，SHA-256 `221d18c76ccd6bcc0ec9141508ef109a254783d029b04e3401993450440ccc7a`，
三个窗口全在，报告校验通过。artifact `tb710fu-macos-userdata-43724852936`
（压缩 13.2 GiB，私有保留 3 天），本机未下载。详见
[根卷构建记录](docs/USERDATA_BUILD.md)。

**未解决**：设备 214.7 GiB userdata 装满 Android data、GPT 无空闲扇区、
data 多层加密；官方保留数据缩容工具会拒绝。用户表示"清空就行"，但擦除/
重分区/刷入**尚未执行且不可逆**。Data 卷 seed 是独立输入，未做。
根卷 UUID 与实际镜像的匹配、根卷挂载、launchd 均未验证。

## 下一阶段

先补本机DT与最小启动/日志输入，再做UEFI→DarwinBoot→单核EL1/PAC验证。真实UFS、
独立UnixNU存储、根策略、Data/cryptex与launchd随后接入；GPU和桌面后置。当前产物是
通过云构建与静态加载器检查的候选，并非已验收的双系统安装包。

## UEFI 只读日志查看器 · 2026-11-15

用户确认当前 A 槽能进 Android 和 TWRP；此前备份不是当前 A 槽状态。
新增 `Tb710fuRamoopsDxe`，在 BDS 前复制 `0xAC340000+0x40000`，
通过官方 Tools 协议提供 ECC 0/16 布局的只读日志翻页。显式框架补丁会在
旧 payload 配置缺少 Tools 时追加入口，保持默认项不变。
GCC/Clang 严格 C11、GCC ASan/UBSan 主机解码测试、40 项内存图和 11 模块预检通过；
云构建与实机结果分别记录，不把源代码/主机测试等同于 DDR 保留或屏显成功。
详见 [UEFI 日志查看器](docs/UEFI_LOG_VIEWER.md)。

本轮从已连接的 TWRP 只读核对 A 槽启动输入，未变更设备。A 槽现有 Linux
内核已带 AARCH64 EFI stub，无需重新编译 Linux；输入哈希见
`docs/evidence/android-oneshot-a-inputs.json`。USB/ADB/fastbootd 与一次性 A 槽引导
尚未实现；UFS 读取、DTB 和 ramdisk 组合是后续前置条件。普通 set-active 不满足下次回 UEFI 的要求。
空 pstore 曾被归因于 ECC 头校验失败；已更正为未证实，ECC=0 时参考实现跳过该校验。

日志查看器云构建 [run43722058056](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43722058056)
成功，实际构建源 `825a8560a237dc2de1426b207a3ed8ca00842338`；artifact
`tb710fu-uefi-lzma-43722058056`，FD 3,145,728 字节，SHA-256
`6a6ff288ddf48d1576cff991fb99b6feb90b4298c442f26eaf3442ee05a60f1e`。
本地独立复验：两个真实 FV、11 个必需模块 GUID、23 个 overlay 文件哈希全部匹配。
机器记录：`docs/evidence/uefi-log-viewer-build.json`。本版是保留 LZMA 加载路径的
可测试 FD，未签名、未刷入；日志查看器实机状态仍未验证。

## 绿条停屏复核与无重编译入口探针 · 2026-11-15

用户报告实际绿条、长按电源强制重启，未经过 Android/TWRP；日志工具原文
`sig 4107024 start1072 size6808`。此头不能作为有效 persistent_ram console 解码，
但无法据此区分未初始化与复位后数据丢失。当前证据不足以归因为根卷缺失。

已用固定官方入口探针生成现有 cc690c9a 预链接内核的诊断副本：蓝色标记
XNU 入口、紫色标记开启 MMU 前。不新增自动复位。仅 129 字节变化，同版生产
解析器通过，LZMA 往返和 payload FAT/读回通过。详见 `docs/XNU_ENTRY_PROBE.md`。
本地 unsigned vendor_boot 已准备；本轮未签名、未刷写，探针未上机。

## 绿条后加载器检查点 · 2026-11-15

针对原有绿条不能区分停点的问题，新增显式 DarwinBoot Entry.S 补丁：
同一条顶部色带依次绿→橙→黄→青→白，分别覆盖进入交接、数据缓存维护完成、
指令缓存维护完成、MMU 关闭后、TLB 维护后且即将跳转。保留 x0/x1，
不改变原缓存/MMU/TLB 操作顺序，未更改内核或根卷。

云 run43729209898 success，构建源 `0ea7095d5d99a8d66b4ff8b77bbdf9268e6cfd52`；
FD `5722252b40e37d6c94e252c8daeddd9f3499b5483bca44f0cba93c795fb78a71`。
本地 11 GUID、22 overlay 哈希、Entry.S 哈希和两个真实 FV 复验通过。
未签名 boot 载体已准备（113e0792…），现有 boot chain 密钥匹配及源载体
签名/hash 验证通过；尚未签名或刷入。见 `docs/HANDOFF_STAGES.md`。


用户已明确授权仅本地签名。检查点 boot 镜像已签出：
`out/handoff-stages/boot.img`，100,663,296 字节，SHA-256
`a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`。
RSA 签名、内容 hash、FD 读回、原 boot chain 密钥匹配验证通过。
本轮未执行设备命令、刷写或重启，实机结果待用户测试；步骤见
`docs/HANDOFF_STAGES_FLASH.md`。

## 白条后进入核内诊断 · 2026-11-15

用户实测报告顶部白条，尚无核内标记。本轮沿用已有本地签名授权，将固定官方
蓝/紫入口探针副本 e2156f42… 装入 vendor_boot，产出已签名测试对：
`out/kernel-entry-test/vendor_boot.img`（93c81e7d…）及 `vbmeta.img`（58aad870…）。
保留现有已到白条的 boot_b，不涉及根卷。

签名、vendor_boot hash、匹配 vbmeta 引用均通过；vbmeta header、公钥和其余
22 个 raw descriptor 及次序逐字节不变。签名后读回内核并解压哈希正确，
配置/ADT 不变，FAT 几何通过。未新编译 native/prelink，原始内核文件未改，
未执行设备命令。实机入口停点待本次测试反馈，不能称为已修复内核。
步骤见 `docs/KERNEL_ENTRY_TEST.md`，机器记录 `docs/evidence/kernel-entry-test-build.json`。

## 核内蓝/紫已确认，继续 MMU / C 入口诊断

用户报告 out/kernel-entry-test 测试出现白＋蓝＋紫条。核内入口执行与
到达开启 MMU 前检查点已经获得用户实机反馈；设备镜像哈希未读回。
目前仍不能证明 MMU 开启失败或已进入 C 初始化，根卷未成为该停点的已证实原因。

新增青色（SCTLR/ISB 后）、黄色（arm_init KVA 入口）诊断，使用守卫的物理/KVA
代码别名及临时 SCTLR 切换写屏，恢复后继续，未修改页表。357 个变更字节限于
四处跳转和四段空填充区；生产解析器、LZMA、FAT、指令模型检查通过。
指令模型不模拟实际 MMU/缓存/PAC，探针不是永久修复。

已沿用仅本地签名授权签出 out/kernel-mmu-test/vendor_boot.img（1704e2c4…）
和匹配 vbmeta.img（30d6b6ad…）。签名与 hash 引用通过，其他 22 项描述符及
header/公钥不变；未操作设备、未新编 native/prelink。测试说明和边界见
`docs/KERNEL_MMU_TEST.md`，机器记录 `docs/evidence/kernel-mmu-test-build.json`。

## 核内异常文字与 OneSend · 2026-11-16

用户要求不再堆满色条，并希望屏显日志或OneSend。当前用异常快照捕获
ESR/FAR/ELR、原SP/LR和所有通用寄存器，显示关键字段及静态LMS1/OS二维码。
固定微软QrEncodeData原样编译，单帧720字节载荷/752字节帧，三个层级CRC32；
不是完整连续日志，未实现核内五档按键控制。旧五条清除，只覆盖顶部48像素。

诊断入口保留原PACIBSP，安装EL1向量，在异常或panic时用独立物理栈报告；
若到平台初始化前则主动BRK b710，明确标注intentional checkpoint。
追加Reporter self-test菜单，以BRK b711验证报告通道，原默认项/ADT不变。
报告器不返回内核，不是永久修复，不冒充完整内核验收。

out/kernel-report-final/vendor_boot.img（77af03cd…）及vbmeta.img（8edf1237…）
沿用已有仅本地签名授权签出。签名与引用通过，其余22描述符/header/公钥不变。
指令模型、同版生产解析器、变更范围、LZMA/FAT/签名读回通过；独立ZXing将
四类fixture二维码解回完整OS帧，CRC正确。实机报告和扫码待验，未操作设备。

独立诊断源云编译 run43744001120 success（源018a7d7a…），产物及源码身份已复核。
之前43743753427的AARCH64对象编译通过，失败于macOS主机测试的ELF段名；已仅修
host-test分支并证实freestanding对象逐字节不变，不是XNU编译失败。
本地签名镜像仍为77af03cd…/8edf1237…，没有新设备操作。

## 实机文字报告已出现，撤掉默认主动停点 · 2026-11-16

用户照片中的reporter1显示 XNU CHECKPOINT (intentional stop)，关键字段为
ESR=f200b710 / FAR=0 / ELR=fffffff00c28ff68。确认C序言与重定位路径走过，
不是原始故障。图片未见QR，原因未定；仅记录图片哈希和可信转录，未上传图片。

用户要求在实际卡住处报告。本版默认不注入主动BRK，原PE_init_platform及
arm_vm_init调用继续；14个直接调用包装只记进入/返回。早期同步异常/panic
才触发终止式报告，正常其他向量转发原处理器。自检是独立菜单，不作默认。

修正报告器关闭MMU后仍使用KVA向量的源码问题，增设物理终止向量、QR进度行和
二次异常报告，避免编码再异常时静默。此修正尚不等于已确认旧图缺QR原因。
实际AARCH64报告/QR机器码模型运行到READY、独立ZXing解码、捕获/包装/向量模型、
生产解析器和payload/签名校验通过；实机后续页表阶段和纯死循环抓取未验证。

交付 out/kernel-passive-report-final/vendor_boot.img（9e8920b0…）及匹配
vbmeta.img（c75712b8…），沿用仅本地签名授权，未操作设备/根卷。
详见 docs/KERNEL_PASSIVE_REPORT.md 和机器记录 kernel-passive-report-build.json。


独立诊断源云验证 [run43747184259](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43747184259) success，源提交2fd02c6d7f7b5760a40c8397a4625cfa2e984b9c。实际AARCH64对象和主机传输检查通过，未重新编译整个XNU；签名测试对保持9e8920b0…/c75712b8…，实机结果待验。

## 自检确认未对齐写入，修正并增加实时文字 · 2026-11-16

自检图REPORT_PHASE=2，FIRST_ESR=f200b711，第二次ESR=96000061；
ELR=8c8829fc0对应stur w8,[sp,#0x1e]，SP=8c88b1e90，加偏移正好
FAR=8c88b1eae，32位写入未按4字节对齐。报告器关闭MMU时发生该异常。
确认是报告器自身问题，不是原XNU故障。旧模型未检查此条件。

全部freestanding C增加-mstrict-align；旧机器码对齐回归在相同PC失败，
新机器码在自然对齐检查下完成报告与QR READY，独立解码正确。
14调用包装新增实时文字，只覆盖顶部48行：临时独立TTBR0设备映射写GOP，
代码/栈走TTBR1，保持MMU/缓存开启，恢复旧TTBR0/TCR，不改原页表。
指令模型验证描述符、参数/结果/X18和栈保留、只写顶部、原控制恢复。

out/kernel-live-report-final/vendor_boot.img（f8688874…）及vbmeta.img（eb126c61…）
签名/hash/匹配引用通过，原22描述符/header/公钥不变。未操作设备或根卷，
未新做native/prelink；内核原始停点及新版实机行为待反馈。


源编译云检查 [run43749763327](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43749763327) success，提交7076a0036a87a633cba367292c3cc31c774287d3。AARCH64对象与主机传输检查通过，独立产物匹配；不是新native/prelink。实机QR和实时文字仍待验。


## 自检扫码通过，普通路径增加阶段 QR · 2026-11-16

用户已扫出 reporter 2 自检，ESR=f200b711 / ELR=fffffff00f8280bc。
因此自检的严格对齐修复和 OneSend 路径已有实机结果。普通启动最后报告
函数名 arm_vm_init（没有 enter/returned 前缀），没有 QR；此前普通路径
仅画文字，只有异常才生成 QR，不能把未出 QR 当作编码再次失败。

reporter 3 从进入 arm_vm_init 前开始，正常路径也先生成阶段 QR 再继续；
在其内部加入 10 个已验证直接调用的检查点，覆盖同一显示区，不累积色条。
独立 16 KiB 栈、有界堆重置、实际 GOP 范围的临时映射、TTBR0/TCR 恢复。
外层 arm_vm_init 和最终 set_mmu_ttb_alternate 使用原 LR 尾转移，最终
切换前恢复原 VBAR；保留原 panic 入口，避免页表切换后依赖附加诊断段。
到最后切换标记仅代表观察边界，不能认定该调用卡死。

实际 AARCH64 正常路径 QR 无异常运行通过；3200×2000/640×480 两种
实际绘制像素均被 ZXing 解回精确 752 字节帧。异常路径自然对齐回归、
24 调用/私有栈/原 LR 与 VBAR 模型、401 字节受限修改、同版生产解析器、
LZMA/FAT/签名读回通过。未重新 native/prelink，实机正常 QR 和原卡点待验。

交付 out/kernel-live-qr-final/vendor_boot.img（644cfe58…）与匹配
vbmeta.img（37de2e0f…）；沿用已有仅本地签名授权，其他22描述符/header/
公钥不变，无设备操作。见 docs/KERNEL_LIVE_QR.md 与构建证据
kernel-live-qr-build.json。


正常阶段 QR 的独立源码云检查 run43755493448 success，源提交
67893c542757b1b2e40c97c30903b63bb020b013；四份 AARCH64 ELF 对象与
重复帧主机测试通过。下载 artifact SHA-256 与 GitHub digest 完全一致，
源码提交/QR固定来源匹配。此检查不是重新 native/prelink 或实机验收。


## 实机到 VM 切换边界，固定文件轮播导出 · 2026-11-16

照片及用户补充确认最后阶段 vm/set_mmu_ttb_alternate#1，有静态码但
OneSend 收件不完成；该阶段是既有观察边界，不能认定真实故障位置。
固定 receiver 39e95ce5 的 OpticalReceiver 与 decodeTransferFile 对
旧最终帧独立接收成功；先收到前一阶段时最终会话被忽略。该复现解释设计
缺陷，尚不能认定它是用户此次相机接收失败的全部原因。

reporter 4 只在最终检查点发送同一报告，序号0/1/2/3的合法K=1帧
每500ms轮换；普通XNU项16次后继续，新增明确的 Export VM checkpoint
(animated; intentional pause) 持续轮播。提前缓存位图；编码/等待用
原页表，绘制短暂映射GOP即恢复；968像素M纠错QR置左侧。报告补充原
SCTLR/TCR/TTBR0/TTBR1，原始ARG0来自实际调用。没有后台硬件看门狗。

实际AARCH64普通路径16次轮播后返回、显式暂停路径20次仍持续的模型
通过，实际启动参数解析与映射恢复通过；实际像素被ZXing解回精确OS帧，
实际接收器从任意变体完成与源文本一致的文件。异常自然对齐、24调用
模型、403字节受限变更、生产解析器、LZMA/FAT/签名读回通过。

交付 out/kernel-animated-qr-final/vendor_boot.img（28ac87c3…）与
匹配 vbmeta.img（01982899…），沿用已有仅本地签名授权，其余22描述符/
header/公钥不变。未做设备操作或重新native/prelink；实机接收和原故障
待验。机器记录 docs/evidence/kernel-animated-qr-build.json。
