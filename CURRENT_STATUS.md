# Current status · 2026-11-21

## DLIL 等待两个启动线程 · 2026-11-21

照片 SHA-256 `016148c87d499416c00778902ac62bf3efa06e19b2fdbcd1a6aed2ad7e0edc00`。
屏显包含 run `44887638464` 新增的 0036 DLIL 标记和 CDC 入队行，完整构建标签
未入镜。CDC 已入队，入队时 `gic=1 timer=6602 irq=1`。DLIL SUM16 自检通过，
主输入与 detacher 两个线程创建都已返回。最后一行是
`waiting for main input and detacher`，页脚为 `dlwait`，`W=1`。
入口诊断尚未显示。定时器中断和 LiveLog worker 在这一启动中已经运行过。

`dlwait` 在获取 DLIL 互斥锁之前刷新。此后若仍有待确认线程，BSD 使用无超时
`msleep`，由两个 DLIL 线程确认后唤醒。复核同版二进制的初始内核栈、
`Switch_context` 与 `Call_continuation`，指针认证参数吻合。

0037（SHA-256 `b0cb990a16140e628775f42e565a40c95e4a12d3c3007dd585db1739804bad43`）
仅追踪这一段等待和相应的调度切换。屏幕底缘 `DLIL XX SEEN YYYY` 使用独立于
普通日志的固定帧缓冲区域。所有数值为十六进制，`XX` 为最近阶段，
`YYYY` 对阶段 N 使用 bit N-1。主要阶段为 `01` 等锁、`02` 已获锁、
`03` 调用 msleep、`08` 进入 thread_block、`09`/ `0A` 选择线程前后、
`0B` thread_invoke、`0C` 切换前、`0E` 新线程进入 thread_continue、
`0F` 调用 continuation 前。DLIL 主输入和 detacher 入口为 `06`/ `07`，
返回 BSD 为 `0D`，等待完成为 `05`。

补丁在前次精确输入上通过正向应用及逐字节反向恢复，三个 arm64e 对象编译通过。
独立帧缓冲渲染测试验证了写入范围，LiveXnu 19 组主机测试通过。
实机停点、CDC 枚举及根挂载待验证。

macOS `ufs-integrated` run
[44891383574](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44891383574)
在设备提交 `a2d2231db6295cdbaf2a7eab07de5031ede70634` 通过原生 arm64e 构建、
严格预链接及 production DarwinBoot parser。产物 `14702980093` 共 39,216,318 字节，
ZIP CRC 和 GitHub SHA-256
`9b276edbb3868fbf42e3521b011dedc0a8128ea6374f34084a6dced09999449f` 回读通过。
预链接内核 SHA-256 `04329df0a12417b191b19bc6735073c7949306adfb7c2c125c88ba0127cb6ce0`，
屏显构建标识 `src-a2d2231d-ramoops`。

打包工具来自该设备提交的 Git 导出。色带衍生内核 SHA-256
`6015345e62619d3319fdc1771bd9a942b5abc365abe0d366dbc677df165bf6cc`。
未签名镜像 `out/live-checkpoints-44891383574-dlil-scheduler-unsigned/vendor_boot.unsigned.img`
共 37,859,328 字节，SHA-256
`13c3101168bcc2df32f4d67ba83b31b21a3de51d1ef46b875cae4291dda32dc0`。
从 FAT/LZMA 回读的内核包含 0037 的三个作用域函数和两个帧缓冲函数，均为唯一的
强可执行定义。掌握的硬件证据尚未定位到具体调度操作，故本版用于诊断。

## CDC 枚举与 BSD 网络初始化 · 2026-11-21

本轮照片 SHA-256 `a53308cb37ee92efe025a532dd1c38bfa32e7e168f71a23003fa20353324a11a`。
日志越过 `cdc-start done`，最后同步显示 `shm-sem-init done`，当前照片无
CDC worker 或 UFS 发布线程行，无可核对的构建标签。Windows 的当前 PnP 设备和
SetupAPI 日志中没有找到 CDC 的 `1209:0001` 或 `TB710FUCDC001`。

已复核 run `44883138301` 的现场候选签名包，ZIP SHA-256
`3ca98ed35a5576b7684997323528b6e507578b769424fc2641573459ba069e2e`，
CRC、成员校验和、FAT 内核与启用 CDC 的启动参数通过。BSD 仅排队优先级 20
的 CDC worker，启动线程优先级 95。现有画面不能确认 worker 实际执行。

CDC 改用内核任务默认优先级，BSD 启动仍保持异步。新增入队回读（含 GIC、
定时器计数和 IRQ 状态）、worker 首条同步页脚、总线连接和首个 EP0 SETUP
标记。DWC3 在拉起 RUN_STOP 前提交 EP0 SETUP TRB，采用 Linux v6.1
`drivers/usb/dwc3/gadget.c` 的启动顺序。主机模型断言启动时已有 SETUP
TRB，并验证 CONNECT_DONE 和 Windows 常见的首轮描述符后总线复位不会重复提交。

XNU 0036（SHA-256 `9b5a4d69349c076b0ceac7b829070f1b3720a7cd0f4f55f670094e4fd37194a4`）
覆盖 `nwk_wq_init`、`dlil_init`、`socketinit`、`domaininit`
与 DLIL 线程启动和等待。补丁按前次产物的输入哈希复原，正向应用与反向恢复通过，
三个对象通过本地 arm64e 编译。三组 CDC 主机测试通过。Windows 枚举、
根挂载和用户态待实机验证。

macOS `ufs-integrated` run
[44887638464](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44887638464)
在设备提交 `9a15a799fe59b24b6dc3ff59a409d881e4a62ea4` 通过原生 arm64e 构建、
严格预链接和 production DarwinBoot parser。产物 `14701530033` 共 39,218,843 字节，
ZIP CRC 和 GitHub SHA-256
`6017d000386ff94c3373efde9b45e283ca18e1dd5f50a3899d37f1b3e5afe276` 回读通过。
预链接内核 SHA-256 `1cc5c833c6190dc582795fadac5072536f861068ba342f08e3d299cc9a764275`，
屏显标识 `src-9a15a799-ramoops`。

打包工具来自上述设备提交的 Git 导出。色带衍生内核 SHA-256
`3c33ca697438ce42c0b8a79996f7102ba78119e82cc228311176c74ba0859aaf`。
未签名镜像 `out/live-checkpoints-44887638464-cdc-network-unsigned/vendor_boot.unsigned.img`
共 37,859,328 字节，SHA-256
`95d3009155d77f7c89b954de581c160b70f1b8db3f467b50f4c029a5db62db07`。
独立回读通过 CDC 与 BSD/DLIL 标记、ADT、RootPolicies、`unixnu` 启动参数。
实机测试及签名待完成。

## CDC 异步启动与根卷发布修复 · 2026-11-21

实机照片 SHA-256 `c507fbd80ae442e881142381cc9c91431dd72c21a6530e7b0211540be836fb02`。
屏幕显示 UFS 控制器接管、LUN 就绪、`manifest=0 windows=3` 和
`source-storage-start done`，随后停在 `cdc-start begin`。
这段输出同时存在于 run `44874414139` 和 `44877673063`，照片里没有构建标识。
UFS 服务返回时只保证根卷发布线程已排队。此处还没有 BSD 根挂载结果。

CDC 现在由低优先级 worker 启动，BSD 钩子排队后立即返回。XNU 的 `delay()`
可进入定时器等待，本机已在 UFS 的同类等待中复现停点。短寄存器轮询改为此前
实机用过的计数器自旋。DWC3 事件优先通过 ArmCpu 的 level-high INTID 165
唤醒 worker，定时轮询保留作回退。固定同机 Linux 源 `4d044107…` 的
`sm8650.dtsi` 给出 SPI 133 和 TCSR wrapper 参考时钟。CDC 补齐该时钟，
读取 eUSB2 前先核对 PHY 复位和参考时钟，保留 GCC 分支的硬件门控位。
仅接管已处于 device 角色的控制器，因为 DCTL 不能停止 xHCI host。

UFS 0010（SHA-256 `9e68b032748a4a570e9c230061815281862314b16885e1faa21d33842e947d87`）
改为等待 IOKit 发布 UUID 和上游 registry ID 匹配的 APFS 介质。收到匹配后复核
实际 UUID 和 provider 祖先链。正常匹配由事件唤醒，30 秒失败超时仍需要定时器。
XNU 0035（SHA-256 `8b9d7fa62f633eb1203cf21cd4aef8c6e308e6220b5d16f1361d3ed29bd89f07`）
记录根 vnode、根卷认证、devfs 及初始进程 exec 边界。

三组 CDC 主机测试和 UFS 发布/命令/SMMU 主机测试通过。CDC 和 UFS 设备对象
均以 arm64e 编译通过。USB 枚举、根挂载和进入用户态待实机验证。

macOS `ufs-integrated` run
[44883138301](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44883138301)
在设备提交 `bb22cff99b33f03f8c6951a1d19559d116eb9c1c` 通过原生 arm64e 构建、
严格预链接及 production DarwinBoot parser。产物 `14698249745` 共 39,216,838 字节，
ZIP CRC 及 GitHub SHA-256
`dd1f6a4d982e3b2ec05636bf0f1c951164e7c8085f888a52a241fc9dedfe826d` 回读通过。
预链接内核 142,835,712 字节，SHA-256
`38729ac77d6d81f03c6126e35b7a3173dea06ab016ba60906270db0471d9f1ff`，
`mapping_complete=true`、`missing={}`、`stubbed={}`。屏显标识 `src-bb22cff9-ramoops`。

工作区存在本地 Linux 编译配方修改。打包从 Git 导出此 run 的精确设备提交，
以该版本的原始打包工具对照云端配方哈希，保留当前工作区修改。
衍生内核 SHA-256 `6f32ad269d027d1d66e05f82cf51be51b9c3c860e7669b463d7e3ef558a2ef3e`。
未签名镜像 `out/live-checkpoints-44883138301-cdc-async-unsigned/vendor_boot.unsigned.img`
共 37,859,328 字节，SHA-256
`9c27ae30a17334f58d9ff43dfbbb6a2bfcad5229019592e11d87c43232add210`。
FAT/LZMA 独立回读确认同次构建、原 ADT、RootPolicies、`tb710fu-root-partition=unixnu`
及 24 个检查标记。报告随镜像保存。签名及实机验证待完成。

## CDC 判定改由 BSD 线程打印 · 2026-11-21 15:12

上一版（run 44874414139）实机启动停在 `TB710FU bsd: cdc-start begin`，`done` 与任何
`TB710FU CDC:` 行都没出现，屏幕冻结在那里。钩子已经确认被 0034 挪到了正确位置，所以
问题在 `tb710fu_cdc_start()` 之内：要么 worker 卡在某次寄存器访问上，要么 worker 的
kprintf 与主线程的 kprintf 抢同一把控制台锁、双方都不返回。

改法：worker 线程从此**不调用任何 printf**，只把"即将执行的操作"写进 `gStage`
（`gcc-reset` / `gcc-gdsc` / `gcc-master` / `gcc-sleep` / `gcc-utmi` / `gcc-cfg-noc` /
`gcc-aggre-noc` / `gcc-sample` / `clock-check` / `phy-sample` / `smmu-route` /
`core-id` / `dma-alloc` / `dma-check` / `dma-preflight` / `vbus` / `dwc3-start` /
`ready`），并把八个 GCC 字、六个 PHY 字、core ID/GCTL、SMMU SCR0/ID0 采样到全局。
BSD 主线程最多等 100 ms 后一次性打印五行：`stage=... settled=... failure=...`、
`gcc ...`、`phy ...`、`dwc ...`、`platform up dma=...`（或 `failure` 名）。

- 构建 run `44877673063`（`d2e0aa7`）success；prelinked 内核 `1060cf88…eb81033`，142,835,712 B；
  色带衍生 `a1a34e19…7f0c442`，`verify_standard_bands` PASS；打包断言 10/10 marker 命中
  （含 `TB710FU CDC: stage=`、`gcc bcr=`、`phy rst=`、`platform up dma=`）。
- 签名：`vendor_boot.img` `aa13aab7…e48581`、`vbmeta.img` `2fa33c62…72e65f`、
  `boot.img` 仍为基线 `a3236acc…d817a83`。包 `out/live-checkpoints-44877673063-cdc-signed.zip`，
  35,560,871 B，SHA-256 `1cda433a4b280cd586f637ec3451be30f797b85f47082e2d75051d0bd0ef8a9c`，
  11 成员，ZIP CRC 与 SHA256SUMS 回读一致。证据 `docs/evidence/nufs-cdc-signed-44877673063.json`。
- 下一步判读：若 `stage=` 停在某个寄存器操作名且 `settled=0`，就是那个访问挂死
  （例如 `dma-alloc` 说明 `kmem_alloc_contig` 在 vfsinit 之后卡住，`gcc-gdsc` 说明
  GDSC 窗口不可访问）；若 `settled=1` 则 `failure=` 直接给出拒绝原因。

## USB CDC + BSD 链检查点镜像已签名 · 2026-11-21 14:29

CDC 平台自举与 0033 并进同一次构建，只发一套镜像，少刷一次机。

- 构建 run `44874414139`（`ca5c7f8`）success；prelinked 内核 `24194e56…359f20`，142,835,712 B；
  标准色带衍生 `d909abe4…bdc0c`，`verify_standard_bands` PASS（6 处色带、3 处紧凑探针、
  其他字节与段未变）。0033 的 15 个 bsd-* 标签与 0034 的新钩子位置都在这一份内核里。
- CDC 证据：内核含 `TB710FU CDC: gcc bcr=`、`phy rst=`、`USB2 platform up`、`controller role=`
  四条新字符串；`usb-cdc` 的 archive 与 compile report 按本次构建重新绑定（旧脚本沿用
  run-44713622044 的旧归档，已改成 rebind 到本 run）。
- 签名：`vendor_boot.img` `1921cc13…68e58b7`（100,663,296 B）；`vbmeta.img` `b60be65c…92a7926b`
  （8,000 B，只改 `vendor_boot` 一个描述符）；`boot.img` 与基线一致 `a3236acc…d817a83`。
- 包 `out/live-checkpoints-44874414139-cdc-signed.zip`，35,560,840 B，
  SHA-256 `8737ce3dca742dc315365cda430cf01c12372c790d920b6d8eedb0a8a0c3acfa`，11 成员，
  ZIP CRC 与 SHA256SUMS 回读一致。
- 证据：`docs/evidence/nufs-cdc-signed-44874414139.json`。
- 未验证：真实枚举。开机末尾应看到 `cdc-start begin` → 三行寄存器 → `USB2 platform up`
  或一条 `refuse ...`；后者直接指出是时钟、PHY、SID40 还是控制器。

## USB CDC 平台自举与可见性改造 · 2026-11-21 14:11

用户要求把 USB CDC 日志通道修通，避免调试只能靠拍屏幕。CDC 此前是严格"固件采用"：
只校验 GCC 时钟门、EUSB2 PHY 控制与 SID 0x40 恒等路由，任一谓词不过就 `refuse`
并返回；ABL 正常启动可能在充电检测结束后把 USB30_PRIM 整块关掉，而驱动明确不负责
开时钟和 PHY，于是"采用"路径先天依赖固件恰好留住状态。

- `CdcXnu.c` 增加有界 GCC 侧自举：解块复位（`0x39000`）、开 USB30_PRIM GDSC
  （`0x39004` 控制 / `0x39008` 状态 bit16）、开五路分支（`0x39018`、`0x39024`、
  `0x39028`、`0x39088`、`0x3908c`；只写软件 enable，清硬件门控投票，保持只读 halt 位）。
  这些寄存器都在常开的 GCC 窗口，写操作打不到 USB 互联；GDSC 只门控 DWC3 与 PHY。
  PHY、PMIC/中继器、SMMU、UFS 仍然只采用不编程。
- `GCTL.PrtCapDir` 从硬性拒绝改为只报不改：`tb710fu_dwc3_start()` 本来就会 halt 内核
  并写 device 模式；只保留 core ID 与 DMA 位宽两条硬要求。
- 每次尝试先打两行紧凑寄存器（`gcc ...`、`phy ...`）再给判定；`tb710fu_cdc_start()`
  最多等 worker 20 ms，保证判定停在日志尾部。
- 新补丁 `patches/xnu/0034-tb710fu-cdc-start-after-storage.patch` 把 0012 的 CDC 钩子
  从 `vfsinit` 之前移到 `source-storage-start done` 之后，让判定落在照片最后一屏。
  补丁第二块带 0033 上下文，必须排在 0033 之后。
- 宿主检查（WSL gcc）：`check_usb_cdc_protocol.py`、`check_usb_cdc_hardware.py`、
  `check_usb_cdc_transport.py` 全 PASS；`git apply --check` 对 post-0033 树返回 0。
- 未验证：真实枚举。既有任何一次启动都没抓到 `TB710FU CDC:` 行，究竟卡在时钟、PHY、
  SID40 还是控制器，要等这次镜像开机的最后两行。

## 0009 静默镜像已签名 · 2026-11-21 13:49

用户授权本地签名。用 run 44870542276 的 prelinked 内核走标准色带衍生 → 无签名
包装 → AVB 签名 → 回读校验，产出双系统镜像集。

- 内核：`714a2a18bfee9ea4a0a30c717dd54b6ac41e3364a6f02e0833f2fa4080ac7805`，
  142,835,712 B；标准色带衍生 `e4ddacf214a8063ce4d35b4194bc2bb458c5542ae506f3629cc3a0d1872b603f`。
- `boot.img` `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`
  （基线 loader，未变）；`vendor_boot.img` `64d2d9596a8d47b780e33ad53f3d5bfe397e634f3748f3183e5370af755ef558`，
  100,663,296 B；`vbmeta.img` `30454ef3808cfa44ab9fd0c14ad8a64819ece95be2834fd9e10c082eb1dac76d`，
  8,000 B。vbmeta 只改 `vendor_boot` 一个描述符，其余 22 个与基线一致。
- 包 `out/live-checkpoints-44870542276-quiet0009-signed.zip`，35,559,421 B，
  SHA-256 `94a509a85f398462a91db97cf849eb05dd70379db57ad81da4dfce2f4fb3501a`；
  11 个成员，ZIP CRC 与 SHA256SUMS 回读一致。
- 独立复核：`avbtool verify_image` 对 `vendor_boot.img` 与 `boot.img` 均报
  `Successfully verified footer and SHA256_RSA4096 vbmeta struct`。
- 证据：`docs/evidence/nufs-quiet0009-signed-44870542276.json`。
- 未验证：硬件启动与真实停点。按 `TESTING.txt` 成对写 `vendor_boot_b` 与
  `vbmeta_b`，默认启动应当安静，只看最后一个 `ufs*` 标签和 CKPT-TEXT 页脚；
  需要某阶段的逐命令细节时再加 `tb710fu-cmd-trace=1`。

## 0009 探针静默补丁：设备端声明顺序修正 · 2026-11-21 13:55

用户要求撤掉每次开机刷屏的 `cmd-*` 日志、保留停点诊断。0009 把
`RootCommands.h` 与 `TransferPoll.h` 的命令/自旋探针改成运行期开关：
`NativeUfs.cpp` 在 `start()` 里读 `tb710fu-cmd-trace` boot argument，默认关闭；
`ufs*` 阶段标签与 CKPT-TEXT 页脚保持无条件输出，安静配置下停点仍然可见。

本轮发现并修掉一个只在设备端暴露的编译错误：0009 初版把
`#define TB710FU_CMD_CHECKPOINT_PROVIDER 1` 提到两个 include 之前，但
`ufsCmdTraceEnabled()` 的定义仍留在 include 之后。`TransferPoll.h` 的探针在
模板函数体内，clang 在解析模板定义时就做非依赖名字查找，设备编译报
`use of undeclared identifier 'ufsCmdTraceEnabled'`；host 构建没有 provider，
整段被宏擦除，宿主测试看不到这个问题。修复是把 `gCmdTrace` 与
`ufsCmdTraceEnabled()` 一并前移到两个 include 之前。

- 补丁 `patches/ufs/0009-quiet-command-probes.patch`，12,879 B，SHA-256
  `71350d0d47b15a4b6715aa0eb85edcef13224cff6dbd8b2871804bb31c4becc3`；
  `build-tb710fu-ufs-adapter.py` 的 PATCHES 已加入 0009。
- 验证器 `tools/check_ufs_bringup_trace.py` 追加 0009 应用与守卫：开关与访问器
  早于 include、`tb710fu-cmd-trace` 只读一次、16 处命令探针全部走门控、自旋
  探针共用同一开关、24 个阶段标签数量不变、MMIO/命令/panic/错误串不变，
  **57/57 PASS**。
- 适配器宿主测试（WSL clang 19.1.7，`-Werror`）：`SmmuTakeover` 与
  `RootCommands` 均 PASS，`host-tests.json` 记录 9 个补丁哈希。
- 环境注记：0007 补丁是 CRLF，WSL 的 Linux git 未设 `core.autocrlf=true` 时在
  LF 暂存树上匹配失败；Windows 侧或 `GIT_CONFIG_*` 注入后正常。验证器建议走
  Windows python 运行。
- 设备端编译已由云构建确认：run 44870542276（commit `738269c`，
  stage=ufs-integrated，success，05:21:58–05:31:18Z）。step 10 内的 archive-08
  调用 `build-tb710fu-source-storage-archive.py`，`source-storage/build-report.json`
  的 board_patches 共 9 项，0009 的 SHA-256 是 `71350d0d47b15a4b…`；
  `NativeUfs.log` 只有既有的隐式转换/符号警告，没有 `ufsCmdTraceEnabled`
  未声明错误，说明顺序修正在设备配置下成立。archive
  `5566c7b52ea73b6397f62fa273d22579038a4aced207875bb9f694e8fa071c74`，1,479,296 B；
  同一次构建的 source-storage host tests 2/2 PASS。step 13/15 因舞台是
  ufs-integrated 而 skipped，adapter 与独立 archive 任务没有跑。
- 本地仍未跑完整设备编译：11:47 起的内核构建仍在 bsd/osfmk，`build/storage-abi`
  头未生成，adapter 的设备编译路径缺 `startup.o.json`。

## clang 四参数 `__ptrauth` 补丁定稿 · 2026-11-21 11:45

本地 Linux/WSL arm64e 工具链打通后的收尾：把 `llvm-ptrauth-null-values.patch`
从"只改 Sema 的旧版"重做成覆盖三层检查、且不含 builtin-type 污染的干净补丁。

- 定稿文件 `patches/llvm/llvm-ptrauth-null-values.patch`，5349 B，5 hunk，
  4 文件（`ParseDecl.cpp`、`DiagnosticParseKinds.td`、`Attr.td`、
  `SemaType.cpp`×2），SHA-256 `12aec19e13be0df8f96dc1a5d3687bffb0b9cad06e8b1b93d7e446abc29aeb22`。
  工作副本 `.work/UniXNU/scripts/linux/llvm-ptrauth-null-values.patch` 同步，
  `setup-toolchain.py:220` 在 `llvm-xnu-builtin-type.patch` 之后应用。
- 污染根因：旧生成器以"已应用 builtin-type 的工作树"当 baseline，把
  `XnuUsageSemantics` 混进本补丁，重装工具链会重复应用。现改为
  baseline = 纯净 tarball + builtin-type 的 `Attr.td` hunk。
- 验证：`patch -p1` 与 `git apply` 均干净应用；应用结果与
  `~/tb710fu-llvm-project` 四个文件 SHA-256 全同。该树的 clang（`ninja exit=0`）
  已编出 arm64e `libfirehose_kernel.a`，`clang-24` 报
  `'__ptrauth' qualifier must take between 1 and 4 arguments`。
- 脚本：`.work/regen-patch-v3.py`、`.work/verify-patch-v3.py`。
- 未做：clang 全量重编没有再跑（补丁与已重建源码树逐字节等价）；
  本地 `prepare-xnu.py libs` 仍卡 Libc 的 `GnuTLS handshake`，属网络问题，与补丁无关。

## 0008 双系统运行时分区名补丁 · 云构建中 · 2026-11-21

用户目标：保留安卓系统，给安卓 data 留 70 GiB，其余建 macOS 根卷分区；即官方
`dual-system-partition.md` + vermeer 板的 `*-root-partition` boot arg 方案。

设备现场（TWRP，HA2HZAW2 / topaz，只读勘察）：`userdata` = sda14，4096B 扇区，
start 5105064 / end 61390842 = 214.71 GiB；`metadata` = sda11 64 MiB；`/data` 未挂载。
userdata 首块是加密随机字节（Android data），不是 SHNGROOT —— 根卷从未刷入，
所以此前实机停在 `ufsscsi` / `ERR=0`（GPT 读成功、清单无效）。GPT `Total free space is 0 sectors`。
本机 userdata 多层加密（`inlinecrypt+wrappedkey+metadata_encryption`），官方 `dual_sys`
在线缩容工具会拒绝，必须手动 sgdisk 硬切（清空安卓 data）。

精确切分数值（4096B LBA，已核算）：
- userdata 70.00 GiB: LBA 5105064..23455143（18350080 扇区，75,161,927,680 B）
- unixnu 144.71 GiB: LBA 23455144..61390842（37935699 扇区，155,384,623,104 B）
- macOS 根镜像 17,603,493,888 B（16.39 GiB）刷入 unixnu 后余 ~128 GiB 给 Data store+日志。

官方机制核实（vermeer 板 `5945dda` / fork `bf21739`）：`RootLayout.h` 把硬编码
`userdata` 换成 `extern const char *gRootPartitionName`，`NativeUfs.cpp` 定义它并从
boot arg 设置，默认 `userdata`；加 GPT 名 NUL 终止检查。boot arg 名为本机化的
`tb710fu-root-partition`（本机已确认 `PE_parse_boot_arg_str` 在 24A446 prelink 白名单内）。

本轮实现：`patches/ufs/0008-runtime-root-partition.patch`
SHA-256 `0811c2a0146c60b8ac3744d9e58a6df720f48e2e81f3e24af247e68800579683`（110 行）。
改 4 个文件：RootLayout.h（运行时名+终止检查）、NativeUfs.cpp（默认定义+start 读 boot arg，
静态缓冲避免悬空）、tests/RootLayout.cpp 与 tests/RootPathBridge.cpp（定义符号）。
无 MMIO/cast/命令/错误串/panic 行为改动。

验证：`tools/check_ufs_bringup_trace.py` 追加 0008 应用与符号守卫，**46/46 PASS**，
原行为守卫（MMIO/cast/write32/IOSleep/错误串/panic）全部不变；全 7 补丁序列
(0001,0002,0004,0005,0006,0007,0008) 在隔离 git 环境 apply 干净且符号确认存在。
注意基线是 git archive + 0001-0007（NativeUfs.cpp 2596 行），不是 feed 缓存已应用副本。
本机无 clang++，host C++ 编译交 macOS 云构建。

接线：`build-tb710fu-ufs-adapter.py` 的 PATCHES 加入 0008；
`package_source_storage.py` boot-arg 白名单加入 `tb710fu-root-partition`；
`patches/ufs/README.md` 记录 0008。提交 `97f5aed`（5 文件），已推送
`origin/codex/ufs-transactional-handoff`（d0f6416..97f5aed）。构建前工作区另有本会话
之前就存在的 `docs/USERDATA_BUILD.md` 改动与 `docs/HANDOFF_2026-11-18_QR_DIAGNOSTICS.md`，
未纳入本次提交。

签名包已就绪（2026-11-21）：run 44835102754 的 kernel.macho SHA-256
`230ff011860cc1f2c62d9b3d58b7523c4dd1abf333894d10909b76462f319311`（142,835,712 B，
arm64e），0008 的 `tb710fu-root-partition`/`gRootPartitionName` 已确认编入 kernel 与
source-storage archive。经标准色带变换（核 SHA
`85e0dedb0163ff3f0337a7c10bf9dc4d2e3813d49615eb0568ea16eaf60ec9e8`）后打包签名。
签名三件套：boot.img `a3236acc…817a83`（loader 基线不变）、
vendor_boot.img `b528bc7f21b0175d7102d124dabdcf80c2a4e030b9afdefbae6a44831735668a`、
vbmeta.img `9b3398ccb321e0e9c1cce3e79b74c4b8d439d29a107e41dc69f1ac51dfa07048`。
交付包 `out/live-checkpoints-44835102754-dualpartition-signed.zip`（35,559,400 B，
SHA-256 `01ebfa3aca23dff141b135bad76db0d324343b4a21d404ecc5c1245ade516ba1`，11 成员，
ZIP CRC/SHA256SUMS PASS）。boot args 含 `tb710fu-root-partition=unixnu`，6/6 打包标记
验证通过，AVB SHA256_RSA4096 签名/salted hash/public key unchanged 全 PASS。
证据 `docs/evidence/nufs-dualpartition-signed-44835102754.json`。display label
`src-af038ef4-ramoops`。**尚未刷写或操作设备**；`device_operations=false`、
`hardware_boot_verified=false`、`runtime_qualified=false`。

云构建：stage=ufs-integrated run **44831350082**，commit `97f5aed` —— **failure**。
失败在 step 10 内的 archive-08（source-storage archive 编译 NativeUfs.cpp）：
`NativeUfs.cpp:2347: reference to 'gRootPartitionName' is ambiguous`——`RootLayout.h` 在
NativeUfs.cpp 的 anonymous namespace 内 include，其 `extern` 声明属于 anon namespace，
而 0008 初版把定义放在全局，两个命名空间同名符号导致歧义（-Werror 下成 error）。
注意 job 日志里的 `_tb710fu_live_log_putc/refresh` 链接错误是基线既有现象（上一成功
run 44820012647 同样出现且 success），不是失败原因；真正原因是 archive-08 的 ambiguous。

修复：定义移进与 RootLayout.h 声明相同的 anonymous namespace（紧跟 include 之后），
host 测试仍在全局定义（其 include 在全局）。WSL clang++ 最小复现确认 OLD 版本触发
`-Wundefined-internal`、NEW 版本干净通过。新补丁 SHA-256
`205b6ca14918340087afdad22441e043baa6c69203985ad34663510bf6a433e6`。
workflow verifier 仍 46/46 PASS。提交 `af038ef` 已推送，重发云构建 run
**44835102754**（commit `af038ef`，stage=ufs-integrated），本条记录时 `in_progress`。
待产物复核确认 0008 编入内核。

依赖链（顺序不可颠倒）：①内核认 unixnu（本轮，构建中）→ ②boot args 带
`tb710fu-root-partition=unixnu`（打包 manifest 阶段加）→ ③设备 GPT 切 70G+144.7G 并刷机。
③是不可逆设备写操作（清空安卓 userdata），**本轮未执行**。Data seed 是根卷挂载成功
之后的独立阶段，排在刷机验证之后，本轮未做。
`hardware_boot_verified=false`、`runtime_qualified=false`、`device_operations=false`。

## 0007 实机越过 hostBringUp，停在 root manifest · 2026-11-21

新照片确认 0007 生效：`host lun-ready-done attempt=0` 首次出现，随后
`host root bring-up-done` 与 `host root layout-begin`。页脚从上次的 `ufssetl`
前进到 `ufsscsi`，`host wake-settle-begin` 之后的停点已经不存在。TUR 第一次
尝试即成功，UFS 传输层与 `hostBringUp()` 均已完成。

`root layout-begin` 之后是三条连续的 READ(10)：`op=28 len=10 dir=2
bytes=4096 timeout=1000000`，每条都 `cmd-return end=0 ocs=0 polls=1`、
`rsp-head b0=21 b1=00 b2=00 b3=00 b6=00 ... want=21/0/0`、
`rsp-ok len=0`、`scsi-done status=0`。设备 GPT 是 **32 项表**（`.work/lun_gpt.txt`），
`(32 × 128 + 4095) / 4096 = 1`，所以这三条恰好对应 `findRootLayout()` 的三次
`hostRead4k`：GPT 头（LBA 1）、GPT 项表（1 块）、userdata 首块（1 块）。三次
读取全部成功，`findRootLayout()` 的 GPT 校验、CRC、分区边界与 userdata 查找
都通过。

但 `ufssuper` 没有出现。`ufssuper` 位于 `findRootLayout()` 成功之后、清单
状态检查之后；它缺失说明 `layout->manifestStatus != 0`，即 userdata 首块不是
有效的 `SHNGROOT` 清单。`startRoot()` 对这个分支只调用
`rootDiagnosticFailure()` 与 `IOLog()`，后者不上文字控制台；诊断开关关闭时
`rootDiagnosticFailure()` 也不 panic，所以屏幕静默停在 `ufsscsi`，`ERR=0`。
这与 `docs/USERDATA_BUILD.md` 的记录一致：`userdata-root-vroot.img` 尚未写入
设备，userdata 仍是 Android data。

页脚为 `CKPT=TEXT LOG=1006471 QR=998469 W=1 N=18 Q=1 B=0/1 R=1 ERR=0
ufsscsi`。

本地待刷镜像已核对：`.work/root-policy-run-43724852936/userdata-root-vroot.img`
共 17,603,493,888 字节，SHA-256
`35b1c581c7074c626b4b3750af93901a988425e42671f1aaeafd7fd6e13de199`；首块
`SHNGROOT`、version 2、6 个条目、`image_bytes` 17,603,493,888，与
`userdata-root-vroot.json` 的三个窗口、三个 `.vroot` 对象一致。报告给出的
刷写命令是
`fastboot flash userdata .work/root-policy-run-43724852936/userdata-root-vroot.img`。

下一步是设备侧写入 userdata；这会覆盖现有 Android data，属于设备操作，本轮
未执行。`hardware_boot_verified=false`、`runtime_qualified=false`、
`device_operations=false` 保持不变。若写入后仍停在同一位置，再需要一条
`root layout-done status/magic` 的屏幕打印来区分清单解析与发布阶段。

## 计数器自旋修复镜像已签出（0007）· 2026-11-21

04:59 设备照片确认 0006 仍停在同一处：最后一行 `NativeUfs: host
wake-settle-begin`，页脚 `CKPT=TEXT LOG=1002956 QR=996628 W=1 N=18 Q=1 B=0/1
R=1 ERR=0 ufssetl`，既没有 `wake-settle-done`，也没有 `ufssetd`。唤醒命令本身
仍是完整成功（`cmd-return end=0 ocs=0 polls=1`、`rsp-head b0=21 b2=d0 b3=00`
匹配 `want=21/208/0`、`scsi-done status=0`）。

根因：0006 把 `IOSleep` 换成 `IODelay` 的前提是后者忙等，但 UniXNU 文档与
`ArmCpu.cpp` 说明，线程没有注册 idle latency 时每次 `IODelay` 都落到定时器等待
（`armcpu-idle-latency-us` 默认 0）。因此 0006 与 `IOSleep` 一样不会被唤醒。
命令轮询里的 `IODelay(2)` 同样受影响，TUR 重试也会踩到。

修复单独成补丁 `patches/ufs/0007-quiescent-counter-spin.patch`（SHA-256
`1c513341095bbf0b930e14377ce11988251f9e8b5c1f8cb6f73c6698e358ab60`）：

- 新增 `hostSpinUs()`，用 `clock_interval_to_deadline` + `mach_absolute_time()`
  有界自旋，循环内 `yield`，不依赖任何中断或调度器唤醒。
- settle 改为自旋 50 ms，并在前后加 `ufsspn` / `ufssetd` 检查点；
  TUR 每次尝试打印 `host lun-ready-attempt=N` 并加 `ufstur`，重试间隔自旋 10 ms。
- `HardwareTransferPort::delay()` 的 2 µs 也切到同一自旋；host 测试仍用
  `IODelay`（其时钟只在 delay 回调中推进）。通过
  `TB710FU_CMD_SPIN_PROVIDER` 编译期切换，未改注册写、超时、所有权或错误字符串。

`tools/check_ufs_bringup_trace.py` 已扩展为应用 0007 并断言自旋函数、两处自旋、
三个新检查点、命令路径切换与 host 路径保留 `IODelay`，全套 46 项 PASS。
提交 `64c8dfb479472c11d5ec15ce01b8eba29edd9a04` 已推送到
`codex/ufs-transactional-handoff`。

macOS `ufs-integrated` run [44820012647](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44820012647)
在该提交上成功。产物 `14672625838` 共 39,207,560 字节。预链接内核 142,835,712
字节，SHA-256
`ab4f809214830a1cfff782a4dbf33056cf055831b201a326600419fb9d17bdf5`，
`cpu_subtype` `0xc0000002`（arm64e），屏显 `src-64c8dfb4-ramoops`。内核扫描
确认 31 个 `ufs*` 检查点标签（新增 `ufsspn`、`ufstur`）与 14 个必需标记
（含 `host wake-settle-spin now=`、`host wake-settle-done elapsed=`、
`host lun-ready-attempt=`）全部编入。

签名三件套：`boot.img` `a3236acc…817a83`（基线），`vendor_boot.img`
`298f3efd0b9825c926f56957296fb7bbf0fbd2453f76a7488ad5940c24514a8f`，
`vbmeta.img` `d7b5d2de19c3a0964b82fc1e8bbe5b3f403ca41f9cd82126853ba6ae5213b9f0`。
交付包 `out/live-checkpoints-44820012647-spin-signed.zip` 共 35,559,220 字节，
SHA-256
`7ab84f1bce94364c5d0f606c04e415d9b8b9db701b7be38622a393e52d28d9e4`，11 成员。
独立复核全部通过：zip 完整性与逐成员校验、AVB `verify_image` 对内嵌公钥验证
（WSL 下必须改走 `/usr/bin/openssl`，随包的 `openssl.exe` 无法写临时文件）、
vbmeta 头签名与基线头逐字节一致、加盐哈希、未签名体逐字节保留、LZMA 解回的
内核为带区衍生体且含 31 个标签与全部 14 个标记。证据见
`docs/evidence/nufs-spin-signed-44820012647.json`。

尚未刷写或操作设备。`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。

预期本次实机表现：页脚应依次出现 `ufsspn`（自旋开始检查点已刷出）、50 ms 后
`ufssetd`，并打印 `host wake-settle-spin now=` 与 `host wake-settle-done
elapsed=`；随后是 `ufslun`、`host lun-ready-begin`、`ufstur` 与
`host lun-ready-attempt=0`。若页脚停在 `ufssetl`，说明检查点本身没有发布；
若停在 `ufsspn` 而没有 `ufssetd`，说明 `mach_absolute_time` 在此上下文不推进
（与此前每条 `cmd-deadline` 的证据矛盾，需要另查）。

## 忙等修复镜像已签出（0006）· 2026-11-21

03:52 照片把停点定死在 `IOSleep (50)`：唤醒命令完整成功（`cmd-return end=0
ocs=0 polls=1`，`rsp-head b0=21 b2=d0 b3=00` 匹配 `want=21/208/0`，
`rsp-ok len=0`，`scsi-tail scsi=0000 resid=0 len=0`，`scsi-done status=0`），
`host wake-done` 已打印；但 `host wake-settle-begin` 之后再无
`wake-settle-done`，页脚停在 `ufssetl`（`ufssetd` 未出现）。`hostScsi` 返回与
`lun-ready-begin` 之间唯一的阻塞调用就是这两处睡眠。

根因：UFS 命令路径以 `IODelay` 轮询运行在屏蔽中断上下文（`RootCommands.h`
注明命令只轮询，因此也在关中断下运行），`IOSleep` 需要调度器唤醒线程，
在此上下文中永不被唤醒，线程不再返回，且不产生 panic。

修复单独成补丁 `patches/ufs/0006-quiescent-sleep-replacement.patch`，把
`hostBringUp` 内两处睡眠换成等时长忙等：

```
for (unsigned settle = 0; settle < 50; settle++) IODelay (1000);   // 原 IOSleep(50)
for (unsigned pace  = 0; pace  < 10; pace++)  IODelay (1000);     // 原 IOSleep(10)
```

职责分离：0004 保持纯追踪（其"剥离追踪后与 post-0002 逐字符相同"的等价性
证明不受影响），行为改动只落在 0006。验证器 `tools/check_ufs_bringup_trace.py`
已扩展为应用 0006 并断言：`hostBringUp` 内无睡眠调用、两处忙等均就位、
文件中其余睡眠未被触碰（计数先剥注释，避免把 0006 注释里提到的
`IOSleep(50)` 误判为调用）。全套 37 项 PASS，host 测试 `-Werror` 编译 PASS。

macOS `ufs-integrated` run [44809442859](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44809442859)
在设备提交 `a25fd1c292a9dfc42b4010ad0ab3f5f0aac1871f` 成功。产物
`14665422014` 共 39,206,909 字节。预链接内核 SHA-256
`17a8f0c2d5a3b41ee7f97f04b4e1363a79ffb43f6829717e9d2e6c33768bf6b9`，
`cpu_subtype` `0xc0000002`（arm64e），屏显 `src-a25fd1c2-ramoops`。内核扫描
确认 29 个 `ufs*` checkpoint 标签与 53 条 host 阶段探针全部编入。

签名三件套：`boot.img` `a3236acc…817a83`（基线），`vendor_boot.img`
`59897e27e65ac2792fc27ada8a0139b3fc130614f609079e6175e1090ad05e4a`，
`vbmeta.img` `9318ba88f1b4a690b73923bb46717743d34c425e7271ce50473fabcb2e23775a`。
交付包 `out/live-checkpoints-44809442859-busywait-signed.zip` 共 35,559,324
字节，SHA-256 `88e4f1c5abacc4da5722ddffe6f76602938eec4676f732fb92773859cc914cf7`，
11 成员。独立复核全部通过：zip 完整性、`SHA256SUMS` 覆盖每个成员、AVB
vbmeta 签名对内嵌公钥验证通过、vbmeta 头与公钥未变、恰好一个描述符变更
（`vendor_boot`）、加盐哈希与签名体一致、未签名体逐字节保留、以及从 FAT
载荷中 LZMA 解回的内核含全部 29 个标签与 10 个必需标记。证据见
`docs/evidence/nufs-busywait-signed-44809442859.json`。

尚未刷写或操作设备。`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。

预期本次实机表现：页脚从 `ufssetl` 前进到 `ufssetd`，随后出现
`host lun-ready-begin` 与 `host lun-ready-done attempt=N`。若页脚仍停在
`ufssetl`，则说明忙等本身也未走通，需要另行定位。

## IOSleep 前后探针镜像已签出 · 2026-11-21

03:19 照片确认唤醒命令完整成功：`rsp-head b0=21 b2=d0 b3=00` 与
`want=21/208/0` 一致，`rsp-ok len=0`，`scsi-tail scsi=0000 resid=0 len=0`，
`scsi-done status=0`。但 `lun-ready-begin` 仍未出现，页脚停在 `ufsscsi`。
`hostScsi` 返回与 `lun-ready-begin` 之间唯一的阻塞调用是 `IOSleep (50)`；
该路径以 `IODelay` 轮询运行在屏蔽中断上下文，调度器睡眠是候选原因。补丁
0004 因此在它前后各加一条 `printf`（`wake-settle-begin` / `wake-settle-done`），
并在 TUR 重试循环的 `IOSleep (10)` 前加 `tur-retry`，**不改动睡眠本身**，
以便下一次实机运行显示是否进入、是否返回。

期间曾尝试直接替换两处 `IOSleep` 为忙等，但那会把 0004 从纯追踪补丁变成
行为补丁，破坏验证器基于"剥离追踪后与 post-0002 逐字符相同"的等价性证明，
已全部回退（验证器改动同样回退）。当前 0004 仍为纯追踪：
SHA-256 `a61af2a6ac6f378ea090170a3e4fa3a864ab7bf75b90e4644b31ca916e6a196c`。

macOS `ufs-integrated` run [44798948486](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44798948486)
在设备提交 `eb4b9e305a716f532b65fa417b74fdd492ce6195` 成功。产物
`14660777748` 共 39,205,997 字节。预链接内核 SHA-256
`8589b5af19a167e8d97e6e24919742d7e6a3cca321596b3721d55ebc89e8d750`，屏显
`src-eb4b9e30-ramoops`。内核扫描确认两条 settle 探针与 `tur-retry` 编入，
`ufssetl`/`ufssetd` 不存在。

签名三件套：`boot.img` `a3236acc…817a83`（基线），`vendor_boot.img`
`cfc07b94714e328863df7791c252741696199b0e0b74b6029ac41efc38fe7929`，
`vbmeta.img` `fc21e65922d3f52f1211f08749ab0c21000225b9f77ead26bffe9dc4daf15251`。
交付包 `out/live-checkpoints-44798948486-settle-signed.zip` 共 35,558,893 字节，
SHA-256
`39091acdb03cd826b3fa6194254a0d769071c27eae5ad25b90a655cf3db3f274`，11 成员。
独立复核全部通过。证据见
`docs/evidence/nufs-settle-signed-44798948486.json`。

尚未刷写或操作设备。`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。

## SCSI 响应头探针镜像已签出 · 2026-11-21

实机 `ufscret` 确认唤醒命令完成（`end=0 ocs=0 polls=1`），但 `wake-failed` 与
`lun-ready-begin` 都未出现，停点落在 `cmd-return` 与调用方下一条语句之间。
UCD 边界已核对：`kUcdOffset=4096` + `kRspOffset=512`，到 `kPrdtOffset=1024`
尚有 512 字节，`rsp+34` 在范围内，不存在越界。补丁 0004 因此在
`hostResponseHeader` 与 `hostScsiDir` 尾部增加探针：`rsp-head` 逐字节打印响应
头与期望的 type/lun/tag，`rsp-bad type|tag|lun|len` 指明具体拒绝原因，
`rsp-ok len=N` 表示通过，`scsi-tail` 打印 status/residual/length，
`scsi-done status=N` 打印最终 IOReturn，并加 `ufsrsp`、`ufsscsi` 检查点强制刷屏。

macOS `ufs-integrated` run [44792429573](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44792429573)
在设备提交 `a062a6b62768b2a3d8b0694050091b607b03f140` 成功。产物
`14658890436` 共 39,205,794 字节，SHA-256
`abfcd7ffb61a1c7a012651154bd5b2b1dc7d3300e7a7be016fc4b8581e7e267b`。预链接
内核 SHA-256
`5ca480710ce2487246a8f274b839a79f1a2c7da0daca00785285d2a4032890ac`，屏显
`src-a062a6b6-ramoops`。内核扫描确认 8 条响应头探针、2 个新检查点与既有
cmd/poll 探针全部编入。

签名三件套：`boot.img` `a3236acc…817a83`（基线），`vendor_boot.img`
`ad32a55ee23706ec9eff840a190a8745a83cf974c3149e9e5cd713ab4c473a08`，
`vbmeta.img` `e8df4d4cdd4b8d0ec5f0668bac9826e1e32a2cf9341b9e11d754a623c8b1c72d`。
交付包 `out/live-checkpoints-44792429573-rsp-signed.zip` 共 35,559,142 字节，
SHA-256
`47e36ee11dfef429f07720a3c51e319760e1dc75be8f424a021b47fc8ddf66d8`，11 成员。
独立复核全部通过。证据见 `docs/evidence/nufs-rsp-signed-44792429573.json`。

尚未刷写或操作设备。`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。

## SCSI 命令每步同步屏显镜像已签出 · 2026-11-21

实机页脚 `ufswake` 确认镜像生效，执行进入 `host wake-begin`。NOP 与 power-query
均打印 `cmd-return end=0 ocs=0 polls=1`，而 wake 之后没有 `cmd-enter`——但那两条
原生命令走 `hostSubmit`、不经 `hostScsiDir`，本来就没有 `cmd-enter`；wake 是第
一条进入 `hostScsiDir` 的命令，其 `printf` 可能仍留在环形缓冲区未上屏（页脚
QR 落后 LOG 4547 字节，说明显示 worker 已停摆）。因此给命令路径每一步加
`ufsCheckpoint` 强制刷屏：`ufscent` / `ufscucd` / `ufscsub` / `ufscdb` /
`ufscring` / `ufscret`。

`RootCommands.h` 也被 `ShengUfs/tests` 直接包含，直接调用 `ufsCheckpoint` 会
编译失败（run `44783303560` 报了 8 处 `use of undeclared identifier`）。
`weak_import` 在 host 侧同样不可用（`-Werror=attributes` 与 `-Werror=address`）。
最终改为编译期开关：`NativeUfs.cpp` 在 include 前 `#define
TB710FU_CMD_CHECKPOINT_PROVIDER 1`，头文件据此展开为真实调用或 `((void)0)`。
本地用 `c++ -Wall -Wextra -Werror` 编译 `tests/RootCommandsBridge.cpp` 通过。

macOS `ufs-integrated` run [44785665693](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44785665693)
在设备提交 `8eb1a7d63f66ad1a6980dd0ad05e79af7ebb3657` 成功。产物
`14653774042` 共 39,204,195 字节，SHA-256
`42b7dc336a5d6fedf2f52937944ff856f2a856555e21a4729940c35d289e4960`。预链接
内核 SHA-256
`8496ed8e79d3120c234df92962ea427be60ec6ffab91f7befe384fe556bd9627`，屏显
`src-8eb1a7d6-ramoops`。内核扫描确认 6 个命令检查点、全部 cmd/poll 探针与既有
阶段追踪均在。

签名三件套：`boot.img` `a3236acc…817a83`（基线），`vendor_boot.img`
`923c4409e56b6e8053a1a0ada10114e48dedc7b99f92574bb1b5822d8cc3116b`，
`vbmeta.img` `ae4c9d6bf21b60f1cf5b38de8d26bc257c874b2098a4926f61fd0157a0a9988e`。
交付包 `out/live-checkpoints-44785665693-cmd-ckpt-signed.zip` 共 35,559,046
字节，SHA-256
`63b879569b8cad3bb2bd036c7e98c5bee6bfcfcaba332fec0f7da508699e7319`，11 成员。
独立复核全部通过。证据见
`docs/evidence/nufs-cmd-ckpt-signed-44785665693.json`。

尚未刷写或操作设备。`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。

## SCSI 命令路径逐级探针镜像已签出 · 2026-11-21

实机页脚 `ufswake` 确认镜像生效且执行进入 `host wake-begin`。此前 poll 探针每
200000 次才打印，约 0.4 秒而超时 500 ms，可能整段错过，且无法判断
`hostSubmit` 是否进入。补丁 0004 现将阈值为 1000，并在 `hostScsiDir` /
`hostSubmit` 增加 8 条无条件输出：`cmd-enter`（lun/opcode/len/dir/bytes/
timeout）、`cmd-ucd-cleared`、`cmd-ucd-refused`、`cmd-submit`、
`cmd-doorbell`（HC 0x58/0x20）、`cmd-deadline`（mach_absolute_time 与
deadline）、`cmd-ring-done`、`cmd-return`（end/ocs/polls/db/is）。返回值、
寄存器写、等待上界与错误路径均未改（SHA-256
`d147453fd1c4c0fffe17242e79efcdb290afba16a59e3f220d5341e05362fa67`）。

macOS `ufs-integrated` run [44777314987](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44777314987)
在设备提交 `3f6ab5294c3c8b4a7eabfbf926c48b10199748d8` 成功。产物
`14651980205` 共 39,203,199 字节，SHA-256
`479c6d3e22552abec16b7171eb1ee4edd9295a54d0ad3f05cdc4d030889ccee4`。预链接
内核 SHA-256
`914be0663ea7773f128f4a31a803756ccb292427b476ac9aea4ae4c32b580c9d`，屏显
`src-3f6ab529-ramoops`。内核扫描确认 8 条 cmd 探针、2 条 poll 探针、
`ufswake` 标签与阶段追踪全部编入。

签名三件套：`boot.img` `a3236acc…817a83`（基线），`vendor_boot.img`
`1f7eff633e074ecd1c2cc0c21b2b2a305b63306b74d3f68d5c3cd0ea2a6761dc`，
`vbmeta.img` `02c8d0fc3efa306a70a796cb25fc0503f8ae3a55ae4dc5ac17e665224088324f`。
交付包 `out/live-checkpoints-44777314987-cmd-probe-signed.zip` 共 35,558,977
字节，SHA-256
`3d2d6666c488dd217e0c925b1226f14388739230d8b92c85da2f7f5c7b03c2fd`，11 成员。
独立复核全部通过。证据见
`docs/evidence/nufs-cmd-probe-signed-44777314987.json`。

尚未刷写或操作设备。`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。

## poll 状态探针镜像已签出 · 2026-11-21

实机刷入 printf 追踪镜像后，NativeUfs 全链路可见，停在
`TB710FU SourceStorage: ufs-start-begin` 之后的 `NativeUfs: host wake-begin`，
无 `wake-done`、无 `wake-failed`、无 panic。已通过项：`start-enter`、
`start-args probe=0 root=1`、`gcc-state gdsc=f022f800 axi=00000001
ahb=00000003` → `gcc-state-accepted`、controller-map、`dma-ready
phys=8ca6e000 contig=139264`、smmu-map、queue snapshot/stop/start、
`nop-done`（传输层已通）、`power-query-done power=00000033`。

`pollUfsTransfer` 是纯忙等，仅在完成、fatal 或时钟到期时退出；若控制器不抬
doorbell 或 deadline 永不到期，它不打印任何内容。补丁 0004 因此增加两处输出：
每 200000 次轮询打印 `polls/db/is/ocs/expired`，以及返回 Timeout 前打印
`poll-timeout`（SHA-256
`86fda82896c4f27187ae2f8c5f0b34440871cb370557af3fe2824dc06acb923f`）。循环退出
条件、寄存器读取与 cache 操作未改。

macOS `ufs-integrated` run [44766487477](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44766487477)
在设备提交 `f1572109757449d5522e87a20e393f812fa3b72b` 成功。产物
`14644668818` 共 39,197,776 字节，SHA-256
`5993eba6c5de5927c4b456c9ea7b7c6812dbca2ed7aab5c9ed80dd80a3426201`。预链接
内核 SHA-256
`5405986b9c1dc88023d08aa1a2d65203cd336f5767033007a4eb68b3f202677f`，屏显
`src-f1572109-ramoops`。内核字符串扫描确认 2 条 poll 探针、20 个
`ufsCheckpoint` 标签、57 条阶段追踪全部编入，且 0004 引入的多余四次控制器
寄存器读与重复 `OSDynamicCast` 已被 0005 移除。

签名三件套：`boot.img` `a3236acc…817a83`（基线），`vendor_boot.img`
`ced652095f0d722fadf80cb342157fd3df450f8df0e64a195b43c94981d8e6b9`，
`vbmeta.img` `3fe2fcba7749f28b6ca7b8f95a0b9cf4afbd2ab6456ca165999757f920c5c233`。
交付包 `out/live-checkpoints-44766487477-poll-trace-signed.zip` 共 35,559,043
字节，SHA-256
`c5579eb6de18d8ff5a1713123150bc5a26eeb1c28bfc2e3d6c80d74b0fe678b3`，11 成员。
独立复核全部通过。证据见
`docs/evidence/nufs-poll-trace-signed-44766487477.json`。

尚未刷写或操作设备。`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。

## NativeUfs 同步检查点镜像已签名 · 2026-11-20

按用户授权，对 run `44755999777`、设备提交
`0c89f66495bf5988c09b79098c859b4f82543e5b` 的诊断镜像使用现有 AVB
`SHA256_RSA4096` 基线签名。屏显标识为 `src-0c89f664-ramoops`。

签名包 `out/live-checkpoints-44755999777-ufs-sync-signed.zip` 共 8 个成员，
35,630,503 字节，SHA-256
`33017b307c87e84d4ad58636e7b3e794f7c9b64bc0779ec011e559dfc686a00f`。

| 镜像 | SHA-256 |
| --- | --- |
| `boot.img`（沿用已验证的 loader） | `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83` |
| `vendor_boot.img` | `dfe1fe75d56321c78af8e44d07fb2b20bb1d3417a9fa0875323cbad5cb3b0f3c` |
| `vbmeta.img` | `657b7692f60fb3481d9c1228e1d6b79845a58e103418be5221fdc5692202fa62` |

独立回读通过 ZIP CRC、SHA256SUMS、AVB 签名、boot 信任链、两处 vendor_boot
带盐哈希及另外 22 个描述符的逐字节比较。FAT/LZMA 回读匹配本次内核和 ADT，
20 个同步页脚标签均存在，production DarwinBoot parser 通过。
实机启动及根挂载待验证。`device_operations=false`、
`hardware_boot_verified=false`、`runtime_qualified=false`。

## NativeUfs 入口和接管边界同步屏显 · 2026-11-20

本轮照片 SHA-256
`5e3e8dbb6a3d4bc276db97b3ff2d82fbdbc2e60b2550780eb2caa8ee26927ca8`。
截图走完 `ufs-init` / `ufs-attach`，最后一行 `ufs-start-begin`，页脚
`CKPT=TEXT LOG=999423 QR=996385 W=1 N=18 Q=1 B=0/1 R=1 ERR=0 ss-ufs-s`。
照片内没有可核对的镜像标识。此页脚来自调用 `start()` 前的同步刷新。
`NativeUfs` 的 `printf` 只写入环形缓冲区，屏显还要等后台刷新或下一次显式检查点。
因此这张照片无法定位到 `start()` 内部，也无法判断是否已进入该函数。

静态复核 run `44748388382` 的 strict 预链接内核
（SHA-256 `a76508900eeebe2308f9a2245f840dd4d58626f54985920fc19acf53f00522df`）。
调用点 `0xfffffff00c26b2e0` 与 vtable 槽位 `0x5f0`、
PAC discriminator `0x3c68` 和 chained fixup 一致，指向
`NativeUfs::start` 的 `0xfffffff00c263dfc`。调用者与入口日志均调用同一个
`_printf`。保留现有虚调用路径。

补丁 `0005-synchronous-bringup-checkpoints.patch`（SHA-256
`8f2d333f3a12237161430afaaffc0491b8c690798cc489f02b0d233ca71cdd6a`）
加入 20 个同步文本检查点。
`ufsentry` 在入口第一次打印前刷新页脚，随后覆盖参数门槛、GCC、DMA、
SMMU、队列接管、UFS 命令、根布局与发布准备。移除了诊断日志额外执行的
一次 provider cast 及四次控制器 MMIO 读取。主机补丁序列及调用比对、
两组 UFS 测试和 19 组 ASan/UBSan LiveXnu 测试通过。实机停点和根挂载待验证。

macOS `ufs-integrated` run
[44755999777](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44755999777)
在设备提交 `0c89f66495bf5988c09b79098c859b4f82543e5b` 完成。原生 arm64e、
严格预链接及 production DarwinBoot parser 均通过。产物 `14641271615` 共
39,197,010 字节，SHA-256
`47b33c86a7100aa9be977d521b83e37dee13eb19454e26094529fea63beb64d4`，
与 GitHub 摘要一致，ZIP CRC 回读通过。严格预链接内核 142,835,712 字节，
SHA-256 `292f1e2d798d717937e3982ca0545ffdce59514e2902564e53248257d7d6d668`，
屏显构建标识 `src-0c89f664-ramoops`，`mapping_complete=true`，
`missing={}`、`stubbed={}`。

二进制复核确认 `0xfffffff00c26b4e4` 的 PAC 虚调用仍指向
`NativeUfs::start`（`0xfffffff00c263dfc`），weak import 已绑定 LiveLog。
`0xfffffff00c263e30` 调用同步文本刷新并传入 `ufsentry`，
随后才在 `0xfffffff00c263e40` 调用 `printf`。

standard-bands 派生内核 SHA-256
`9fe6de296a61653b154538c4d786c8ef2fc40554b2b40613c81b7f6f3aef4f37`。
未签名产物 `out/live-checkpoints-44755999777-ufs-sync-unsigned/vendor_boot.unsigned.img`
共 37,859,328 字节，SHA-256
`1ee56a0c5480be7f6ba8f9b03b63812cafeddac79974afdf5752a69ebf108344`。
打包后 production parser、FAT/LZMA 回读、ADT 哈希及 20 个页脚标签均通过。
`signing_performed=false`、`device_operations=false`、
`hardware_boot_verified=false`、`runtime_qualified=false`。

## 追踪改用 printf，避开 IOLog 在该上下文的限制 · 2026-11-20

实机刷入 run `44742777160` 后仍无任何 `NativeUfs:` 行且无 panic，但
SourceStorage 的 `printf` 行（`ss-`）正常显示。核对 XNU 源码：`IOLog` 经
`_IOLogv`，其 `__doprnt` 受 `disable_iolog_serial_output` 保护，且函数末尾
`assertf(interrupts_enabled || ...)`；`printf` 走 `vprintf_internal`，
无条件调用 `__doprnt`。两者最终都到 `console_write`，但该函数在
`!console_io_allowed()` 时静默返回。既然 `printf` 在同一上下文中已被证明
可达 live-log 环，48 条追踪改为 `printf`（补丁 SHA-256
`b4383ebf9f01a339d2bd3c59019ea749755465bccc1055341a4a8ee0e60a057d`）。

9 个既有 `IOLog` 与 8 个既有 `snprintf` 未改动；删除追踪语句后仍与
post-0002 源逐字符相同。验证器现同时识别 `IOLog`/`printf` 两种写法，并
断言 7 个 `start()` 探针存在。

macOS `ufs-integrated` run [44748388382](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44748388382)
在设备提交 `2cf9f380df150a23280df4a6ffca199a44d6d3ee` 成功。产物
`14634996232` 共 39,193,573 字节，SHA-256
`995a4b01a6848b181bf60c94f4c933fcbd7687fc2b5a58abe2944503f507910e`。预链接
内核 SHA-256
`a76508900eeebe2308f9a2245f840dd4d58626f54985920fc19acf53f00522df`，屏显
`src-2cf9f380-ramoops`。签名三件套：`boot.img` `a3236acc…817a83`（基线），
`vendor_boot.img` `8c0007452a9bf4719b4accbdd1313a0cf6f95c7818d62bfafbf0be435dcefc61`，
`vbmeta.img` `e79eb00a7fe8ede1a0382daf41ae07ad0e8a5376cd113ab00dabd837d8435230`。
交付包 `out/live-checkpoints-44748388382-printf-trace-signed.zip` 共
35,558,980 字节，SHA-256
`acd065ab8e7c1ef3420ad038a8dc9d0428852ad1d60a784bac2c296ba233c571`，11 成员。
独立复核全部通过。证据见
`docs/evidence/nufs-printf-trace-signed-44748388382.json`。

尚未刷写或操作设备。`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。

## NativeUfs::start 追踪镜像已签出 · 2026-11-20

实机刷入 run `44732960980` 后，屏显在 `TB710FU SourceStorage: ufs-start-begin`
之后没有任何 `NativeUfs:` 行、也没有 panic。上一版 0004 只覆盖了
`hostBringUp()` 和 `startRoot()`，`NativeUfs::start()` 入口到 `startRoot()`
调用之间没有探针。补丁 0004 因此扩展 7 个检查点（SHA-256
`d1e72c5f5ee680589929b868f16cc3c2feb4182b7ce0a079e407db1d2bc000c5`）：
`start-enter provider=%p`、`start-args probe/root/cast`、
`start-gate-return`、`start-options-ok`、`start-diag`、
`start-dispatch-root`、`start-fallthrough`。其中 `cast` 直接打印
`OSDynamicCast(IOPlatformDevice)` 结果——该 cast 失败会让 `start()` 在
2219 行静默返回。

macOS `ufs-integrated` run [44742777160](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44742777160)
在设备提交 `e8d08b67e84f694a033756670c61431ab24e939c` 成功。产物
`14631828891` 共 39,194,200 字节，SHA-256
`fbdd0be3d36242361158dad495d6a0bf387b57a52de9836b4c525d713456116c`。strict
预链接内核 SHA-256
`5dbc7cba096d474d2ba9823cbb6be10706e5e687dc76941cfba343ae0c67fb74`，屏显
`src-e8d08b67-ramoops`，mapping complete、parser PASS。内核字符串扫描确认
7 条 `start-*` 与之前的 45 条 `host`/`root` 追踪全部编入。

签名三件套：`boot.img` `a3236acc…817a83`（loader 基线不变），
`vendor_boot.img` `dd0b27f73eef961d2616fdae424b6cabb654e52e9d3c98b372d07a58d6b49f38`，
`vbmeta.img` `4e4b8bb534f66643733c48923abce4dca6a0fcb7eb2d268b5d159e0d9da19225`。
交付包 `out/live-checkpoints-44742777160-start-trace-signed.zip` 共
35,558,845 字节，SHA-256
`352e1a07d8cb93ce0cee57df6f830fcc3d144d187bba5ba397c16dfc7f9a897c`，11 成员。
独立复核重算全部镜像与成员哈希、ZIP CRC、SHA256SUMS、AVB RSA-4096、
22 个未变 descriptor、公钥、payload LZMA 回读、ADT 与 parser 均 PASS；
14 个关键标记在打包后内核中确认存在。证据见
`docs/evidence/nufs-start-trace-signed-44742777160.json`。

尚未刷写或操作设备。`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。刷机后
`ufs-start-begin` 之下应出现 `NativeUfs: start-enter`；若其后的
`start-args` 显示 `cast=0`，即为静默返回点。

## NativeUfs 阶段追踪镜像已签出 · 2026-11-20

macOS `ufs-integrated` run [44732960980](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44732960980)
在设备提交 `e6d56ca9de64335a5a419fafadda0dbe5f980df7` 成功（原生 arm64e、
strict prelink、production DarwinBoot parser）。产物 `14627517335` 共
39,192,652 字节，SHA-256
`63ac7d68a3fe7c8a4e40a3212a459fed203255fb752f9e3eca4ed8c836317ed9`，与 GitHub
摘要一致。strict 预链接内核 SHA-256
`4953c8e52ddf155b3a44552e922caf8383570aaf451e64d1c12e0f34147d5872`，屏显标识
`src-e6d56ca9-ramoops`，`mapping_complete=true`、`missing={}`、`stubbed={}`。
对内核的字符串扫描确认 0004 的 26 条 `NativeUfs: host/root` 追踪串全部编入，
且上一版 21 个 `ss-` source-storage 检查点仍在。

签名三件套：`boot.img` SHA-256
`a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`（loader 基线
不变），`vendor_boot.img` SHA-256
`e0fd7853aea197bc09fdcdcb79e785f7edc1c6ce42a92448c369a1cf20b3f3ec`，`vbmeta.img`
SHA-256 `a81da9a415059e577eeccb1cbc9118c45d03372b2a556628720b0486e1ef1ee5`。
交付包 `out/live-checkpoints-44732960980-nufs-trace-signed.zip` 共 35,559,461 字节，
SHA-256 `29616ca8674616028ccf1f944a0378958e42f20bc628da467f8161ac50f4b8c1`，
11 个成员。独立复核从磁盘重算全部镜像与成员哈希、ZIP CRC、SHA256SUMS、
AVB RSA-4096 签名、22 个未变 descriptor、公钥不变、payload LZMA 回读、ADT
`f9b955ba…0dca3b` 与 parser 均 PASS；15 个关键追踪串在打包后的内核中确认存在。
证据见 `docs/evidence/nufs-bringup-trace-signed-44732960980.json`（bundle 哈希记在
包外，避免自引用）。

尚未刷写或操作设备。`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。刷机后
`ufs-start-begin` 之下会新增 `NativeUfs: root bring-up-begin` 起的 26 条阶段行；
`host gcc-state` 直接打印 `gdsc/axi/ahb` 三个寄存器值，可判定固件是否已开启
UFS 时钟与电源。

## 实机确认 UFS 接管停点，补上 NativeUfs 阶段追踪 · 2026-11-20

新刷入 `44713622044` 镜像（`src-a89bac58-ramoops`）后，实机屏显按序走完
`source-storage-start begin` 的全部 20 个子步骤：`gate-passed`、
`kext-ioStorage`、`base-class-lookup`、`root-policy`（RootPolicies 认证通过）、
`kext-apfs`（打印 `apfs-3288.60.7`）、`premodload`、`class-reg-base/ufs`、
`postmodload`、`wait-platform`、`ufs-create`、`ufs-init`、`ufs-attach`，
最后停在 `ufs-start-begin`，无 `ufs-start-done`、无 panic、页脚 `ERR=0`。
页脚 `LOG=1000133 QR=997015 ... ss-ufs-s`。执行已进入
`NativeUfs::startRoot()`，即 `hostBringUp()` 或其内部调用阻塞。

照片中 `AppleARMBootPerf` / `AppleARMSFRManifest` 那串 Error 仍出现，但执行
继续走完 `iokit-time-init done` 及后续全部阶段，因此不是停点；它是非 Apple
固件缺少 `/chosen/secure-boot-hashes`、`boot-manifest-hash`、`/chosen/iBoot`
的固有噪声，与镜像版本无关。

`hostBringUp()` 已有 `gRootDiagnostic.stage` 字段，但仅在
`native-ufs-diagnostics=1` 时生效，实机未传该参数。新增补丁
`patches/ufs/0004-bringup-stage-trace.patch`（SHA-256
`8b1f9afd3125ff79abdfb61332e30b8c9a0803ae383b168785e3d226e81c2942`）
在各阻塞操作前无条件输出 `IOLog`：GCC 时钟/电源状态、控制器映射、DMA 分配与
连续性、SMMU 映射、queue snapshot/stop/start、NOP OUT、电源查询、设备唤醒、
LUN 就绪、root layout 读取，共 45 条 begin/done/failed 标记。

该补丁基于 `0001+0002` 之后的源码生成（`0002` 增加了 GCC 电源检查与
事务式队列接管）。验证：完整序列 `0001→0002→0004→0003` 在 pinned 源上
干净应用；删除全部 IOLog 后与 post-0002 源逐字符相同；`write32`、`IOSleep`
常量、`hostStopQueue`/`hostStartQueue`/`hostStopTakenQueue`/`restoreUfsSmmu`/
`smmuBypassStream`/`hostNop`/`hostQueryRead`/`hostScsi` 调用数、`native_ufs_root_step`
取值、`panic` 站点及全部 `*why` 错误串均未改变。

尚未构建、签名或刷写这一版。`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。

## 实机越过全部 BSD 检查点，进入 UFS 接管 · 2026-11-20

新照片上传预览 SHA-256 `ab83777dad20c574d3909a6644f5b114427006dd568ace0b28c950287eaf1475`，
本地原片 `C:/Users/baiti/Downloads/IMG20261120171040.jpg` SHA-256
`ac71895bfe2f2cdbf2147e740fe8080ea7640c80810d3e1be887a43d7071e035`
（14,155,405 字节，3072x4096；与上传预览差异来自传输重编码，屏幕文本为权威证据）。
照片来自签名包 `44703596202` / 设备 `da21aa25` / `src-da21aa25-ramoops`。

屏显按序出现 0032 的全部文本检查点：`bsd-init entered`、`throttle-init
begin/done`、`kauth-init begin/done`、`bsd-bufferinit begin/done`、`iokit-time-init
begin/done`、`cdc-start begin/done`、`vfsinit begin/done`，最后一行是
`TB710FU bsd: source-storage-start begin`，没有对应的 `done`。页脚
`CPT-TEXT LOG=1000983 QR=999984 ... ERR=0 source-s`（`source-s` 为
`source-storage-start` 的截断）。这是新的最前沿：执行已进入
`native_source_storage_start()`，且无可见 panic、`ERR=0`。

`iokit-time-init` 窗口内的 `AppleARMBootPerf` / `AppleARMSFRManifest` 一串
Error（缺 `/chosen/secure-boot-hashes`、`boot-manifest-hash`、`/chosen/iBoot`）
不是当前停点：其后已出现 `iokit-time-init done` 及 cdc/vfs/source-storage 标记。
它们反映非 Apple 固件缺少相应 DT/NVRAM manifest 属性，执行继续。

由于 `native_source_storage_start()` 内部尚无子步骤标记，当前照片无法区分是
RootPolicies handoff、`waitForMatchingService(IOPlatformExpertDevice, 5s)`、
类注册、`native_ufs_source_create()`、UFS `init/attach/start` 的哪一步停住。
这与文档预测的根挂载阻断项一致：`docs/UFS_PORT.md` 记录实机 UFS 接管、SID
`0x60` SMMU、已知块读及根挂载均未验证。

已实施下一版诊断改动：`package/SourceStorage.cpp` 的
`native_source_storage_start()` 在 28 个 UFS 接管子步骤写入文本行并发布同步
live-log 检查点（boot-arg/atomic gate、IOStorageFamily 加载、base class 查
找、RootPolicies handoff、APFS 加载、deferred class 注册、platform wait、UFS
create/init/attach/start）。provider 为 weak 条件引用，独立归档构建不受影响；
不增加任何 kext/policy/UFS 调用，不改变 panic 路径，`init/attach/start` 短路
三元式展开已对全部 8 种返回组合验证等价。本地门槛通过：clang++ 19
`-fsyntax-only`、控制流等价模型、结构/verifier gate。见
`package/Tb710fuSourceStorage/README.md` 与
`.work/source-storage-step-equivalence.json`、
`.work/source-storage-step-structure.json`。

本地改动已提交 `a89bac585f2532eca28193aa93ba704f2f9c14e3` 并推送到
`origin/codex/ufs-transactional-handoff`；已派发 macOS `ufs-integrated` run
[44713622044](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44713622044)
（stage=ufs-integrated，commit `a89bac5`）已 success。该 stage 重新编译
`SourceStorage.cpp`、链路、strict prelink 并跑 production DarwinBoot parser。
strict 预链接内核 142,835,712 字节，SHA-256
`5ec733691da609c66c0e4eef901d273d0d05d915d6c5391c8d01646edb34617d`，与
build-result 一致；USB CDC / LiveLog / source-storage 三个集成报告哈希均与
build-result 相符，production DarwinBoot parser PASS。产物 ZIP
`14619136678` 共 39,188,735 字节，SHA-256
`ba4a2a537cd3b0a6af1dae86f0d1dcb0778558d2b24241956628862f79a36eee`。
source-storage 集成报告确认 `device_commit=a89bac5`、archive 已重编
（SHA-256 `bcd7f46aac5e445d4d4d32e6d2dc119781e663557e737fb57f8164aea4768832`）、
startup hook 选中、archive force_load 恰好一次。对内核 Mach-O 的 ASCII 扫描确认
21 个 `ss-` 检查点标签、`_tb710fu_live_log_checkpoint_text` 符号和
`TB710FU SourceStorage: ` 文本前缀均已编入。详见
`docs/evidence/source-storage-step-checkpoints-build-44713622044.json`。

### 签名可刷写测试镜像 · 2026-11-20

按用户要求先完成 host 端实测再签名。verify 脚本核验 run 摘要、kernel 哈希、
mapping complete、21 个新 `ss-` marker、production + local parser 与 live-log
package（6 objects/27 strong defs），全部 PASS。standard bands 变换 PASS
（6 spans/3 probes，其余字节不变），unsigned vendor_boot 37,859,328 B，
SHA-256 `05d29bd48d7500cd06085710cf3fb90c6c19d0aa6000b4de9031455ddb4b2320`，
production parser PASS。随后用本地 AVB RSA-4096 基线 key 签名，独立核验
RSA 签名、salted hash、boot chain key、22 个未变 descriptor、FAT/LZMA 回读
及 standard 变换后 21 个检查点 marker 全部存在，均 PASS。

签名三件套：`boot.img` SHA-256
`a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`（沿用已验证
loader 基线），`vendor_boot.img` SHA-256
`1070c9e41e36de8a8da548f430a934b9a1a51b14658d54158033144fde27d6f6`，
`vbmeta.img` SHA-256 `491f281e9ea198081da132d79a02bbb80fe9b196082ed63e1738764ed6c609ef`。
交付包 `out/live-checkpoints-44713622044-ss-steps-signed.zip` 共 35,561,830 字节，
SHA-256 `6fb74e7d125ec4e4a2c5672876121a1ba8f034c2c53c8a43376ad058f825ac03`，
13 个成员，ZIP CRC 与 SHA256SUMS 回读通过。详见
`docs/evidence/ufs-step-checkpoints-signed-44713622044.json`。

尚未刷写或操作设备。`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。刷机后下一张实机照片应显示
`TB710FU bsd: source-storage-start begin` 之后最后一条
`TB710FU SourceStorage: ss-<step>`，据此定位 UFS 接管的具体停点；在定位前
不改动固件/DMA 逻辑。原始照片证据见
`docs/evidence/live-checkpoints-source-storage-begin-device-IMG20261120171040.json`。

## BSD 初始化同步文本检查点 · 2026-11-20

补丁 `0032-tb710fu-bsd-text-checkpoints.patch` SHA-256
`f1546f4ffcb9f57e0626b9ae23bc8c9a2ae5ee3f7915d81e47e9a0cc29c6ab6c`。从 run
`44696022526` 记录的固定源码和补丁序列重建后，正向应用和反向恢复通过。新增 21 处
文本检查点，覆盖 `throttle_init`、kauth、BSD buffer、IOKit 时间、CDC、VFS、source-storage、
BSD autoconf、根设备匹配及 `vfs_mountroot` 的返回值。文字在调用边界同步刷新，发送由现有
QR 调度独立处理。19 组 ASan/UBSan LiveXnu 主机测试及 14 项打包验证通过。

固定框架的通用 `XNU_NATIVE_AES` 配方要求 prelinked 加密引导。TB710FU 使用 source 模式，
以保留 OSKext 所需的 `prng_random`。Sheng 同版 `26C5035r` 参考另有晚期 AES 配方，验证
IOAESAccelerator 虚表、PAC discriminator 和类布局，注册点在 `IOKitBSDInit()` 返回之后。
AppleKeyStoreState 还需固定设备身份和已核实的 macOS selector。两条适配都需各自验证。

在本地 `OS.raw` 和 `userdata-root-vroot.img` 的 OS 窗口分别校验 APFS 的 NXSB、对象映射和
APSB，读到同一卷 UUID `84581E99-85BE-4957-AC53-2947E887B3E4`，与 RootPolicies 和签名
镜像 ADT 的 `boot-uuid`、`root-matching` 一致。实机 UFS 接管与根挂载待验证。证据见
`docs/evidence/userdata-apfs-root-uuid-43724852936.json`。

macOS `ufs-integrated` run [44703596202](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44703596202)
通过 arm64e 原生构建、严格预链接和 production DarwinBoot parser。设备提交
`da21aa254f90cb0171de78189676241121d98a64`，屏显 `src-da21aa25-ramoops`。下载产物
`14614766127` 的 SHA-256 `6199024176d34329994748343407342ad99c1e669deb3daa5cb9520fb3dd5733`
与 GitHub 摘要一致。严格预链接内核 SHA-256 `14006760161fe018c98bd667a56e61096e28d3042b7e5d6bf26ec1f096c8ca08`。
打包校验 21 项输入及 ADT SHA-256 `f9b955bad7f9d6b3831d95cb70aa03cd44205c8e30f647e5e5e4f0a1708dca3b`。
签名 ZIP `out/live-checkpoints-44703596202-bsd-text-signed.zip` 共 35,561,642 字节，SHA-256
`5ef56c8cdbae886c9dafdd50ef9052fec1dd2e8ea68bd55f5b9c0cdf7309d5b1`。AVB RSA-4096、
内核与 ADT 回读和 13 个 ZIP 成员验证通过，详见
`docs/evidence/live-checkpoints-bsd-text-signed-44703596202.json`。待实机回传最后一条
`TB710FU bsd:` 文本。`hardware_boot_verified=false`、`runtime_qualified=false`、
`device_operations=false`。

## 已越过 CPU 拓扑等待，进入 BSD 初始化 · 2026-11-20

新照片原文件 SHA-256 `c8ff7fdd4675f5ba38bde09254f9c15572a640458b72df980a532a415513b45d`。
屏显先后出现 `kperf-topology-wait begin/done`、`kperf-init-early done` 和
`bsd-init begin`，确认启动 CPU 拓扑等待已返回。页脚 `LOG=996965 QR=996640 W=1
N=18 Q=1 B=0/1 R=1 ERR=0 bsd-init` 表明 worker 已启动。尚未看到 `bsd-init done`；本照片
没有显示构建标识或 XNU 异常寄存器，不能断言停在哪个 BSD 子调用。照片未提供设备分区回读。

当前签名包 `44696022526` 的组件为 ArmDTPlatform、ArmCpu、XnuPrelink 和 DarwinBoot，
具备本次观察所需的 GIC `/arm-io/intc` 与单核 timer/CPU bring-up；`smp=1` 未启用，
ADT 尚无 E/P cluster-type。macOS 驱动指南还要求评估 `SoftwareAES`（AppleImage4 等待
`IOAESAccelerator`）与 `AppleKeyStoreState`。两者不在当前组件或 archive 清单内，属于应补齐
审查的 macOS 适配项，可能影响文件系统/密封系统卷与后续用户空间，不足以单凭当前 `bsd-init`
入口判定为即时停机原因。BSD 内部第一个 `throttle_init()` 和后续 `vfsinit()`、root 挂载均无
分阶段 marker；需取得更细粒度日志，才可区分早期 BSD 初始化与存储/root 阶段。

根卷前置条件另有明确缺口。`docs/USERDATA_BUILD.md` 记录 userdata-root 镜像尚未写入设备，
签名包中的 `kernel.macho.vroot` 提供 1,712 字节 RootPolicies 数据。`docs/UFS_PORT.md`
记录 NativeUfs 仍假设固件保留运行中的 UFS controller/link，实机接管、SID `0x60` SMMU、
已知块读及根挂载都未验证。因此根挂载是后续明确的阻断项，但当前照片只停在 BSD 入口，尚未
证明执行已到 UFS 或 root mount。macOS 驱动指南还列出缺失的 `SoftwareAES` 与
`AppleKeyStoreState` 适配；前者关联 AppleImage4 等待 `IOAESAccelerator`，其作用阶段与
触发条件需要后续日志确认。固定 XNU 的 `bsd_init()` 入口后的首个调用为 `throttle_init()`；
其后 `IOKitInitializeTime()` 对 `IORTC` 最多等待 30 秒并会继续返回。下一张只需关注文本中
新增的 `throttle_init begin/done` 及后续 IOKit 时间初始化边界，无需调整二维码路径。

参考：[驱动适配清单](https://unixnu.com/zh/porting/drivers)、
[macOS 驱动要求](https://unixnu.com/zh/porting/macos#drivers)、
[平台与内存/OS 服务](https://unixnu.com/zh/porting/platform#block-and-os-services)。

## 实机越过 machine_lockdown，到达 CPU 拓扑等待附近 · 2026-11-20

新照片原文件 SHA-256 `01de2c9744c483ba5f4ef352e19cc4d152e63727b395365365b738c04eeef98f`。
滚屏已越过 PRELINKTEXT RNX、`machine-lockdown done`、`iokit-lockdown done` 和
`vm-set-restrictions done`，最后一行 `kperf-init-early begin`。页脚 `W=0 N=16 Q=2
B=0/1 R=2 ERR=0 LOG=996516`，构建标识和二维码本身在照片范围外。设备分区未回读。

固定 XNU 的 `kperf_init_early()` 首先调用 `ml_wait_max_cpus()`，等待 `ml_set_max_cpus()`
发布启动 CPU 拓扑。此配置由 ArmCpu 的 `IOCPUInterruptController::initCPUInterruptController()`
完成发布。上一版签名镜像内的 ADT `/arm-io` 无子节点，ArmCpu 的 `mapGic()` 需要其中的
`intc` 才能进入后续初始化。实机是否停在等待内部仍需新检查点确认。

LiveLog 新增优先检查点。工作线程开始前，关键启动点立即发布可独立扫码的最新单帧。
页脚分别记录滚屏 `LOG=` 和最近成功绘制的二维码 `QR=` 末尾偏移。`0031` 在 kperf、
拓扑等待及 BSD 入口调用优先检查点。常规检查点保持节流。18 组 ASan/UBSan 主机测试、
14 项打包验证和 4 张实际 Mu 编码帧的 ZXing/OneSend 回读通过。优先帧在时钟未推进时
独立解出 652 字节，包含最新的 `kperf-init-early begin`。

本地打包可从经 SHA-256 核对的实机 FDT `bba7ae3636a43ac4c3506327212ed8378a9f043b092c45883d592124c4bc557e`
投影 `/soc/interrupt-controller@17100000`。固定 UniXNU 生成器在 ADT 写入 `/arm-io/intc` 和
对应总线范围。GICD 为 `0x17100000/0x10000`，GICR 为 `0x17180000/0x200000`，步长 `0x40000`。
两段均落在设备 UEFI 的 APSS_HM MMIO 窗口，保留 RootPolicies 和现有 CPU 节点。16 项
source-storage 打包测试通过，2 项缺少可选夹具而跳过。GIC 初始化与 CPU 发布待实机验证。

macOS `ufs-integrated` run [44696022526](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44696022526)
通过原生 arm64e 构建、严格预链接与 production DarwinBoot parser，设备提交
`7f772a8d9272554c1f2520350be9524ed6de0586`，屏显 `src-7f772a8d-ramoops`。GitHub artifact
`14611735452` 的 SHA-256 `4c83ce20b882866be8cd3c6034d17e1368422859ce0196cce95b23e79666928c`
与下载一致。严格预链接内核 SHA-256 `c98006c2bcdb05652dd839b2c72490a5831e618f91008deb77a9cb561e2ca5ad`。
标准色带变体及 21 项打包输入验证通过。签名镜像 FAT 回读的 ADT SHA-256
`f9b955bad7f9d6b3831d95cb70aa03cd44205c8e30f647e5e5e4f0a1708dca3b`。

`out/live-checkpoints-44696022526-kperf-gic-signed.zip` 共 35,561,810 字节，SHA-256
`0fcbfbec0abc013db398016021c74ea613019f8785f03ad47f86cc9a8f44660b`。AVB RSA-4096 签名、
FAT/LZMA 内核回读和 13 个 ZIP 成员的 CRC 与 SHA256SUMS 均通过。验证报告
`docs/evidence/live-checkpoints-kperf-gic-signed-44696022526.json`。实机待确认
`kperf-topology-wait done` 和 `bsd-init begin`，`hardware_boot_verified=false`、
`runtime_qualified=false`、`device_operations=false`。

## 重叠映射修复版签名测试镜像 · 2026-11-20

macOS `ufs-integrated` run [44690443482](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44690443482)
通过原生 arm64e 构建、严格预链接及 production DarwinBoot parser。设备提交
`c093ece95bbd66868cb5a2c7f55fb3d9d608a638`，屏显 `src-c093ece9-ramoops`。GitHub artifact
`14609315374` 下载摘要一致，`0030` 的编译源码与本地隔离夹具一致。严格预链接内核 SHA-256
`d74db5252dc26fe77719d89ff05d0d229b87490f27ed569d5fa00d0dd2ad60a8`，标准色带变体 SHA-256
`0ec12e43e14fcec423230caeb5f48d89caff80055aa05e1dc8863fd90b710c99`。3 个探针、6 处修改及
其余内核字节和 segments 已逐项回读验证。

签名三件套位于 `out/live-checkpoints-44690443482-prelink-overlap-signed/`。同名 ZIP
共 35,571,937 字节，SHA-256 `4faaf759891001743799a647ec31223b184c57e88cd29dd2afed5ebb69ac6582`。

- `boot.img` `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`，沿用已验证 loader 基线。
- `vendor_boot.img` `4fa37239074d9ac5d73012a1d38053d6f6675e90c39242c6de39c5b9325507cb`。
- `vbmeta.img` `4a0cdc457574990e8e9b22b5c74cf6b661ef52d1fc26eaae08d353286099baa2`。

20 项打包输入、AVB RSA-4096 签名、vendor salted hash、boot chain key、FAT/LZMA 内核回读、
DarwinBoot parser 及 ZIP 13 个成员的 CRC 和 SHA256SUMS 均已验证。启动后待观察
`ld-prelink-rnx-head-done`。`runtime_qualified=false`、`hardware_boot_verified=false`、
`device_operations=false`。详见 `docs/evidence/prelink-overlap-no-hint-build-44690443482.json`
和 `docs/evidence/live-checkpoints-prelink-overlap-signed-44690443482.json`。

## 实机越过 400 次 PTE 调用，修复重叠映射的 hint 冲突 · 2026-11-20

用户实机扫出 `src-2bd0cc05-ramoops` 的单块日志，确认早期二维码可独立还原日志。
该帧在 `ld-prelink-rnx-edge-existing-table` 结束，范围 `[991325,991510)` 共 185 字节，
`dropped=0`。屏幕更新至 `LOG=994145`，最后可见 `completed=400 next-i=412
next-va=0xfffffff00719c000`。计数表示 `update_or_defer_tte()` 返回次数。照片原文件
SHA-256 `2657abbf24161448f73708f1a2b2dae665a3163134341b2ab13811c791f9f126`。

该 VA 是签名测试内核的 native `__TEXT` 起点。对照 Mach-O 得到 native `__TEXT`
`[0xfffffff00719c000,0xfffffff007340000)`，整个区间落入外层 `__PRELINK_TEXT`
`[0xfffffff00700c000,0xfffffff007dc4000)`。建表时 native `__TEXT` 允许 64 KiB contiguous
hint，首个完整组为 `i=416..431`，VA `0xfffffff0071a0000` 起。随后活动页表上的
PRELINKTEXT RNX 禁用 hint。复核已签名内核的 `tbz #52` 检查和失败分支定位到
`brk #0xbffc` @ `0xfffffff00c2b3eac`。`i=416` 落在最后可见的下一项 412 与下一条预期
检查点所报的下一项 428 之间。已有可复现的描述符属性冲突，实机异常 PC 尚待确认。

`0030-tb710fu-prelink-overlap-no-hint.patch` SHA-256
`27c7b65f52c17a3cbb49e9ba435c53d7927176f0f56470e6317611122de47876`。在页表启用前计算区间
相交关系，重叠时初始 native `__TEXT` 只允许块映射，采用原 RNX 权限和活动 PTE 断言。
互不重叠时允许原有 hint 策略。隔离的 0028 源码文件正反向应用通过，精确 preimage
`f03c10dfc3e7c860d490747973b009c2e1c37c94`、postimage `bd948b65ebc6c8b558ac8a924fcc5e0ae6802315`。
实机需要观察是否到达 `ld-prelink-rnx-head-done`。

## 早期二维码诊断镜像已签出 · 2026-11-20

按用户请求用 run [44685819017](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44685819017)
制作可刷写的本地测试镜像。内核来自设备提交 `2bd0cc0502e5c841bdfb5200d4ee44667b9d6cfd`，
显示标识 `src-2bd0cc05-ramoops`。严格预链接内核 SHA-256
`9456d0bb9f57a35f17cb47cb463792fbcaeaf39773f58a095a2d7556b9c8ae0a`，打包使用的标准色带
变体 SHA-256 `f7d7f339e7fa34def293f338a9d63eb74957814881eb3f17841c7b2bad70ad58`，
6 个修改范围、3 个早期探针和其余内核字节、segments 已逐项验证。

`out/live-checkpoints-44685819017-early-qr-signed.zip` 共 35,572,134 字节，SHA-256
`7c80b0c497e5049df6011f1d02bb1fbd83bde5f8d8b698d68f089764ddd415f8`。三件套 SHA-256 如下。

- `boot.img` `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`，保留已验证 loader 基线。
- `vendor_boot.img` `5918d744c62381b5c70ec4a81dfd4c3081ae270dacf6d1f18107643270724261`。
- `vbmeta.img` `85d1ead5d9665967e1d85246fe071ba1f73d8878cbcdeaf2e1fd54b66a922b30`。

完整 20-input manifest、AVB RSA-4096 签名、vendor salted hash、boot chain key、另外 22 个
descriptor、66,512-cluster FAT、LZMA、ADT、RootPolicies、启动参数和 production DarwinBoot
parser 验证通过。ZIP 的 13 个成员通过 CRC 和 SHA256SUMS 回读。包内 `TESTING.txt`
记录 B 槽镜像配对和 OneSend 单帧/多帧读取方式。完整验证见
`docs/evidence/live-checkpoints-early-qr-signed-44685819017.json`。
实机扫码和启动仍待验证。`runtime_qualified=false`、`hardware_boot_verified=false`、
`device_operations=false`。

## 早期二维码恢复轮播与 kalloc 滚屏减载 · 2026-11-20

实机照片原文件 SHA-256 `8c99a5118d70190b7363b08c2877e8a676cc94e16a9eef806b66c40bdaa84a3b`。
可读到 fixed `kalloc` class 3 的 `58/64` 完成、`59/64` helper 返回。构建标识、故障指令和
是否重启均无法从此图确认。此前有可核实构建到达 SYSCTL 与 `machine_lockdown`。

固定 XNU 的 ZALLOC 早于 LiveLog 的 EARLY_BOOT 工作线程注册。此阶段检查点现在每隔至少
250 ms 继续发送一份可从单帧完整解出的短日志，成功显示两帧后允许刷新到最新尾部。
线程开始执行后切换到带早期备份去重的多帧文件，保持 16 轮替换门限。线程和检查点共享
发送节流。采样文本每 250 ms 更新，立即检查点仍直接更新。文本渲染批量追加未显示的
字节，再只更新变化的字形像素，控制台重建时重绘现有文本。

`0029-tb710fu-bound-fixed-kalloc-progress.patch` SHA-256
`7149448f0aa5a440c3d0a8e28790ca58ab56268cc9105f43269cf580867060c7`。固定组在首组、每完成
16 组和末组记录进度，末组保留 skip-list/assignment 细节。隔离的固定源码通过正反向
应用检查，剔除诊断输出和守卫后 allocator 操作一致。实机扫码和原启动故障边界待验证。

macOS `ufs-integrated` run [44685819017](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44685819017)
成功，设备提交 `2bd0cc0502e5c841bdfb5200d4ee44667b9d6cfd`，显示标识
`src-2bd0cc05-ramoops`。Native arm64e SHA-256
`bf1e2b6a885334e9c24899327b10622d8ce96c28509d5f1c15aa90db7d601491`；142,835,712 字节
严格预链接内核 SHA-256 `9456d0bb9f57a35f17cb47cb463792fbcaeaf39773f58a095a2d7556b9c8ae0a`。
`mapping_complete=true`、`missing={}`、`stubbed={}`、`allow_missing=false`。生产 DarwinBoot
parser 在云端及本地回读通过，GitHub artifact 14606672384 的字节与发布摘要一致。

17 组 LiveXnu 主机测试、8 种尺寸滚屏回归、14 项打包校验通过。主机端到端测试由实际
LiveXnu 和固定 Mu 编码器生成三张帧缓冲图，ZXing 解出精确帧数据，固定 OneSend 从每张
图独立还原 652 字节日志文件。前两张是同一会话，第三张更新到较新的尾部。产物与测试
摘要见 `docs/evidence/live-checkpoints-early-qr-loop-44685819017.json`。
`runtime_qualified=false`、`hardware_boot_verified=false`、`device_operations=false`。

## PTE substep signed test images · 2026-11-20

用户要求将 run `44612581308` 产物打包为可刷写测试镜像。保留原 bootloader，使用
设备提交 `6d82bbe50780b8fb84e383c00b609db555f23432` 的预链接内核，并生成 standard-band
derivative（3 个 probe，6 个 span，其余内核字节及 segments 未变）。完整 20-input manifest
经 `tools/package_source_storage.py` 验证：UFS/source-storage、RootPolicies、CDC、LiveLog、
loader provenance、ADT、FAT 与启动参数均绑定到同一 build。签名沿用已授权的本地 AVB
RSA-4096 baseline pair，仅更新 vendor_boot descriptor；没有把密钥放入仓库或镜像包。

输出目录 `out/live-checkpoints-44612581308-pte-substeps-signed/`，bundle
`out/live-checkpoints-44612581308-pte-substeps-signed.zip`（35,652,235 字节，SHA-256
`07573785b8adc466200746d52e66463bdf7fe362ae7fbb83c5e0cae7d89b4b7a`）：

- `boot.img`：100,663,296 字节，沿用已验证 baseline，SHA-256
  `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`。
- `vendor_boot.img`：100,663,296 字节，SHA-256
  `ea146f0a8c4093f8ff7581a3e1c3b2f613561918c8337cecee4134b2dcb8e13b`。
- `vbmeta.img`：8,000 字节，SHA-256
  `e60d8177b34387ea6a381463d6b0bd061852d2a5fdb63bdb191d3119441804ca`。

RSA-4096 AVB 签名、vendor salted hash、boot chain key、未变的 22 个 descriptor、
vbmeta header/public key 和 unsigned vendor body 回读通过。FAT 为 66,512 clusters，LZMA、
kernel/ADT/RootPolicies/TOML 回读及 production DarwinBoot parser 均通过；bundle 13 个成员的
CRC 和 SHA256SUMS 回读通过。source-storage 主机测试 17 项通过（2 项历史输入测试跳过），
LiveLog package 测试 13 项通过。完整证据见
`docs/evidence/live-checkpoints-pte-substeps-signed-44612581308.json`。

这是签名的 A/B 诊断测试包，不是实机验收：未刷写、未重启，也未操作分区或设备文件系统；
`runtime_qualified=false`、`hardware_boot_verified=false`、`device_operations=false`。

## PRELINKTEXT PTE 进度与 USB 启动边界 · 2026-11-20

实机帧来自 `src-45d347f6-ramoops` / run `44546733725`。当前 `[1502250,1502502)`
共 252 字节、`dropped=0`，尾部连续记录 `ld-prelink-rnx-pte` 的索引
192、256、320、384。checkpoint 打印发生在对应 `update_or_defer_tte` 之前，因此
索引 320 的批次已完成；索引 384 是下一个待更新项，不是已确认 fault PC。首个
edge helper 的活动范围是索引 12–511，至少还有索引 384–511 共 128 项待处理，之后
还需完成 edge helper 和整个剩余 PRELINKTEXT/lockdown 路径。屏幕日志停在该标记只表明
没有绘制更新，不能单独认定内核在 PTE 384 指令上故障。证据见
`docs/evidence/live-checkpoints-prelink-rnx-pte-44546733725.json`。

新补丁 `0028-tb710fu-prelink-rnx-pte-substeps.patch` 仅在 PRELINKTEXT 活跃范围内，
于 `update_or_defer_tte()` 返回后计数；每完成 16 次调用记录 completed count 和
`next-i/next-va`。这样首批按真实更新次数对齐，且不会为未更新的页表项报进度。补丁
SHA-256 `5ddf72b81c5a148e5afe04c1d6dbc1af5e320eb01723eb620fb0b6d52c7fb9f3`；已在隔离的
pinned-source fixture 反向检查、正向 apply-check 并重放通过。run `44608855873` / commit
`832ed7e44a004fe2700b196826400a7bf43b5ad2` 完成 native arm64e link，产物 SHA-256
`3dd1e6a814e194244d8a6a381640badafc6acf3adeaa22e0857b7ffdc19302e6`，但 run 在 post-link
LiveLog integration verifier 失败：helper branch 计数门槛 `>=8` 高估了优化后 relocation 数；
不是 PTE 补丁编译错误，strict prelink 未运行。已将门槛恢复为既有最小值 7，并改为检查
实际编译源码中的 active update 位置、每 16 次 completed 计数和目标对象中的 marker。下一次
`ufs-integrated` run [44612581308](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44612581308)
已在设备提交 `6d82bbe50780b8fb84e383c00b609db555f23432` 成功。pinned framework
`0c1bac3d51a3009dca1708b15a5198f4b43d057a`、XNU `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`；
native arm64e SHA-256 `194ea250b0fab2d35ad6f473a87be8b642329d5eb2e80e39262729f634128c19`。
严格预链接 fileset 为 142,835,712 字节，SHA-256
`a6d93191e64c3d8e4cad63d23bcb24fa4e34ff433d6fac6d4331040c43fcb999`；production DarwinBoot
parser PASS，`mapping_complete=true`、`missing={}`、`stubbed={}`、`allow_missing=false`。
对象的 helper immediate relocation 数为 7；编译源码和对象 marker 的专门检查确认 completed-PTE
子步已编入。生产 package verifier 本地复核 6 个 archive objects、23 个强定义通过。完整记录见
`docs/evidence/live-checkpoints-prelink-rnx-substeps-44612581308.json`。此前 sampled
QR cadence 曾提前退出 checkpoint publisher，从而也挡住文本更新；该 gate 已移除，滚屏
现在每个有效 capture checkpoint 都更新；首次有效快照可 seed 一帧 QR，之后由独立 worker
以 250ms cadence 轮播，不再在 PTE 检查点中同步 fallback 编码。若 worker 未获调度，二维码
轮播会停止，但滚屏仍随内核检查点更新。
多帧 raw 文件按绝对范围去重：本次 `earlybackup=[0,498)`、`current=[0,662)` 时早期
字节已完全包含在 current，导出不会重复拼接这 498 字节。用户随后提供的二维码文件记录
`startup-phase` 和 `vm_mem_bootstrap` / phase 5 begin；其滚屏照片随后又出现 PTE 384，说明
QR 文件可能仍是尚未达到 16 轮替换门限的旧 immutable session，不能将屏幕较新内容误认为
已进入该 QR 文件。本版本的状态栏新增 `W/Q/B/R` 用来观察 worker、sequence、block 和轮数。

本次 build 已把 CDC transport 链接，测试启动参数也含 `tb710fu-cdc=1`；但官方补丁
`0012` 把 `tb710fu_cdc_start()` 放在 `bsd_init()`、紧邻 `vfsinit()` 之前。XNU 先执行
`machine_lockdown()`，再进入 `bsd_init()`，当前帧仍处于 `arm_vm_prot_finalize`，因此
尚未到 USB CDC 启动点。此时没有 USB port 属于阶段尚未到达，不能作为端口集成失败
证据。真实 USB 枚举和 CDC 主机收字节依旧未验证。固定 UniXNU 文档
[`4 KiB 页表`](/D:/unixnu-tb710fu-clean/.work/UniXNU/docs/zh/xnu/four-k-page-tables.md)
解释 4 KiB granule/L3 PTE；[`调试与日志`](/D:/unixnu-tb710fu-clean/.work/UniXNU/docs/zh/guide/debugging.md)
说明早期用屏幕或 UART、USB 前须确认协议兼容。设备 CDC 限制详见
[`USB_CDC_PORT.md`](/D:/unixnu-tb710fu-clean/docs/USB_CDC_PORT.md)。

后续屏幕日志还出现 `pe_arm_map_interrupt_controller: failed to find the
interrupt-controller` 和 `pe_arm_init_debug: failed to find cpu-debug-interface`，随后
滚屏到 `vm_mem_bootstrap` / phase 5。pinned XNU 中前者令 `pe_arm_init_interrupts()` 返回 0
并跳过其 `pe_arm_init_timer()`，但 `PE_init_platform()` 忽略返回值，因此不是此处的直接
panic；后者在没有 `panic_trace`/`stop_clocks` 启动请求时只是可选调试节点日志。当前生成
ADT 的 `/arm-io` 有 0 个硬件子节点（`docs/ADT_STATUS.md`），能解释 interrupt-controller
查找失败；它对平台其他 GIC/timer 初始化、worker 调度及启动的影响仍未验证。这是独立的
ADT/SoC 集成缺口，暂不根据 Linux DTS 单方面合成节点。

多帧 QR 版本已由官方 integrated run [44572852316](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44572852316)
验证成功，设备提交 `ddfd2c97d22f965caa24270dff9bf04551f453dc`。Native arm64e SHA-256
`8a7e5056bece5922ca530ceb0c44f6dfda9d35c286a0c0677b890971e5eaf577`；严格 prelink
142,835,712 字节，SHA-256 `02253bed245a3f15b78b03821f2d8cd7f78c464023414ea09d390ad26da2a6b2`，
DarwinBoot parser PASS，mapping complete 且没有 missing/stubbed。

签名多帧测试包为 `out/live-checkpoints-44572852316-multiframe-signed.zip`，35,566,530
字节，SHA-256 `2f0bf32df81a143b661851ba4137adda005ada235c401114aeba27bbfdf835a7`。
`vendor_boot.img` SHA-256 `5b2a0e17e351c3decfe09ff6e9affa362f4c7186550f6aab57508352b925a208`；
`vbmeta.img` SHA-256 `a49690d95c470c548701b61ddb3304141a9defedf0bfbf2ed55ac6bd0e1ae770`；
沿用基线 `boot.img`。AVB RSA-4096、boot chain/salted hash、其他 22 个 descriptor、FAT/LZMA、
启动参数及 ZIP 校验通过。package report 确认每份二维码 session 是不可变多帧日志，
带 ready early backup，16 轮后才替换；协议互操作样本为 3693 字节、6 帧。完整记录见
`docs/evidence/live-checkpoints-multiframe-signed-44572852316.json`。

这是最新签名诊断包，不表示根因已修复；多帧采集需要 receiver 收齐每个 session 的块并在
session 变化时 reset，若内核或 worker 停止，二维码循环也会停止。未刷写或重启设备；
`runtime_qualified=false`、`hardware_boot_verified=false`、`device_operations=false`。

## PRELINKTEXT 滚屏日志诊断包已签出 · 2026-11-19

实机回传帧对应 run `44524596920`、设备提交
`4abefb367d219908a9ebb3ebc2fdf830547e7e2d`，build label
`src-4abefb36-ramoops`。单帧范围 `[1502918,1503168)` 共 250 字节，
`dropped=0`；备份 `[0,498)` 共 498 字节并延后多帧导出。帧到达
`checkpoint=ld-prelink-rnx-enter`，随后记录 PRELINKTEXT 范围
`0xfffffff00700c000`、大小 `14385152`、结束地址 `0xfffffff007dc4000`，尚未到
`ld-prelink-rnx-head-done`。该边缘调用覆盖对齐后的首个 L2 区域，约 500 个有效
4 KiB PTE。0026 的 sampled 检查点会被入口 QR 后的 250 ms quiet period 抑制，
因此该帧还不能区分 TTE 查找/表选择和首批 PTE 更新。

新增补丁 `0027-tb710fu-prelink-rnx-edge-checkpoints.patch`，SHA-256
`49aa11bd13e0356ff9ec326cb7168cdc9ffc0dcf40889e904c31850b8a2e2baa`，接在 0026 后。
在边缘 helper 的 L2 TTE 查找、读取、已有表或新表分支加即时检查点；块展开与
PTE 循环每完成 64 项立即发布一次，以避免 sampled quiet period 隐藏短进度。
页表赋值顺序未改变。隔离 pinned-source fixture 的标准 `git apply --check` 通过；
官方 macOS `ufs-integrated` run [44538544326](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44538544326)
attempt 1 success，提交 `4cf79a8422da4d1e46c472c6421f2c39f5be8ea7`。Native arm64e
SHA-256 `467408dec8eed913810b764ce013959a6bdeb206ee203aea58d9ebb686971490`；严格
prelink Mach-O 为 142,835,712 字节，SHA-256
`feb58a85b7f7e786601ccf4458a76083ba2e66f0ffb4ee731cb32ff4a26bad03`，production
DarwinBoot parser PASS。Runner artifact 14539106331，39,167,082 字节，GitHub SHA-256
`c28889f89505e749fb676d0a6ac79801834a4c70e6c4f241a599b1d7e55e2188`。完整 provenance
见 `docs/evidence/live-checkpoints-prelink-rnx-edge-build-44538544326.json`。

用户要求保留二维码、移到右下角并开启滚屏日志。检查点模式现将最近 XNU capture
tail 显示在左侧有界 ASCII 文本区，检查点 QR 固定在右下角；日志按绝对字节范围
增量更新，检测到缓冲覆盖时显示 `[LOG GAP]`，字符采集回调保持纯内存写。顶部官方
色带和非检查点双通道布局保留。显示 ASan/UBSan、LiveXnu 15 组 adapter ASan/UBSan
和 live-log package 12 项主机测试通过。官方 `ufs-integrated` run [44546733725](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44546733725)
success；runner 报告 1 个真实滚屏分支、1 个右下二维码分支、6 个状态栏分支均从同步
checkpoint 发布路径调用。严格预链接 SHA-256
`fe056e7669aebf981dae73f7a7c55826a22acc81a67da1a197c1ce3776e7075b`，production
parser PASS，`mapping_complete=true`、`missing={}`、`stubbed={}`。

签名测试包为 `out/live-checkpoints-44546733725-scroll-ui-signed.zip`，35,565,495
字节，SHA-256 `786b0bb6c5a4c0b601548261d8554d6c12978b766dc9c34ddcef72d15829b604`。
`vendor_boot.img` SHA-256
`1e52d7c9ec16484e64a04ffc6d9b54806045bc9bed9f7d8e7bedb6b505a8ffd4`；`vbmeta.img`
SHA-256 `63f6d92c4b7e5f4c53d450d88acf8399937c3b36fb7793b14c2c738fd0bad988`；`boot.img`
沿用已验证基线。RSA-4096、boot-chain key、salted hash、FAT/LZMA 回读和 ZIP
SHA256SUMS 均通过；其他 22 个 descriptor 未变。完整证据见
`docs/evidence/live-checkpoints-scroll-ui-signed-44546733725.json`。

该包用于下一轮实机显示和 PRELINKTEXT 边缘页表进度验证；当前没有设备屏幕或相机
反馈，硬件启动、二维码可读性和原始故障指令仍未验证。本轮没有刷写或重启。
`runtime_qualified=false`、`hardware_boot_verified=false`、`device_operations=false`。

## PRELINKTEXT RNX 可刷写测试包已签出 · 2026-11-19

用户要求为 run `44524596920` 打包可刷写镜像。内核来自设备提交
`4abefb367d219908a9ebb3ebc2fdf830547e7e2d`，build label
`src-4abefb36-ramoops`，严格 prelink SHA-256
`f7e39da1b4869b40174200d53236af213753193e7b5f97d73d116ae8ccf5f1ed`。
沿用官方 standard-band 变换后，Mach-O SHA-256
`1f6511a050d884f3f4b8555ac08a8628faf05e059af6d4117177f9a13b2e394b`；6 个
修改 span、3 个 compact probe 和 production parser 均通过，其他 kernel 字节和
segments 未变。启动项保留 `tb710fu-live-log=1`、`tb710fu-live-checkpoints=1`
和 `kc-segment-release=0`。

签名测试包为 `out/live-checkpoints-44524596920-prelink-rnx-signed.zip`，
35,565,598 字节，SHA-256
`8a5772404ac1a1eed3b7634c117c1a9c5f68a232287b1a11c09e0a6275deca11`。
`vendor_boot.img` 为 100,663,296 字节，SHA-256
`373f213c39402097342c85209078f21805084d44b456ae237440a2b5f6ad93d5`；`vbmeta.img`
为 8,000 字节，SHA-256
`838cd3a69b4bd89103ffddf5c7321bd644fc48a7a40d5352a5249c8214de2b71`；沿用已验证
`boot.img`，SHA-256 `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`。
AVB RSA-4096 回读、boot-chain key、vendor salted hash 通过；vbmeta header、公钥和
另外 22 个 descriptor 未变。FAT 有 66,512 clusters，LZMA、kernel、ADT、root-policy
及 ZIP 内 SHA256SUMS 回读通过。

完整记录见 `docs/evidence/live-checkpoints-prelink-rnx-signed-44524596920.json`。
这是可用于实机诊断的签名测试包，不代表硬件启动或根因修复；本轮没有刷写、重启
或其他设备操作。`runtime_qualified=false`、`hardware_boot_verified=false`、
`device_operations=false`。

## PRELINKTEXT RNX 进度检查点已构建 · 2026-11-19

实机回传帧对应 `src-8252efb9-ramoops`，run `44516717448`、提交
`8252efb9d6dfd901fa2a981e8a30ecb8b7e0c84f`。当前单帧范围为
`[1503598,1503855)`，257 字节，`dropped=0`；备份帧为 `[0,498)`，498 字节，
仍标记 `deferred_multi_frame`。尾部依次有 `mbuf-tag-init done`、
`machine_lockdown`、`ld-vm-enter`、`kc-segment-release=0`、`ld-prefix-free`，
最后为 `ld-prelink-ro`。该检查点在
`arm_vm_page_granular_RNX(segPRELINKTEXTB, segSizePRELINKTEXT, ARM64_GRANULE_ALLOW_BLOCK)`
之前；它把范围缩到 PRELINKTEXT 权限更新，未确认 fault PC 或根因。

新增设备补丁 `0026-tb710fu-prelink-rnx-progress.patch`，SHA-256
`b4a602ca20189eada378d948a084179f0c1984a0bb31d7c4fbd359c95f4c988a`，按序接在
0025 后。仅当 live-log runtime gate 打开且该精确 PRELINKTEXT 范围正在处理时，
记录 L2 地址、边缘 PTE 批次和 deferred-TTE/TLB 边界；不改权限更新顺序、参数或
页表写入。对象验证实际包含 `_arm_vm_page_granular_prot` 与
`_arm_vm_page_granular_helper` 的 sampled checkpoint 分支。

官方 macOS 26 `ufs-integrated` run [44524596920](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44524596920)
attempt 1 success，设备提交 `4abefb367d219908a9ebb3ebc2fdf830547e7e2d`，框架
`0c1bac3d51a3009dca1708b15a5198f4b43d057a`，XNU
`ac9718fb1af618d5ce8678d0dc6e8a58f252216f`。Native arm64e SHA-256
`968ca464c7ede034e295ac3ec2603a847766ee2ff5134f1a0531ce140d4b1da6`；严格预链接
Mach-O 为 142,835,712 字节，SHA-256
`f7e39da1b4869b40174200d53236af213753193e7b5f97d73d116ae8ccf5f1ed`。生产
DarwinBoot parser PASS，79 个 fileset entries，`mapping_complete=true`、
`missing={}`、`stubbed={}`、`allow_missing=false`。检查点调用数为
`arm_vm_prot_finalize=10`、`machine_lockdown=5`。Runner artifact 14530166050，
39,166,481 字节；本地下载 SHA-256 与 GitHub digest 均为
`18d6a577d1a81ebc02263df63e56d0a75e5c2ae28ec0ea1e3e0fd809b3de1e4a`。

完整机器记录见 `docs/evidence/live-checkpoints-prelink-rnx-build-44524596920.json`。
此结果是 native+prelink 构建，不是实机启动验证；本轮没有签名或打包刷机镜像，
也没有设备操作。`runtime_qualified=false`、`hardware_boot_verified=false`、
`device_operations=false`。

## machine_lockdown 内部检查点镜像 · 2026-11-19

补丁 0025 的 macOS run 44516717448 attempt 1 成功，源码提交
8252efb9d6dfd901fa2a981e8a30ecb8b7e0c84f，二维码构建标识
src-8252efb9-ramoops。原生 arm64e SHA-256
1c3690215b989e8b5538baf41d4edf88714f952941911d05cb8bfd49f241fe42。
严格预链接内核共 142,835,712 字节，SHA-256
8f33d835564a1798aed62df76ca552dfc3f2738b925f6bc83ac46eb811516e24。
生产 DarwinBoot parser 在 runner 和本地均通过，mapping_complete=true，
missing={}、stubbed={}、allow_missing=false。核验 arm_vm_prot_finalize 的
10 个检查点分支和 machine_lockdown 的 5 个分支。
Runner artifact 14525614395 SHA-256
521e09c2e26ee58ac77d414c3ffd853949161ad27b1bba5053f5b3280b93ba88，
与发布 digest 一致。

本地签名测试包 out/live-checkpoints-44516717448-lockdown-signed.zip 为
35,565,452 字节，SHA-256
ad5237b8b797784877d843df4ffee889832572bc95bb823c0c2569916f042f6a。
沿用对照包的 kc-segment-release=0，内核标准色带版 SHA-256
4aadf486fccd5cd33aeddb451409f181e5d8da9112a25734c2d172b157213932。
AVB 镜像配对、FAT 启动参数、LZMA 内核和 ZIP 回读通过。镜像哈希见
docs/evidence/live-checkpoints-lockdown-build-44516717448.json。
实机根因待定位。下次采集 src-8252efb9-ramoops 的最后一帧，必要时记录
ld-prefix-free 的回收设置。runtime_qualified=false、loadable=false、
hardware_boot_verified=false、device_operations=false。

## 实机再次到达 machine_lockdown 调用前检查点 · 2026-11-19

实机回传 `src-9ac805c2-ramoops` 的单帧覆盖 `[1502335,1502589)`，
254 字节，`dropped=0`。上一次的范围是 `[1503573,1503827)`。两帧均记录
`mbuf-tag-init done`，随后以调用前的 `machine-lockdown begin` 结束。
两个测试包使用同一内核和 build label，帧中缺少 `kc-segment-release` 的
实际取值。即使本次用了参数对照包，可见边界仍停留在原处。
见 `docs/evidence/live-checkpoints-device-44502932009-lockdown-repeat.json`。

补丁 `0025` 在 ARM64 VM 收尾和页释放路径加入同步检查点，并在
`ld-prefix-free` 帧前打印已解析的 `kc-segment-release`。检查点覆盖
页权限修改、异常栈分配与切换、`__BOOTDATA` 回收、TLB 刷新和延迟页释放。
Patch SHA-256 为 `52e2eb073b57457ff8a5fcc23439d95d6fa4388a6563f11f23191672db90081e`。按 run `44502932009` 的输入清单和哈希重放
相关补丁后，独立 fixture 应用与 14 组 adapter 主机测试已通过。

## 实机到达 machine_lockdown 入口 · 2026-11-19

`src-9ac805c2-ramoops` 交付镜像的实机单帧覆盖 `[1503573,1503827)`，共
254 字节，`dropped=0`。`OSKextRemoveKextBootstrap` 和 `mbuf_tag_init` 的
`done` 已出现，随后发布了 `machine-lockdown begin`。该帧对应
run `44502932009` 的 `0xfffffff00c281130` 检查点调用，下一条指令
`0xfffffff00c281134` 调用 `machine_lockdown`。实机确认通过此前的 kalloc
和 SYSCTL 阶段。`machine_lockdown` 的返回与卡点指令仍待捕获。

打包同一构建的参数对照镜像，XNU 启动项追加 `kc-segment-release=0`。
这是固定 UniXNU `memwired-kc-segment-release.patch` 提供的可选段回收开关，
其 SHA-256 为 `c2fa78ec97d6c0cd812d781ef9b8a95f356ca728c9ba86834c494aaffd0b69c0`。
对实际 Mach-O 的 ARM64 指令模型验证了 `ml_static_mfree` 的准入分支，覆盖
`__BOOTDATA`、`__LINKEDIT`、`__KLD`、`__KLDDATA`、镜像尾部和物理 aperture。
四个集合段最多保留 41,107,456 字节，约 39.2 MiB。这个参数保留 `machine_lockdown`
及其页权限收紧路径，用于实机隔离可选段回收的影响。

签名包 `out/live-checkpoints-44502932009-kc-release0-signed.zip` 为 35,565,208 字节，
SHA-256 `1c4ba38c796241e660ec28c8bef9e2f969b27f6904cbf7830feee98e5d5c04c0`。
包内 `vendor_boot.img` SHA-256 为 `a5fcf3735a0308f0237c0975b33a968af1514bb51493beabbc57248563310a0f`，
`vbmeta.img` SHA-256 为 `88ee76ecef77245d1921ed3c65e1d3ddce9e3c0e37af811159eec918bd175073`，
内嵌标准内核仍为 `d6dea82b08c73db409f27df1a939086b1dce38361bf3b9a21980e019927ed5ce`。
AVB 签名、FAT 参数、LZMA 字节和 ZIP 回读通过。证据见
`docs/evidence/live-checkpoints-device-44502932009-machine-lockdown.json`。
下次实机采集 `machine-lockdown done`、后续阶段或最后一帧。

## 限速检查点签名镜像包 · 2026-11-19

应用户要求制作刷机测试包 `out/live-checkpoints-44502932009-signed.zip`，
大小 35,564,913 字节，SHA-256
`5929a5a57ed4a888dcf6e3256a4c72534a29cf8a33adfccbdc7f9c0758311e2a`。
内核来自 run `44502932009` attempt 1、提交 `9ac805c2b5b968580a93c1a642ec26e09c26979f`，
build label `src-9ac805c2-ramoops`。生产 DarwinBoot parser 通过，标准色带版
内核 SHA-256 为 `d6dea82b08c73db409f27df1a939086b1dce38361bf3b9a21980e019927ed5ce`。
内置启动参数包含 `tb710fu-live-log=1 tb710fu-live-checkpoints=1`。

镜像位于 `out/live-checkpoints-44502932009-signed/`。

- `vendor_boot.img`，100,663,296 字节，SHA-256
  `8455aa9d3c915bc4b689e245d7fdc5f0a158ebf0436714a6c3c3d25049db1b67`。
- `vbmeta.img`，8,000 字节，SHA-256
  `408028c7f0fac2961b857ab0f31c25e1de7f598d42130c34d6731c935450c245`。
- `boot.img` 沿用签名基线，100,663,296 字节，SHA-256
  `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`。

在 run `44479475027` 的已核验 unsigned carrier 中仅替换 FAT 内的 LZMA 内核，
FAT 其余文件与基线逐字节比对通过。unsigned vendor SHA-256 为
`5156edd6ddb11e0cf9680e46cf6e140d0e725fc2482ab7982d869cf7cbf353e6`。
沿用本地 AVB RSA-4096 签名基线 run `44124706860`。主机核验 AVB 签名、
`vendor_boot` salted hash、boot chain key、vbmeta header 与 22 项其他描述符，
以及 FAT、ADT、根卷策略、LZMA 和 ZIP 内 SHA256SUMS 回读均通过。
详细证据在镜像目录的 `packaging-report.json`、`signing-report.json` 和
`verification-report.json`。`runtime_qualified=false`、`loadable=false`、
`hardware_boot_verified=false`、`device_operations=false`。

## 实机 fallback 分配记录与检查点限速 · 2026-11-19

实机转录 `src-5b689373-ramoops` 对应 run `44479475027`、提交
`5b6893730575efa0ed7c43a964ee7a4182438739`。该同步单帧带有 `[212979,213230)`
共 251 字节，`dropped=0`，可见 class 6 第 109/109 组的 `fixed-fallback-assign done`。
核对该 run 的 prelink 文件和指令后，分配函数的返回已确认。`done` 写入后调用
`tb710fu_live_log_checkpoint`，外层 helper 随后通过 `retab` 返回。这次转录的最后
可见记录在 helper 返回之前，实机根因待定位。转录和指令地址见
`docs/evidence/live-checkpoints-device-44479475027-fixed-fallback.json`。

原同步模式的每个签名组至少请求八次完整 QR 更新。较早的限速构建 run
`44462593602` 在实机上走到了 SYSCTL rank 1。设备 patch `0024` 给 kalloc 循环与
startup callback 的二维码尝试增加约 250ms 间隔，从上一次尝试结束计时。
组内事件继续写入 XNU 日志缓冲。kalloc 及 fixed/variable views 的入口和结束、
startup phase、bootstrap 子阶段和画面刷新即时尝试绘制。Patch SHA-256 为
`3fbd3de50a554522b4f7f8e7b720020af17bf9cdc1819206dc684e41a1547716`。

按 pinned series 顺序实际应用通过。主机验证通过 14 组 ASan/UBSan adapter 测试、
真实 pinned Mu 编码、固定 OneSend 接收器、显示边界和 11 组打包参数测试。
macOS `ufs-integrated` run `44502932009` attempt 1 结论 success，提交为
`9ac805c2b5b968580a93c1a642ec26e09c26979f`，build label `src-9ac805c2-ramoops`。
Native arm64e SHA-256 为 `d14a29e1af778744c4d3db34d8a8a88e54329c7da224d4d82c3b6c52423f5fea`。
Strict prelink 文件 `out/kernel-44502932009/kernel.macho` 为 142,819,328 字节，
SHA-256 `2cdc4d3acba2af3bd41f9b242bb23710e3ef2eec1d58f13aec4279a778257946`。
生产 DarwinBoot parser 在 runner 和本地回读均通过，`mapping_complete=true`、
`missing={}`、`stubbed={}`、`allow_missing=false`。startup.o 核验 2 个采样分支
和 80 个即时分支，kalloc.o 核验 100 个采样分支和 6 个即时分支。

Runner artifact ID `14518218866`，ZIP SHA-256
`2e3cde57f0f5782d21ed54d80774fd3955a4a7ba04ecba1921e190e4ed792dc3`，与 GitHub
发布 digest 一致。完整验证记录见 `docs/evidence/live-checkpoints-paced-build-44502932009.json`。
本次交付为预链接 Mach-O，`runtime_qualified=false`、`loadable=false`、
`hardware_boot_verified=false`。下一次实机验证观察 `fixed-views done`、
`kalloc-init done` 或更晚的启动阶段。

## 用户授权签名的同步检查点 AVB 包已验证 · 2026-11-19

用户随后明确要求签名。用本地 AVB RSA-4096 key 对 unsigned candidate
`de5c688cd09df32b848cd00e8738784a8fb38a259b158d4e47dcf173652fac43` 签名，基线为
已核验 source vendor/vbmeta `a07f25494968562b8d34a26bf3591b267956959a17850dff68d38e9f440790f2` /
`3fd43a5a7adc0275eb8d241b94f1c2520b6b630ae912c3eff4a3869d76228e04`。`boot.img`
从同一已验证签名 carrier 原样沿用，SHA-256
`a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`。

签名输出位于 `out/live-checkpoints-44479475027-signed/`：
`vendor_boot.img` 为 100,663,296 字节，SHA-256
`0c2274909b4b40ace56103b6e0c8093322d7c1f876735f2877f5346a8e86f454`；`vbmeta.img`
为 8,000 字节，SHA-256
`7c8e66e4be41f28109d2e8f2de23dc1adadcb2215feb59187c80d204eccd2d3f`。配套 ZIP
`out/live-checkpoints-44479475027-signed.zip` SHA-256：
`9e59c712c6ad6a660bbc975833a267890c1a8eeb805ad51b76a30e01ade6f9a6`。

AVB tool 回读验证 boot/vendor_boot footer 和 vbmeta RSA-4096 签名、vendor_boot
salted hash、boot chain key；vbmeta header、公钥和其他 22 个 raw descriptor 未变，
unsigned vendor body 逐字节保留。FAT kernel/args/ADT/root-policy 回读、LZMA
round trip、FAT 几何及 ZIP 内 SHA256SUMS 均通过。完整报告为该目录的
`signing-report.json` 与 `verification-report.json`。

这是签名配对，不等于硬件启动验证：`runtime_qualified=false`、`loadable=false`、
`hardware_boot_verified=false`。没有刷写、重启或其他设备操作。

## 同步检查点 unsigned vendor_boot 已打包 · 2026-11-19

用户要求打包 img。使用成功的 kernel run `44479475027` / device commit
`5b6893730575efa0ed7c43a964ee7a4182438739`，prelink SHA-256
`57717219eab8934bde402ce6b438807af109cbe7a73073729880b8c488482409`，生成标准
early-band 变体并独立验证 6 个允许改动 spans、3 个 compact probes，其余 kernel
字节和 segments 未改。标准 kernel SHA-256：
`eab0b717bf61830595fcc80a0f37c34600269d1cfd0ebf7e681b7d143526dd20`。

打包沿用已验证 unsigned carrier `out/live-ramoops-44462593602/`，其 unsigned
vendor_boot SHA-256 为
`cbfdae23a53cd6f4a67ad47b5444e8fd438dd7f9688b02cf9da4408412aa653a`、payload SHA-256
为 `a0ae8a83a1bea436544d762814323dec4e4325146cb6e3e6711a61d9fa2bee40`，源签名基线
vendor/vbmeta 分别为 `a07f25494968562b8d34a26bf3591b267956959a17850dff68d38e9f440790f2` 与
`3fd43a5a7adc0275eb8d241b94f1c2520b6b630ae912c3eff4a3869d76228e04`。LZMA-Alone
使用项目固定的 8 MiB dictionary、`lc=3 lp=0 pb=2 preset=6`；decoded kernel 与
标准变体逐字节一致。`unixnu.toml` 的 XNU entry 增加
`tb710fu-live-checkpoints=1`，其他菜单、默认项、ADT 和 root policy 保持原值。
FAT32 有 66,512 clusters；配置、LZMA kernel、ADT、root policy 都做了 FAT 回读。

unsigned vendor_boot 为 37,859,328 字节，SHA-256
`de5c688cd09df32b848cd00e8738784a8fb38a259b158d4e47dcf173652fac43`；经独立范围
比较，ramdisk/payload 外所有字节与 carrier 完全相同。完整 bundle 为
`out/live-checkpoints-44479475027-unsigned.zip`，SHA-256
`a1fec3dfebd0f044b20819a7b595891ddd3fc77a97056150ffcc45fd7105da98`；包内
`SHA256SUMS` 全部回读通过，详细记录见同目录 `packaging-report.json`。

遵循 clean cloud-build 指示，本次没有签名，也没有生成/附带匹配的 vbmeta，
所以这是 unsigned packaging candidate，**不可直接刷写**。没有执行任何设备操作；
`runtime_qualified=false`、`hardware_boot_verified=false` 继续成立。

## Bootstrap 子操作检查点扩展已构建 · 2026-11-19

用户要求再增加检查点以减少逐轮编译。本轮在 `0022` 已覆盖的全 startup callback
begin/return、完整 kalloc 路径和 SYSCTL 后 bootstrap 阶段上，继续拆分
`kernel_bootstrap_thread` 内的五次 `early_random()`、`kernel_list_tests()`、
`kernel_do_post()`、`thread_bind()` 与 `resume_cluster_powerdown()`；每个操作有
独立 begin/done QR。XNU patch `0023-tb710fu-bootstrap-suboperation-checkpoints.patch`
接在 `0022` 后，只增加 gated logging/QR 调用，不改操作顺序、参数或返回值。
patch SHA-256：`6faf8482e64642f3cad6d4360dc13e5e74ff2cad1f0761fc20899019089d4a4f`。

按 `0022 → 0023` 顺序应用到 startup fixture 通过。Adapter 13 组 ASan/UBSan、
固定 OneSend 接收器互操作、真实 Mu QR 编码、display 与 live-log/package 参数测试
通过。单次 macOS `ufs-integrated` run `44479475027`，代码提交
`5b6893730575efa0ed7c43a964ee7a4182438739`，结论 success；native arm64e SHA-256：
`6ac3639d1b89a5af59a60fe821ae0e4036791a74fec19d0836f8c43c223203b3`。Strict
prelink candidate 为 142,819,328 字节，SHA-256：
`57717219eab8934bde402ce6b438807af109cbe7a73073729880b8c488482409`；生产
DarwinBoot parser 通过，`mapping_complete=true`、`missing={}`、`stubbed={}`，但
`runtime_qualified=false`、`loadable=false`、`hardware_boot_verified=false`。
集成报告确认 startup.o 有 82 个、kalloc.o 有 106 个 checkpoint 直接分支；build
label 为 `src-5b689373-ramoops`。

Runner artifact ID `14507674751`，ZIP SHA-256
`658be95848b9fcc28505dc5a602fd66d4d937c0cb01fec84072b7f52889c5f68`，与 GitHub
digest 一致。未签名或打包 flashable image，未刷写、重启或改动设备。它扩大了
可见阶段范围，但仍不是故障 PC；硬件启动与二维码面板/相机可读性未验证。

## 同步单帧停点诊断已构建 · 2026-11-19

用户指出内核停住后二维码本身不再刷新。停住后的代码无法更新 framebuffer；
因此新模式改为在每个目标操作开始前同步生成并绘制检查点 QR，让最后一个
成功的 begin 阶段在随后停住时留屏，而不把它误报成 fault PC。

新增显式启动参数 `tb710fu-live-checkpoints=1`，须与 `tb710fu-live-log=1`
同时使用。该模式不启动竞争 renderer worker，普通 250ms poll 不会覆盖检查点；
每次 callback/kalloc/既有 bootstrap 检查点都会创建独立 OneSend 单帧 XNU 文件，
以最新可容纳尾部、阶段字段和准确 `[start,end)` 范围同步编码绘制。每个检查点
为新 session；接收器需 reset 旧 session。早期备份保留在 RAM 并标记 deferred，
单帧不声称包含完整历史。未设置新参数时，旧双通道限流轮询行为不变。

XNU patch `0022-tb710fu-synchronous-live-checkpoints.patch` 接在 `0021` 后，
在 startup callback 前/返回后以及现有 kalloc/bootstrap markers 上调用强 provider。
强 API 声明由 `0014` 导出。包校验要求两个 runtime boot arguments 同时存在；
native integration verifier 还检查 startup.o 与 kalloc.o 对 checkpoint provider
存在直接 branch relocation。patch 0022 SHA-256：
`efad3fb1804f24e9a5804b1bde88ee40a2fda51c85788e91a6a806c3bf7398bc`。

本机验证：13 组 LiveXnu adapter ASan/UBSan 测试、实际 Mu QR 编码、固定 OneSend
浏览器接收器互操作、display renderer、live-log packaging 和 source-storage 参数
边界测试均通过；patch 0022 对 pinned startup/kalloc fixtures 的应用检查通过。
本机 Clang sanitizer runtime 不可用，因此 adapter sanitizer 用 GCC 执行；正式
native 编译由受支持的 macOS runner 完成。

GitHub macOS `ufs-integrated` run `44474773580`，提交
`bf0d580f41448b24064be576f24370c299523bba`，结论 success。Native arm64e SHA-256：
`250685f51793fda2dbac6b7e7e74bf974616df57269149a4e64b605b6bad52d3`。
Strict prelink candidate 为 142,819,328 字节，SHA-256：
`86b673b35f0ca73ace367f467331ba4c1429a2cbc158bb9bcb9b677563816909`；生产
DarwinBoot parser 通过，`mapping_complete=true`、`missing={}`、`stubbed={}`，但
`runtime_qualified=false`、`loadable=false`、`hardware_boot_verified=false`。
Integration build label 为 `src-bf0d580f-ramoops`，报告已验证 runtime 参数及
startup 64 个、kalloc 106 个 checkpoint 直接调用分支。

Runner artifact ID `14506516154`，ZIP SHA-256
`79dac44d6d8c4b00eeb9f3bd8e5991205b80bb549de2a19fe0e0fce31a6ddf4a`；本地回读与
GitHub digest 一致。遵循 clean cloud-build 边界，本轮未签名或打包 flashable
image，未刷写、重启或改动设备。二维码/面板实机可读性与硬件启动仍未验证；
运行成功不代表内核已能启动到 userspace。

## 最新实机帧确认越过 kalloc，SYSCTL rank 1 正在返回 · 2026-11-19

用户提供 build label `src-8968159c-ramoops`，对应 kernel run `44462593602`，
内嵌 standard-band kernel SHA-256
`74e45d659e8df44590f172809a511e03b1073ece2c7fb6128d124b698a6a9494`。当前导出是
current-only 单帧 `[1187799,1188124)`，325 字节、`dropped=0`；未提供原始附件或
log hash。

用 run `44462593602` 的精确 fileset 做 `llvm-nm -n` 符号映射，通用 callback 函数
地址低 32 位 `0x0c2df010` 对应 `_sysctl_register_oid_early`。可见 arg 地址对应
`_sysctl__security_mac_labelvnodes`（转录仅残留地址尾）、
`_sysctl__security_mac_vnode_label_count`、
`_sysctl__security_mac_device_enforce`、
`_sysctl__net_inet_ipsec_ah_cleartos`、
`_sysctl__net_inet_ipsec_ah_offsetmask`、
`_sysctl__net_inet_ipsec_dfbit`、
`_sysctl__net_inet_ipsec_ecn`。这些是 phase 16 / SYSCTL / rank 1；源码中 rank 1
为 `STARTUP_RANK_SECOND`。完整显示的 callback-return 行证明相应 OID 注册回调已返回。

这确认执行已经越过 `kalloc_init` 并进入 SYSCTL rank 1 注册；它不是 fault PC，也不
证明 SYSCTL 整个 phase 完成或内核停在其中。帧末的完整可见记录是
`...ipsec_ecn` callback 返回，前后有截断片段；当前帧不含 phase-complete / 后续
EARLY_BOOT 结果。最新候选已含 `0019` kalloc、`0020` bootstrap 和 `0021` fixed-zone
group 检查点，不需要为这份 SYSCTL 日志再编译。下一步应继续用同一包读取后续帧。
地址映射和边界记录见
`docs/evidence/live-ramoops-device-44462593602-sysctl-rank1.json`。

## fixed kalloc group 边界与最终一次补充构建 · 2026-11-19

用户随后两次粘贴的二维码文本完全相同，均标识 `src-0e3190ab-ramoops`，对应
`ufs-integrated` run `44426482407`、提交 `0e3190abc54f60bc98014f2e816802643fe7ad7e`。
current-only 帧范围 `[92329,92662)`，333 字节，`dropped=0`；没有原始附件或日志
hash，因此两次粘贴应视为同一帧，不是两个连续时刻。帧末完整显示
`fixed-type-zone begin/done index=4 total=4`，表示该次 helper 内层 4 项迭代完成；
后续没有 `fixed-group returned` 等记录。它把当前可见边界收窄到 fixed-zone 分配
循环附近，但不是故障 PC，也不能证明 kalloc callback 没有继续执行。

新 patch `0021-tb710fu-kalloc-zone-loop-checkpoints.patch` 紧跟 `0020`，补 size-class
及 signature-group 索引、helper 返回、shuffle 和 zone assignment 前后事件，并覆盖
fallback/single-type 提前返回。allocator 调用、随机选择和循环顺序不变。隔离 pinned
XNU fixture 按 `0018`、`0019`、`0021` 应用通过；与独立的 `0020` startup fixture 合并后，
完整目标文件都按 series 顺序实际应用成功。

唯一一次最终补充 macOS `ufs-integrated` run `44462593602` attempt 1 成功，head
`8968159c3be1c93222d6f3131ca7d9f38e729781`。native SHA-256
`5051c65e77bc3550408f357a5f68f696ecbaadc21eb6ac2e15aaaaabba436111`，strict prelink
SHA-256 `dc4a8ae160cf8433e48057cc481a4204c9ed626dcfc70fa6890c649781cde941`，
production DarwinBoot parser PASS。runner artifact ID `14501435971`，SHA-256
`0e1a1ddf3e5ec60159c78e2b6a51436fd2f7653a5172cdab672d3d0bd32965b1`；`0019/0020/0021`
三个 patch hash 均与 runner inputs manifest 一致。

最终包 `out/tb710fu-live-ramoops-44462593602-signed.zip` 大小 35,579,443 字节，
SHA-256 `95f02eea63b1d67ac80eb15872718930c3ddbedd96c008ec77f3cffabe66e355`。内嵌
standard-band kernel SHA-256 `74e45d659e8df44590f172809a511e03b1073ece2c7fb6128d124b698a6a9494`。
RSA/AVB、FAT/LZMA 回读通过，22 个其余描述符、vbmeta header 和公钥未变；详细记录见
`docs/evidence/live-ramoops-image-44462593602.json`。

这份包已把固定区 class/group/view 范围、helper 返回和 shuffle/assignment 子操作纳入
同一诊断候选。设备帧记录见
`docs/evidence/live-ramoops-device-44426482407-kalloc.json`。当前根因仍未确认；包仍为
`runtime_qualified=false`、`loadable=false`、`hardware_boot_verified=false`。没有刷写或
重启设备。现在需要这份新包的连续 XNU 日志，不再为已覆盖的边界追加构建。

## SYSCTL 到 pageout 全路径检查点与单次构建 · 2026-11-19

用户提供 build label `src-5016bd2f-ramoops`，对应 kernel run `44401178047`、
提交 `5016bd2fdb2f993776314954fc09689a14fc32da`。XNU DEBUG export 当前只有
单帧 `[65032,65365)`，333 字节，`dropped=0`；原始附件/hash 未提供。可见日志含
fixed type-zone 进度、FIPSPOST `all tests PASSED (58 ms)`、
`corecrypto_kext_start completed successfully`，以及
`startup callback returned [16, rank -1] 0`。

精确源码 `startup.h` 映射 `16` 为 `STARTUP_SUB_SYSCTL`，`STARTUP_RANK_LAST` 为
`0xffffffff`，由 callback-return marker 的 `%d` 显示成 `-1`。`startup.c` 在
`cur->func(cur->arg)` 返回后才输出该 marker。因此新证据确认执行已越过 phase 7
`kalloc_init` 并进入 SYSCTL 的 rank-last callback；该行是 callback 返回记录，不是
fault PC，也不能单独证明停在 SYSCTL。当前 single-frame 导出没有给出随后 callback
或 `EARLY_BOOT` 记录，缺失不能当作停点证据。

用户要求直接补检查点后，新 patch `0020-tb710fu-bootstrap-phase-checkpoints.patch`
紧跟 `0019`，新增每个 `kernel_startup_initialize_upto()` 完整返回标记，并在
SYSCTL 后的地址随机化、IOKit、EARLY_BOOT、开中断、屏幕/commpage、lockdown、BSD
及 pageout 交接等同步初始化操作前后发出配对事件。只做 live-log 诊断，不改变任何
调用或顺序。隔离 fixture 按官方 `zzz-boot-progress`、设备 `0014/0016/0017`、新
`0020` 顺序实际应用通过；33 组 begin/done 配对、两个非返回边界和 `git diff --check`
均验证通过。

唯一一次 macOS `ufs-integrated` run `44426482407` attempt 1 成功，head
`0e3190abc54f60bc98014f2e816802643fe7ad7e`。native SHA-256
`e2c7a64a810f9b0a706607679fd16773dee1577e4c85cad7288a0c2a5a03ccaf`，strict prelink
SHA-256 `5a5ee24fd1d1f128dd54d99379d04cf24eb590ab18dc7fbe35eb7356b4652dcc`，
production parser PASS；runner manifest 中 `0020` patch hash 与提交文件一致。
artifact `tb710fu-ufs-integrated-44426482407` (ID `14485097215`) SHA-256
`7bce3b7f6076cf07b878c3b7dcad986af1f4ead89ba7816e212f9e0412de8171`。

新签名包为 `out/tb710fu-live-ramoops-44426482407-signed.zip`，35,579,217 字节，
SHA-256 `ef5be5d3f5c285010ecedbc8d96a9ed07677cede3c49e620f4ae340ca0532dfa`。内嵌
standard-band kernel SHA-256 `7529040226f707f7afd4de24629ea5ff1c7a59895e1955616956f137ce9ee776`。
签名/AVB、FAT/LZMA 回读通过，其他 22 个描述符、vbmeta header 和公钥不变；包内
`0019` 与 `0020` 标记均已确认。详细证据见
`docs/evidence/live-ramoops-image-44426482407.json`。

包仍为 `runtime_qualified=false`、`loadable=false`、`hardware_boot_verified=false`；
没有刷写或重启设备。SYSCTL rank-last callback 已返回，但具体后续停点仍要依此包的
后续 XNU 帧判断。此前设备 boundary 见
`docs/evidence/live-ramoops-device-44401178047-sysctl.json`。

## 一次性 kalloc_init 全路径诊断 · 2026-11-19

用户提供的 `src-02eed84f-ramoops`（run `44383673876`）当前帧最后显示
`TB710FU kalloc: fixed-views complete`，帧范围 `[127017,127346)`，329 字节。
这是 current-only 单帧，不能把缺少后续行解释成故障 PC；但它明确要求下一份候选
在 reset 前就留下边界。新 patch `0019-tb710fu-kalloc-full-diagnostic-trace.patch`
紧跟 `0018`，在 `bzero` 前后及计数重置前后分别记录事件，并覆盖 fixed/variable
view 解析、排序比较、迭代器、指针数组分组、zone 创建和 heap 分配。长循环按固定
间隔输出索引；所有日志仅在 live-log 活跃时产生，继续使用既有 250 ms poll 节流。
没有跳过或改动 allocator 的分配、随机选择、zone 初始化或 callback 顺序。

验证：隔离 pinned XNU fixture 按 `0018`、`0019` 顺序应用通过，`git diff --check`、
事件 begin/done 配对审计及配置检查通过。唯一一次 macOS `ufs-integrated` run
`44401178047` attempt 1 成功：native SHA-256 `c74c0cd1…a9a1ac94`，strict
prelink SHA-256 `49f49f58…c9afac93`，生产 DarwinBoot parser PASS；runner 输入清单
中的 `0019` patch SHA-256 与本地一致。完整构建 artifact SHA-256
`c4d50282…7d8d1285`。证据见
`docs/evidence/live-ramoops-image-44401178047-kalloc-full-diagnostics.json`。
随后已完成本地 AVB 镜像打包。内嵌 standard-band kernel SHA-256
`35b564b2229e2b7cb816e06af32ee25ed03e43c52bb21896bbd7b8082d996521`；配套镜像为
`out/live-ramoops-44401178047-signed/{boot.img,vendor_boot.img,vbmeta.img}`，
对应 SHA-256 分别为 `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`、
`ca65f55faf764a9547cc15c3ee79ab376ed8c6ed6b4b4d8840267023cb76e847`、
`98e5ef3fcad3c9fcb7ed18b03604f0e18cbfb4d46773c6c1dd559dcb6ee71822`。交付 ZIP
`out/tb710fu-live-ramoops-44401178047-signed.zip` 大小 35,578,921 字节，SHA-256
`b4aece41fea867cb5aa0879dcf406b7e5cdfd71125e8d7b45970bb1f9e99b66b`。RSA/AVB、
salted boot/vendor hash、FAT/LZMA 回读通过；其余 22 个 AVB 描述符、vbmeta header
和公钥未变。完整签名包证据见
`docs/evidence/live-ramoops-image-44401178047.json`。

包仍标记 `runtime_qualified=false`、`loadable=false`、`hardware_boot_verified=false`；
没有刷写、重启或执行设备操作。kalloc 根因仍待设备返回新 live-log 帧确认。

## 实机停在 kalloc_init 入口与子阶段诊断包 · 2026-11-19

用户提供的新版导出 build label 为 `src-94be46ec-ramoops`（run `44370040495`，提交 `94be46ec78bc598f7ac875aa976a87b8401f9d02`）。当前尾部最后一条是 phase 7 / ZALLOC / rank 2，arg=0。入口指针低 32 位与该精确 fileset 中的 `_kalloc_init` @ `0xfffffff00c274b18` 匹配；没有随后的 callback-return 行，所以只证明到达入口，不证明 fault PC 或精确故障指令。此前的 rank 0、rank 1 callback 及 rank 2 前序 callback 均已有返回记录。用户提供的转录为 current-only 单帧，current 范围 `[135174,135503)`，329 字节、dropped=0；不能据此推断其后未显示的日志内容。

固定 XNU 源码显示 `kalloc_init` 按序执行：zsize 计算、private/shared/early heap 初始化、256 KiB scratch 分配、fixed views 初始化、清零/reset、variable views 初始化、scratch 释放。没有证据支持跳过或改写任何 allocator 操作，因此没有做行为修复。新增 patch `0018` 在每个大步骤后发出有界 marker，标签为 `zsize`、`heap-private`、`heap-shared`、`heap-early`、`scratch-alloc`、`fixed-views`、`fixed-reset`、`variable-views`、`scratch-free`；仅 live-log 启用时生效，不改变 allocator 行为，沿用全局 250ms poll 节流。

patch `0018` 已在 run `44383673876` attempt 2 的 macOS native/prelink 构建中通过；marker 字符串已在 fileset 中确认。新可刷写格式测试候选为 `out/tb710fu-live-ramoops-44383673876-signed.zip`，大小 35,579,272 字节，SHA-256 `a88bad697aa242cfe42ebbbd005752ec64e55f7205376fafe8a42f5570b53c5f`。设备日志和镜像证据分别见 `docs/evidence/live-ramoops-device-44370040495-kalloc.json` 与 `docs/evidence/live-ramoops-image-44383673876.json`。没有刷写、重启或改设备存储；是否能越过该 callback 仍待新包实机日志确认。

## 实机阶段 7→16 转录与一次性全 callback marker · 2026-11-19

用户提供的旧包转录 build label 为 `src-90b857ac-ramoops`（run `44352442697`），不是最新的全 marker 包。对该 run 的精确 fileset 映射显示 phase 7 是 ZALLOC：日志在 `_vm_btlog_init` 和 btlog 提示后继续到多个 rank callback；随后 VM bootstrap done，并继续打印 phase 7 rank 1/2/3。出现后续 entry 证明前一个 callback 返回，因此执行已越过 phase 7。之后进入 phase 16 SYSCTL，四次 entry 均为 `_sysctl_register_oid_early`，参数不同；entry 在调用前打印，最后一次 callback 是否返回无法从旧包日志判断。证据见 `docs/evidence/live-ramoops-device-44352442697-phase16.json`。

用户指出 ramoops 只显示 `0:0 (0B)`，说明它不是可靠的最终停点记录；它是轮播的有界尾部，不能保证相机捕获最后更新。故修复方向改为在 XNU 的统一 startup loop 对每个 callback 返回后记录 `[subsystem, rank, arg]` 并有界 poll，而不是逐阶段追加 marker/重复编译。patch `0017` 已在新包 build run `44370040495` 中通过 native、strict prelink 和 production parser；marker 字符串确认存在于 prelinked fileset。run 的第一次 attempt 是 patch 文件 hunk 行数错误，runner 在应用 patch 时拒绝，未进入编译；修正后 attempt 2 全部通过。

该 callback-only 候选已由上方含 kalloc 子阶段 marker 的 run `44383673876` 取代；旧包 ZIP SHA-256 为 `7de6a69d6cc6d407500e5ceae94f46e4cb47045207a5bff49d582f3b51c26f48`，不要与新包混用。

## 全阶段 startup callback 返回诊断基线 · 2026-11-19

用户提供的设备转录来自旧包 `src-90b857ac-ramoops`（run `44352442697`），不是本节的新包。按精确 run `44352442697` fileset 的 `llvm-nm` 映射，phase 7 是 `ZALLOC`：rank 0 地址对应 `_vm_btlog_init`，后续 rank 1/2/3 分别对应 `_vm_kernel_boostraped`、`_vm_page_module_init`、`_zleak_init`、`_zone_create_startup` 等。随后出现的下一条 entry 证明前一 callback 已返回；日志最终已越过整个 phase 7 并到达 phase 16 `SYSCTL`。

phase 16 四条 entry 的低地址均映射到 `_sysctl_register_oid_early`，arg 分别为 `0xfffffff007e2b490`、`0xfffffff007dfb600`、`0xfffffff007e0e0a8`、`0xfffffff007e0dff8`。entry 行在调用前打印；四条行只证明已经进入这四次调用，最后一个 callback 是否返回尚未知。提示 `Unable to find vm tag for btlog` 后启动继续，不能据此判定致命错误。此次转录的阶段/符号和不确定性见 `docs/evidence/live-ramoops-device-44352442697-phase16.json`。

为避免按阶段逐次构建，XNU patch `0017` 在每个 startup callback 返回后记录 subsystem、rank、arg 并调用既有有界 poll；entry 顺序、callback、arg 和启动行为不变，250 ms 全局节流仍保留。新构建 run `44370040495` attempt 2 已通过 native arm64e、strict prelink、production DarwinBoot parser 及宿主检查。prelink SHA-256 `d7bcc55aea54ceb4e8601be9a50e4e72d12305205ab29a786895e5fa6471becc`；该通用 marker 字符串已在此 fileset 中确认。

新候选镜像在 `out/live-ramoops-44370040495-signed/`，匹配 ZIP 为 `out/tb710fu-live-ramoops-44370040495-signed.zip`，SHA-256 `7de6a69d6cc6d407500e5ceae94f46e4cb47045207a5bff49d582f3b51c26f48`。独立 AVB、salted hash、22 个未变 descriptor、FAT 与 LZMA 回读均通过。它是格式/签名已验证的测试候选，不是硬件验收；未刷机、未重启，运行/扫码仍待实测。完整构建和镜像 provenance 见 `docs/evidence/live-ramoops-image-44370040495.json`。

## macOS native/prelink build and可刷写候选 · 2026-11-19

本次修复提交 `90b857ac1f61f61acb30eb59d3daf024c3f7c2ba` 已在 GitHub Actions
run `44352442697` attempt 2 的 `ufs-integrated` 阶段完成。第一次 attempt 仅因
macOS runner 长时间排队、job 未分配 runner 而被取消；没有执行任何编译步骤，
不是源码或编译器失败。attempt 2 的 native arm64e、实际 Mu/QR、ramoops/LiveLog/
CDC 宿主检查、strict prelink 和生产 DarwinBoot parser 全部通过。artifact
`tb710fu-ufs-integrated-44352442697` 为 39,152,253 字节，GitHub digest
`sha256:3598c3d1a3baf374cc0be853462fea5dda33bd2f508fadaee635e341cf03aa06`。

Native arm64e SHA-256 为
`efe1b2a43d42eb6096b9016c9f8ded69f7bc78e021e7eab40b018d039c690e54`；严格
prelinked fileset 142,819,328 字节，SHA-256
`2f3bef2013f35b8998b178eae662b67653f48346c51ad62c30f4c5c9fc9503c2`。
`mapping_complete=true`、`missing={}`、`stubbed={}`、`allow_missing=false`；
LiveLog/UFS/CDC 集成均链接进 kernel，生产 loader parser PASS，仍保持
`runtime_qualified=false`、`loadable=false`。

使用官方标准三色早期带变换生成交付内核，原 prelink 哈希不变，变换后标准带
内核 SHA-256 为
`d18b22f292d0b2ee2555a7a9bf33dbad81ab2463ada5650c53417d87047f3df2`。LZMA
回读、FAT 结构、ADT、`unixnu.toml` 和 `.vroot` 保持检查通过。打包工具补充了
此前已验证的 UFS signed baseline `a07f25494968562b8d34a26bf3591b267956959a17850dff68d38e9f440790f2`
allowlist；未修改官方缓存或脚本。

本地签名使用现有授权和 test key，生成精确匹配的三文件集合：

- `out/live-ramoops-44352442697-signed/boot.img`：SHA-256
  `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`
  （未改变的 loader 基线）
- `out/live-ramoops-44352442697-signed/vendor_boot.img`：SHA-256
  `a31c32a07bce37788047d367bba2b166859014ac377b829a337f2f280a9092fb`
- `out/live-ramoops-44352442697-signed/vbmeta.img`：SHA-256
  `58c90c876078804bfcf3c3ca7f8a9965c8372554cabcebbcd2fe48610dfb22f6`
- `out/tb710fu-live-ramoops-44352442697-signed.zip`：35,578,789 字节，
  SHA-256 `725784e7d1d692486595ab9875d96813f7d45b8136172468a5273fae5c471fee`

独立校验通过：三处 RSA-4096/PKCS#1 签名、boot/vendor salted hash、22 个未
改变的 raw descriptors、vbmeta header/public key、boot chain key、FAT
payload、ADT/config/vroot 和 LZMA kernel readback；ZIP integrity 与
`SHA256SUMS` 回读通过。证据：
`docs/evidence/live-ramoops-image-44352442697.json`。

这是可刷写格式的测试候选，不是硬件验收结果。未刷写、未重启、未切槽、未修改
设备文件系统；硬件启动、二维码相机解码、CDC 枚举、worker 调度和 macOS
userspace 仍未验证。

## ramoops QR 首帧延迟修复 · 2026-11-19

用户提供的 ramoops 文件为 43 字节，仅含 XNU 启动横幅；SHA-256
`7f35085750ebc3f6b3cc83773d47a5cfbb131913192cc89dc358ae0de8af1e36`，文件名范围
`[0,43)`，generation 1。配对的 XNU 导出为 653 字节，SHA-256
`c2f24cdecd68cd58b3df348d1cd422c646ca66490676c751ae21a3626840a05b`，build
`src-1446070e-ramoops`，early backup 409 字节，current 范围 `[904,1243)`
共 339 字节，dropped 为 0。

构建身份是设备提交 `1446070ec13d33165c344dab3be9c21d6d93d83d`、GitHub run
`44325375059`、prelinked fileset SHA-256
`56a6931b5451d90922e54595ea6ecf99380f4b169549dc9d72866851930e864e`。对该
精确 fileset 运行 `llvm-nm`，`_vm_compressor_set_size` 地址为
`0xfffffff00c28dd70`；ramoops 导出末行所给指针的低 32 位相符。该行是
KMEM rank-0 的待执行回调入口，不是 fault PC；此单帧导出没有回调返回标记，
但受 250 ms 轮询节流影响，缺少标记仍不能证明回调未返回。没有依据改 VM
压缩器大小或启动顺序。

已确认的诊断问题是 ramoops generation 1 在显示初始化时发布，内容只有合成
启动横幅；后续快照要通过两轮替换门限，启动若提前停住，屏幕会继续显示该旧
帧。`package/Tb710fuLiveLog/LiveXnu.c` 现保留 freeze 前的状态读取但不发布
ramoops 帧；freeze 后将下一许可槽优先给 ramoops，使用届时的真实尾部。全局
250 ms 节流不变，freeze 仍不编码或绘制，XNU/VM 启动代码及设备配置未改。

本地验证：LiveXnu 适配器 GCC ASan/UBSan 12 组、15 个无效视频场景通过；实际
serial/PE 控制台镜像 18 项通过；生产 ramoops 快照 10 项通过；内核配置解析和
`git diff --check` 通过。证据及输入哈希见
`docs/evidence/ramoops-first-frame-diagnostic-20261119.json`。此次源代码改动尚
未在 macOS runner 上做 native/prelink 构建，未生成新镜像，也未执行设备操作。

## Signed diagnostic image package · 2026-11-18

Following the user's packaging request and existing explicit local-signing
authorization, a matched test bundle is in
`out/tb710fu-live-qr-return-44325375059-signed.zip` (35,589,312 bytes,
SHA-256 `2b7fe30d7b978fdc3ad1dad6b93a0df9f8b259217e28224f73897aee01d22356`).
It contains the unchanged baseline `boot.img`, the new signed `vendor_boot.img`
and matching `vbmeta.img`, README, SHA256SUMS and verification reports. The
packaged kernel is the 142,819,328-byte standard-band derivative
`92d91b61ad6df670a1fa738e934b7d148a925c6393a7e1e720d2172ec0a2f906`, derived
from run `44325375059`'s prelink `56a6931b5451d90922e54595ea6ecf99380f4b169549dc9d72866851930e864e`.

Image hashes: boot `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`
(unchanged), vendor_boot
`0606e62d18028676a721e815dff3d23dbc7f4beda8125d801ffe657ed59d6e90`, and vbmeta
`d8d25f085b622a5830e93861b7ecfc8ef70cb10b3ca00d81703e3dbf1fed1f49`. Independent
verification passes for all three RSA structures, boot/vendor salted hashes,
22 unchanged descriptors, vbmeta header/public key, boot-chain key, four FAT
files and LZMA kernel readback. The signed ZIP passes member and SHA256SUMS
readback; no private key is included.

This is a signed, format-verified test package, not a hardware-qualified boot
image: the runner reports `runtime_qualified=false` and `loadable=false`;
hardware boot, QR camera decode, CDC enumeration and macOS userspace remain
unverified. Nothing was flashed, rebooted or changed on the device. Evidence:
`docs/evidence/live-qr-return-image-44325375059.json`.

## macOS runner build of QR/callback diagnostics · 2026-11-18

GitHub Actions run `44325375059` built commit
`1446070ec13d33165c344dab3be9c21d6d93d83d` on
`codex/ufs-transactional-handoff` with stage `ufs-integrated`; conclusion:
success. Native arm64e SHA-256 is
`af5b2efee21868c3c77db2963f9a4a029f3ec43a62d720921bea2b2e4a7071b6`.
Strict prelink produced a 142,819,328-byte fileset with SHA-256
`56a6931b5451d90922e54595ea6ecf99380f4b169549dc9d72866851930e864e`;
the production DarwinBoot parser passed. Mapping is complete, missing and
stubbed sets are empty, and allow-missing is false. The ordered patch list
contains 0016, and the callback-return marker string is present in the
prelinked fileset.

The runner passed the native build, pinned Mu/QR and protocol interoperability
checks, live-console capture, ramoops snapshot cases (10), display checks, and
the adapter check (12 groups, 15 invalid-video cases, including both new QR
failure counters). UFS standalone adapter stages were skipped by this stage's
workflow conditions. GitHub artifact `14436169457` is 39,152,354 bytes;
downloaded SHA-256 `c5c1a58fde3c01dd39cb7e262510f47c5c0f833178e2a5771c0d0688093d2e0d`
matches the GitHub artifact digest.

At this runner stage no image had yet been packaged or signed. The later local
signed package is recorded above. `runtime_qualified=false`, `loadable=false`,
hardware boot, ramoops QR camera decoding, CDC enumeration and macOS userspace
remain unverified. No device operation occurred. Full run evidence:
`docs/evidence/live-qr-return-marker-20261118.json`.

## QR render feedback and KMEM callback return marker · 2026-11-18

The user supplied a scan from `src-b11eddff-ramoops` (device commit
`b11eddff0e3a33ced74cf7f471c2359cc3af9f18`, macOS runner run
`44237938523`). The export retains 409 early bytes and has
`current_start=905 current_end=1244 current_bytes=339`, `dropped=0`, and
`raw_layout=current_only_single_frame`. Its final startup trace carries
`0x66a392f00c28dd14`; the low address bits match `_vm_compressor_set_size` at
`0xfffffff00c28dd14` in that exact run's `kernel.macho` (`llvm-nm`). This is
still the trace printed before the startup poll and callback, not a fault PC.
The reported `W=0`, `P=4`, and both `X/R=ready:512B 2/1` show ready source
snapshots and one-block sender state before the worker ran; they do not prove QR
encoding or framebuffer writes succeeded. The green/blue/magenta bands are the
intentional standard diagnostic bands in `LiveDisplay.c`.

Working-tree changes add `QR=X<encoded>/<drawn> R<encoded>/<drawn>` to the
status row. Tests cover both a ready source with QR encode failure and a
successful encode whose renderer rejects the write. New XNU patch `0016`
records and polls after the KMEM rank-0 callback only when the live-log display
is active. It invokes the existing bounded poll after the marker; the normal
global 250 ms frame throttle remains in force. A readable return marker will
prove that callback returned; its absence cannot distinguish failure inside
the callback from an earlier stop or a halt before the next permitted frame.
No speculative compressor sizing change was made.

Verification: WSL GCC 14.2 ASan/UBSan adapter checks pass (12 groups, 15 video
cases); production ramoops snapshot checks pass (10 cases); portable display
pixel/guard checks pass. `tools/ci_kernel.py configure` resolves the new ordered
patch series, and `0016` passes `git apply --check` and actual application to a
temporary LF-normalized copy of the selected `startup.c` fixture.
The WSL Clang sanitizer runtime is absent, so local C host checks used GCC.
Local protocol interop was unavailable because WSL lacks Node.js; the pinned
protocol check passed on the macOS runner above. At this diagnostic-source
stage no image had yet been signed; the later package is recorded at the top of
this file. No device operation occurred.

## Short gate built, signed and ready for the device · 2026-11-18

Run `44237938523` (attempt 1, job `134578502483`) built commit `b11eddff` on the
supported macOS runner and passed every step: the ramoops/live-log host checks,
native arm64e compile/link, pinned Mu/OneSend encoding, strict prelink and the
production DarwinBoot parser. Native SHA-256 is
`d3943ee2391c06cb9f0999d26f390921d4586abb4d9303cf504b3efd481aafd0`; the
142,819,328-byte prelinked fileset SHA-256 is
`665b41f1248ab74afb705453e8b8e786004253263f12dafeca8c8d5ba1f6b35d`. Mapping is
complete with empty missing/stubbed sets and allow-missing false.

The runner confirms what could not be checked locally. `adapter-check.json`
reports `result: PASS`, 10 groups, 15 invalid-video cases, and both new checks:
`single_frame_mode_uses_short_replacement_gate` and
`pre_worker_header_and_tail_fit_one_systematic_frame`. Protocol interop is PASS
including the single-frame capacity boundary. In the packaged kernel,
`_tb710fu_live_log_can_snapshot_single` is defined at `0xfffffff00c250bd0`, is
called from two sites, and its body compares `rounds` against 1 (i.e. requires
2 rounds) while `_tb710fu_live_log_can_snapshot` still compares against 15.

The standard-band derivative is `8826ed99…` (6 patch spans, 3 compact probes,
other bytes and segments unchanged). Signed images are in
`out/live-ramoops-44237938523-signed/`: boot `a3236acc…` (identical to the
baseline), vendor_boot `f03102bc9d6d32f84b6e21d560a827692855730c23b124a8451705111816693c`,
vbmeta `c5ae737e19f553e355b4475fc6ee3d9c9e3e748be02e6d30cbf107d18e0f62ff`.
Independent RSA/salted-hash/payload/ADT/descriptor checks pass with 22
descriptors unchanged. ZIP is `out/tb710fu-dual-live-44237938523-signed.zip`,
35,576,693 bytes, SHA-256
`6d71f8c6f77a2d186ad3ddb3efb4f57cad6d8fd5c9270942c71127131fc3640d`.

Still a test candidate: no device boot, no CDC enumeration, no QR scan, and no
evidence that the worker thread is ever scheduled. ramoops remains a volatile
current-boot buffer that cannot survive reset. No device operation occurred.
Evidence: `docs/evidence/dual-live-runner-44237938523.json`,
`docs/evidence/dual-live-signed-44237938523.json` and
`docs/evidence/dual-live-device-44165508547.json`.

## Pre-worker replacement gate shortened · 2026-11-18

The user scanned `src-d181905f-ramoops` on the device: the LEFT QR decodes and
the RIGHT QR is still empty, and the display does not cycle after the kernel
stops. The decoded export is `earlybackup_bytes=409`,
`current_start=904 current_end=1243 current_bytes=339`,
`raw_layout=current_only_single_frame`, and its last line is
`kernel_startup_initialize_upto[6, rank 0]: 0x40ef54700c28dc98(0)`. The
PAC-bearing pointer resolves exactly to `_vm_compressor_set_size` in this
fileset, i.e. `STARTUP_SUB_KMEM` rank `STARTUP_RANK_FIRST`; that line is printed
before the callback runs, so it names the callback next to execute rather than a
fault PC.

`314 + 339 = 653` reproduces both the fixed single-block capacity
(`720 - (26 + 31 + 10)`) and the trim loop, so the LEFT path worked as designed.
The reported symptoms follow from two limits, not from a protocol defect: the
512-byte pre-worker snapshot plus the 16-round replacement gate needs about 8 s
of running XNU (two polls per second per channel), and a frozen framebuffer
means no code reaches `tb710fu_live_log_poll()` at all. A still photograph can
only show the last published snapshot.

Fix applied in this working tree, adapter/protocol layer only:
`TB710FU_LIVE_LOG_SINGLE_FRAME_ROUNDS` is 2 and
`tb710fu_live_log_can_snapshot_single` gates the pre-worker single-block
publisher on it, while `tb710fu_live_log_snapshot_file` keeps the 16-round
multi-frame dwell. Both streams use the matching gate. Header compaction was
attempted, measured at 17 bytes, and reverted to avoid changing existing parse
anchors.

Python checks that need no compiler pass: 10 of 11 `LiveLogPackaging` tests
(the excluded one fails on a sandbox temp-directory permission, unrelated to
this change). This machine has no C compiler and no Node, so
`check_live_xnu_adapter.py` (added check
`single_frame_mode_uses_short_replacement_gate`), `check_live_log_protocol.py`
and every native/prelink stage still require the macOS runner. No image was
built, signed or flashed, and no device operation occurred. Evidence:
`docs/evidence/dual-live-device-44165508547.json`.

## Volatile ramoops runner build and signed candidate · 2026-11-18

The safe current-boot policy and compact pre-worker QR fallback were built on
the supported macOS runner in run `44165508547`, attempt 1, job `134330363445`.
All workflow steps passed: native arm64e compile/link, pinned Mu/OneSend host
checks, strict prelink and production DarwinBoot parser. Native SHA-256 is
`edfbb6e4781ab6ebec6dc631654af2970fd2a95d0a4194a486ca91ebb68ae749` and the
142,819,328-byte prelinked fileset SHA-256 is
`505dc1b352ff64eea6edc74e8b0597e301faf600fd26fa8b84e2ee79876cc3c0`.
Mapping is complete with empty missing/stubbed sets and allow-missing false.
The live archive report contains the new single-frame symbol and direct calls.

Using the retained signed bundle as the exact baseline, the standard-band
kernel is `9181c38ab67c69d760564c64bb801e0678d06e693c607faa51900486d1a117f`.
The new signed images are in `out/live-ramoops-44165508547-signed/`:
boot `a3236acc…`, vendor_boot `7f5ac5520ca6fe2592d050066559c5d7c81529c7468fc1be9271e9388ea1c427`,
and vbmeta `1ce6c6beb49d2282cc892300ca8b6b8d9a481cab012fcd24aa92cbdfc167de44`.
Independent RSA, salted hash, payload, ADT/config/vroot, boot-chain and 22
descriptor checks pass. ZIP is
`out/tb710fu-dual-live-44165508547-signed.zip`, 35,649,194 bytes, SHA-256
`28559a208b8b9a7a816650337faa333804cccacdcda9cf4f0b3e3feee163718d`.

This is still a test candidate: ramoops is volatile current-boot BSS and does
not survive reset; real worker scheduling, QR rotation, ramoops content, CDC
enumeration and hardware boot remain unverified. No device operation occurred.
Evidence: `docs/evidence/dual-live-runner-44165508547.json` and
`docs/evidence/dual-live-signed-44165508547.json`.

## Volatile current-boot ramoops selected · 2026-11-18

The user selected the safe temporary policy after the device report showed the
dual-live display stopped at `W=0 P=6 S=XNU`, the right QR contained only the
ramoops boot banner, and a button-forced reboot was needed before the scan.
The new `0015-tb710fu-ramoops-volatile-buffer.patch` removes the unreserved
physical ramoops mapping and stores the 256 KiB zone in an aligned writable
kernel BSS buffer. It preserves the current-boot snapshot format and early
capture, but intentionally cannot survive reset or serve Android pstore.
`ramoops_selftest=1` now records that limitation without resetting.

The production ramoops snapshot check passes all 10 cases under WSL GCC and the
dual-stream adapter check passes 10 groups and 15 invalid-video cases. These
are host checks only. A fresh macOS arm64e runner build is required before a
new signed image; no image or device operation has been produced for 0015.
The earlier fixed-region analysis is retained as historical evidence and marked
superseded in `docs/LOG_REGION_IN_WINDOW.md`.

The protocol now has a strict one-frame path for the pre-worker state: the XNU
side prioritizes the newest tail while retaining the early backup in memory,
and ramoops uses the same bounded still-frame path. The pinned OneSend
interoperability check passes with ASan/UBSan, including the single-frame
capacity boundary and slow-scan recovery. Once the worker runs, the existing
multi-frame early-backup/current-tail export remains active.

## Dual-live device report and memory reservation risk · 2026-11-18

The user scanned `src-da488d37-ramoops` on the device. The XNU file has a
409-byte early backup and a 512-byte current tail ending at offset 1244; its
last line is the phase 6, rank 0 startup callback. The right QR yielded the
ramoops boot banner. The display read `W=0 P=6 S=XNU` and stopped cycling.
The user forced a reboot and scanned before it stopped, not an independently
reported spontaneous reboot. Verbatim/transcribed evidence and limitations are
in `docs/evidence/dual-live-device-44149353099.json`.

The worker flag and snapshot capacity indicate the displayed snapshot was made
before the worker ran. The callback pointer's low address matches
`_vm_compressor_set_size` in this exact fileset, but the line is emitted before
the diagnostic poll and callback, not at a fault PC. It does not prove whether
the poll or callback completed. The initial right snapshot immediately follows
ramoops init, consistent with a stale boot banner. Timer/worker animation cannot
continue if startup stops before scheduling or the CPU cannot execute it.

The current 256 KiB ramoops default lies inside managed DRAM and is not proven
reserved from XNU's page allocator. The delivered ADT lacks
`darwinboot-memory-excluded`; the observed zero excluded pages is consistent
with this risk. `ml_io_map` alone does not reserve pages. Do not treat this
region as safely reserved or rely on it for persistent logs. The user selected
a compact single-frame fallback prioritizing the latest XNU tail, with the early
backup retained in memory; implementation and a safe ramoops ownership path
are being investigated. No newer hardware result is established.

## Retained signed baseline reused for the dual-live fix · 2026-11-18

The user selected `out/tb710fu-dual-live-wsl-44124706860-signed/` as the
packaging baseline after earlier outputs were deleted. All three retained
images match the historical SHA-256 records. Independent RSA checks for boot,
vendor_boot and vbmeta, boot/vendor salted hashes, vendor descriptor and boot
chain key checks passed. The packager now accepts this exact vendor/vbmeta
pair, validates its existing ADT, configuration, old kernel and root policies,
and replaces only the payload kernel. The original baseline path remains
separately supported. Fifteen of 17 focused tests passed; two tests requiring
deleted earlier fixtures were skipped.

Run 44149353099's prelinked kernel was transformed to the standard-band
derivative SHA-256 `6885af59fb414ce40c459b4144f0eb57219c18c8e5994243c4b2fb73f906d8e1`;
six spans were verified, other bytes and segments are unchanged, and the
production parser passes. The image was locally signed under the user's prior
explicit signing authorization. Independent checks confirm all three RSA
signatures, salted hashes, boot chain key, 22 unchanged other descriptors,
unchanged ADT/config/policies and actual FAT/LZMA readback of the new kernel.
No device command or hardware test occurred. The receiver still needs a reset
for a different session; runtime animation and ramoops contents remain unproven.

Signed files are in `out/live-ramoops-44149353099-signed/`; ZIP is
`out/tb710fu-dual-live-44149353099-signed.zip` (35,646,233 bytes), SHA-256
`1f23937eb7157bd731de6e83baa86beaa7ecef1b58c5911a1837ed7c475a52af`.
Every ZIP member was compared with its source. Display label is
`src-da488d37-ramoops`. Details are in
`docs/evidence/dual-live-signed-44149353099.json`.

## macOS runner resumed for dual-live fix · 2026-11-18

At the user's request, device commit
`da488d376954e5490685aa0580f71c98ecb1c2c0` was pushed to
`codex/ufs-transactional-handoff` and one `ufs-integrated` workflow was
dispatched as run `44149353099`, attempt 1, job `134273078747`. It completed
successfully on macOS: native arm64e compilation/link, both QR checks, strict
prelink and the production DarwinBoot parser passed. The native input SHA-256
is `941b61b13111842fcf129f43bbd163cf06b20df58694e0e602c949e2a7deaf73`;
the 142,819,328-byte fileset SHA-256 is
`b368b5bd8525124816d01f3f6173c488a599616490f5429fa365ce7abbb8a92c`.
Mapping is complete, missing and stubbed are empty, and allow-missing is false.
The downloaded artifact SHA-256 matches GitHub's digest. Its LiveLog integration
report binds the exact device commit and label `src-da488d37-ramoops`. Details
are recorded in `docs/evidence/dual-live-runner-44149353099.json`.

This is a prelinked build, not a signed or flashable image or a hardware boot.
The old `vendor_boot` and `vbmeta` packaging baselines are missing at their
recorded paths; a matching boot image and prior unsigned payload were located,
but the missing signed baselines have not been reconstructed or verified.
No signing or device operation was performed. No duplicate run was dispatched.

## WSL toolchain source investigation · 2026-11-18

The pinned XNU README requests a matching Xcode and, for Apple Silicon,
a matching KDK; it does not supply a Linux arm64e Apple Clang. The pinned
UniXNU Linux environment documents plain arm64 iOS 17. A public UniXNU WSL
example builds the same XNU revision as plain ARM64 for iOS and reaches a
QEMU boot boundary; it is not evidence of macOS arm64e or signed-null ABI
compatibility. A newer, unpinned framework checkout selects an arm64e target
but still pins the same LLVM revision and does not resolve the observed error.

The public swiftlang/llvm-project Apple branch at
`7ca7a2547f00e34f5ec91be776a1d0bbca74b7a9` rejects more than three
`__ptrauth` arguments in Sema. The checked `next` revision
`c1cf5082012dc490621de846c0aa112ad47af6ea` fixes
`AuthenticatesNullValues` to false. These are bounded source checks, not a
claim about every branch or Apple's private toolchains. A third-party patch
that removes the fourth argument changes null authentication behavior and is
not an established ABI-equivalent solution. Exact links and local findings
are in `docs/evidence/wsl-toolchain-investigation-20261118.json`.

The WSL installation has Debian Clang 19.1.7 and the pinned UniXNU Clang 24;
neither passed a minimal four-argument qualifier probe. Its `xcrun` is a
UniXNU shim, not Xcode. No compatible local compiler has been identified and
the existing native bootstrap failure remains the last actual build result.
No new native kernel, prelink or signed image was produced, and no runner or
device operation was started during this investigation.

## WSL native build blocked by arm64e pointer authentication · 2026-11-18

At the user's request, native source revision
`da488d376954e5490685aa0580f71c98ecb1c2c0` was built in WSL without a GitHub
runner. The first attempt on the Windows-mounted framework stopped in the
AvailabilityVersions tool because the checked-out shebang contained CRLF;
that was an environment error, not a compiler failure. No fetched cache was
edited. A separate LF checkout on the WSL Linux filesystem used the official
framework `0c1bac3d51a3009dca1708b15a5198f4b43d057a`, the same device commit,
and the official pinned Linux toolchain. It passed that point and began the
native bootstrap, including all 140 selected XNU patches.

The first actual compiler error occurred in libdispatch firehose compilation:
`sys/ucred.h` uses XNU's four-argument `__ptrauth` with
`"authenticates-null-values"`; the installed Clang 24 rejects more than three
arguments. The fourth argument concerns authentication semantics of a security
credential pointer. Removing it or disabling pointer authentication would change
the kernel's security and ABI behavior and is not accepted as a build fix.
The pinned official `linux_env` is documented for plain arm64 iOS 17 and sets
`DARWIN_TRIPLE=arm64-apple-ios17.0`, not the selected arm64e macOS profile.
No compatible local compiler has been established. Actual bootstrap log SHA-256
is `b0efbf244ba0ab4271737cc603089965f23ab7830c129734e0a0be25a2a725f8`;
the exact compiler SHA-256 is
`ff36a7a15a3f68434a41af0732e8dda147436275d1409abeb4741526d3f385dc`.
See `docs/evidence/wsl-native-arm64e-blocker-20261118.json`. No new native
kernel, prelink, signed image or hardware result was produced by this attempt.

## Dual-live field report: session timeout and hidden status · 2026-11-18

The user tested the signed `src-7f409ffb-ramoops` bundle. They report a
purple-looking band, no apparent rotation, the XNU receiver stopping near 50%,
an empty ramoops transfer, and only part of the on-screen status row visible.
The row did not reveal `showing`, `R=` state or rounds, so the hardware cause is
not established. The third pinned upstream band is explicitly magenta
(`0xffff00ff`); its physical panel color interpretation remains a hardware gate.

The sender kept each six-block live session for two rounds. At one frame per
stream per second this allowed a replacement after about 12 seconds. The pinned
OneSend receiver requires an explicit reset at a session change and rejects
frames from a new session in the old receive state. This is a strong explanation
for a transfer freezing part way through, though it does not explain a visibly
static framebuffer. On successful ramoops initialization the native code writes
a boot banner before snapshots, so a genuinely empty file points to failed
initialization/mapping, a stopped worker, stale display content or session
handling. The old status row that would separate these states was obscured.

The source fix now gives each channel 16 rounds before replacing a session and
polls one frame every 250ms, advancing each stream at about two frames/second.
A maximum six-block session remains stable for about 48 seconds once the worker
runs. It moves the status row below both QR images and reports worker state, poll
count, active stream, per-stream state/bytes/rounds/block count, early-backup
state and build label. The original standard color bands are unchanged.

Host checks pass: display and adapter ASan/UBSan, 10 adapter groups and 15 bad
video cases, 16-round protocol replacement, the pinned OneSend receiver,
10 ramoops snapshot cases, both DEBUG configurations of the real Mu encoder,
independent ZXing recovery, and all 11 package verifier tests. Evidence is
`docs/evidence/dual-live-runtime-fix-host-20261118.json`. New native macOS
compilation/link, strict WSL prelink, signed image and hardware retest are
pending. The pinned receiver still needs a reset between each stream/session;
this change gives the camera longer to collect a file but does not add automatic
receiver session switching.

## Dual-live WSL continuation completed; signed test bundle ready · 2026-11-18

The requested dual-live candidate is complete at the host/image stage. Exact
native source remains `7f409ffbc282c918e9ec2a5996c6e413b7a9682a`, compiled on
the macOS runner in run 44124706860 attempt 2, job 134197243714. Its overall
failure and original native-only records are preserved. Host checker revision
`03c5da1ede9dbdf7f96e28293abd838e01d3bee3` changes no native target inputs.
The user requested WSL instead of waiting; queued run 44131818618 was cancelled
before acquiring any runner. No duplicate native build was performed locally.

The unchanged official XnuPrelink `440589b0c31625f17edf363e24b3c82c51c70bb9`
was independently checked against all 25 Git blobs, including 16 Python files,
then executed in WSL with the pinned original kernelcache. Native input SHA-256
is `da18f5aac95b488b838cf6fd71ce4a58432d4f2da023ea433a9e744a1ada08b0`.
The new 142,819,328-byte strict fileset SHA-256 is
`adc7b6d867d0ec161dd757fc5c3c5087e822489b2d489c29943cf9083155f2ba`.
Mapping is complete, conflicts/missing/stubbed are zero, allow-missing is false,
and the actual production DarwinBoot parser passes. WSL Mu DEBUG 0/1 and
independent ZXing recovery pass, bound to the current host checker and the
same native production sources. No XNU source was recompiled in WSL.

The separate six-span standard-band transform preserves all other kernel
bytes and segments; final kernel SHA-256 is
`27669f1fd468b8422837652174483e8744f01379e9e34c69e8e8c65a53343c41`.
The 20-input manifest SHA-256 is
`a71501afc1275c6edd2728b89ae477313b942329f68350017ae3e0dca3c9f37a`.
Strict source/archive/integration, parser, policy, loader, FAT/LZMA readbacks
and separately authorized local AVB signing all pass. Independent three-image
RSA verification, salted hashes, unchanged payload and 22 unrelated descriptors,
header/public key/rollback checks pass. The matching signed images are in
`out/live-ramoops-44124706860-signed/`; source/run/local-prelink provenance is
included in the ZIP, and no key or raw IPSW member is included.

ZIP: `out/tb710fu-dual-live-wsl-44124706860-signed.zip`, 35,661,040 bytes,
SHA-256 `d1fa85940a3fa910bce83639cc33ad6a0c71ce2c21936a46ba6178702138afce`.
Signed vendor_boot SHA-256 is
`a07f25494968562b8d34a26bf3591b267956959a17850dff68d38e9f440790f2`;
matching vbmeta is `3fd43a5a7adc0275eb8d241b94f1c2520b6b630ae912c3eff4a3869d76228e04`.
The existing signed boot SHA-256 remains
`a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`.
Evidence: `docs/evidence/dual-live-wsl-{prelink,image,signed}-44124706860.json`.

Display label: `src-7f409ffb-ramoops`. LEFT exports independent live XNU debug
text with its immutable early backup in every file; RIGHT exports live current-
boot ramoops. Standard green/blue/magenta bands remain, with no checkpoint replay
waits. Real display, animation, log contents/retention, CDC enumeration and USB
bytes, UFS/root mount and macOS userspace still require hardware evidence.
No device command, flash, reboot, partition or filesystem operation occurred.

## User switches queued remainder to WSL · 2026-11-18

The user requested WSL instead of waiting for the hosted runner. The single
03c5da1ede9dbdf7f96e28293abd838e01d3bee3 run 44131818618 was cancelled once
while queued at 17:06:45Z; the final remote state is cancelled, runner_id 0,
empty runner name and no steps. No compilation or prelink occurred in that run.

The WSL continuation reuses the exact macOS-compiled native kernel from
44124706860 attempt 2 at 7f409ffbc282c918e9ec2a5996c6e413b7a9682a, preserving
its original native-only record and overall failed CI result. Its failure was
the later host namespace test, not native compilation. The 03c5da1e revision
changes only that host checker and status/evidence; target sources are identical.
The display label therefore remains src-7f409ffb-ramoops. Local WSL Mu checks
for DEBUG 0 and 1 and independent ZXing reconstruction pass. The original
kernelcache is independently rehashed to the exact pinned d1003b51…9fe9.
Official strict local prelink and packaging are next; no local prelink success
or new signed dual-live image is established by this entry.

## Dual-live native build passes; host Mach-O name check corrected · 2026-11-18

Run 44124706860 attempt 2, job 134197243714, at exact device source
`7f409ffbc282c918e9ec2a5996c6e413b7a9682a` passed actual macOS arm64e
native compilation, final link and all CDC/storage/LiveLog integration gates.
The native kernel is 25,406,216 bytes, SHA-256
`da18f5aac95b488b838cf6fd71ce4a58432d4f2da023ea433a9e744a1ada08b0`.
The exact six-object LiveLog archive SHA-256 is
`9fd004e41c234b87f030064d5059eca67c96b081bba00eb7dc23520215ad973f`.

The next host Mu check stopped at `____asan_globals_registered`: its namespace
checker omitted Darwin's one ABI-leading underscore. This is a host-checker
false rejection, not a failed native link. The actual Apple Clang object has
one ASan common registration variable and nine private `_tb_live_qr_*` code
definitions. Prelink was skipped. The artifact digest and exact staged result
are bound in `docs/evidence/dual-live-native-44124706860-attempt2.json`.

The host checker now normalizes that one underscore only for actual thin64
Mach-O objects, and permits only the observed ASan registration and private
ODR metadata names. Ordinary globals and fake ASan names remain rejected.
All three real Mach-O ASan QR/Mu objects, the actual Apple Clang object,
GCC ASan/UBSan repeated encoding and independent ZXing recovery pass. See
`docs/evidence/macho-qr-namespace-fix-20261118.json`. Native sources, pinned
Mu bytes and target compile flags are unchanged by this fix. One new bounded
source build follows; no dual-live prelinked image or signing is established.

## User selects two live log sources with retained early backup · 2026-11-18

The latest user choice supersedes the fixed-left display: LEFT now exports
an independent 64 KiB XNU character capture ring, while RIGHT exports the actual
current-boot ramoops tail. Both channels update. Before machine_startup, one
XNU tail of at most 512 bytes is retained once; each later LEFT file includes
that unchanged backup and its own current tail of at most 3072 bytes, with
explicit metadata and raw-section lengths. The complete LEFT body is bounded
by 4096 bytes. RIGHT remains raw ramoops text, at most 4096 bytes. Before the
worker actually runs each current tail is at most 512 bytes. Freeze busy/empty
states are explicit; later bytes are never relabelled as pre-machine_startup.

Each channel keeps its file immutable for at least two rounds. A poll renders
at most one frame and alternates channels with no checkpoint wait. Buffer
overwrite still loses older current bytes under sustained output, but cannot
alter the separately retained early backup. Unlike the former fixed single
frame, a stopped multi-frame transfer cannot be assumed recoverable from one
photo. The original CDC implementation and its unverified hardware gates remain.

Run 44118339552 at `46e1cac8590c47548f1241b488186ec68dd8dad2` passed the
header-export compilation boundary and built archives 01–06. Archive 07 first
failed because XNU's object-like `DEBUG 0` conflicted with EDK2's function-like
DEBUG service in LiveQrCompat.h. The earlier bootstrap undefined-provider link
is expected, not this run's compiler root cause. Full provenance is in
`docs/evidence/live-ramoops-ci-44118339552.json`. The QR-only compatibility
header now explicitly undefines DEBUG before its private EDK2 macro. Native
XNU/LiveXnu flags and pinned Mu source bytes are unchanged.

Current host checks cover independent capture wrap/busy/drop, source-specific
OneSend sessions, explicit exported backup/current sections and actual pinned
Mu encoding with inherited DEBUG 0 and 1. Independent ZXing recovers all 26
frames per configuration and the exact XNU backup/current file. Portable
arm64e cross compilation passes the three QR/Mu objects in both configurations;
this does not replace the next supported macOS native/prelink build. The
production adapter's 10 groups plus 15 invalid-video cases and 18 extracted
production-console capture cases pass ASan/UBSan. All 140 selected patches
check and apply from pristine XNU; the first 139 hashes are unchanged. Package
verification passes 11 tests including malformed contracts and inlined hook
paths. The exact current sources and 11 passing reports are bound in
`docs/evidence/dual-live-log-host-checks-20261118.json`. One new bounded native
run follows this diagnosed source revision. No image or signing has yet been
produced from the dual-live source.

Dual-live source is pushed at `7f409ffbc282c918e9ec2a5996c6e413b7a9682a`.
The only next bounded target is run 44124706860, `ufs-integrated`. Attempt 1,
job 134185221494, never obtained a hosted macOS arm64 runner: steps and
artifacts were empty, and GitHub explicitly reported capacity constraints.
This is not a compiler result. The exact record is
`docs/evidence/dual-live-runner-capacity-44124706860-attempt1.json`.
After diagnosing that infrastructure failure, one rerun of the same source
started attempt 2, job 134197243714. Native/prelink success remains pending.
The intended display label is `src-7f409ffb-ramoops`.

## Stage 89 observed; native fixed/live ramoops QR prepared · 2026-11-17

The user supplied `src-503599a0-plat-qr` at `0x59`, machine_startup entry,
and reports that display stays there. This is the final old instrumented point,
not a fault PC or proof of a stall inside machine_startup. Actual BSD/CDC
execution remains unobserved. The verbatim evidence is
`docs/evidence/usb-cdc-stage89-device-20261117.json`.

The requested replacement is now source-implemented: independent native
LiveLog archive, current-boot-only consistent ramoops snapshots, a fixed
single-block pre-machine_startup tail of at most 512 bytes and a separate
live tail of at most 4096 bytes. Each live file remains immutable for at least
two complete transmission rounds. The fixed file/bitmap never follows ring
overwrites. No checkpoint replay waits are retained in this mode. Native
startup polls one bounded frame; an EARLY_BOOT worker continues after it
actually runs. The framebuffer is independently mapped permanently, with no
shared old reporter stack or translation-control writes. The standard upstream
green/blue/magenta three 1/16-byte bands are restored and preserved.

Patches 0013 and 0014 separate snapshot semantics from XNU integration. The
graphical console's disableConsoleOutput early-return paths now retain the
memory taps; otherwise later BSD/kprintf text would be lost before _cnputc.
Ten actual extracted production-function capture cases pass with ASan/UBSan.
The actual pinned Mu encoder and OneSend receiver remain unchanged. Sources,
target commands, dependencies, font, fixed upstream bytes, actual strong hook
branches and final force-load are independently bound by the new recipe and
integration/package verifiers. See `docs/LIVE_RAMOOPS.md` for limits and usage.

Production ramoops, real sender/receiver, display bounds and production-adapter
control-flow host checks pass with ASan/UBSan. Actual Mu repeated encoding and
independent ZXing recover all 14 test frames and the fixed512/live4096 bytes.
The separate standard-band-only transform passes six-span reconstruction,
mutation rejection and AArch64 instruction models. Host evidence is
`docs/evidence/live-log-host-checks-20261117.json`.

Source commit `eb1f2bc70f08d410dbcb7e182372735d38bb018a` is pushed. The only
new bounded kernel target, run 44112832250 (`ufs-integrated`, job
134142786467), ended during bootstrap compilation. The first actual error is
`pexpert/arm/pe_kprintf.c:3:10`: `kern/tb710fu_ramoops.h` was not found, because
the new private header was not added to `osfmk/kern/Makefile` XNU_ONLY_EXPORTS.
The later missing link.filelist message is secondary; corecrypto itself reports
529 C sources and zero failures. Prelink did not start. The complete logs,
artifact identity/digest and exact first failure are bound in
`docs/evidence/live-ramoops-ci-44112832250.json`. The device-only header export
fix adds both real headers to XNU_ONLY_EXPORTS using the official existing
mechanism. A pristine reconstruction of all 140 selected patches passes, with
the first 139 hashes unchanged. The old run 44088845841 does not compile
these changes. No new image, local signing or device operation has yet
occurred.

The diagnosed fix is pushed at `46e1cac8590c47548f1241b488186ec68dd8dad2`.
Its only new bounded kernel run is 44118339552, job 134162499711,
`ufs-integrated`, now in progress. Native/prelink success is not yet established;
the intended display label is `src-46e1cac8-ramoops`. Patch-application proof is
`docs/evidence/live-log-header-export-fix-20261117.json`.
Real scanout, scheduling, ramoops contents/retention and CDC enumeration are
still hardware gates. A CPU halted with interrupts masked cannot animate QR;
the already drawn fixed single-frame code remains scannable.

## User reaches stage 56, no CDC port; extended signed platform trace · 2026-11-17

The user supplied a `src-503599a0-cdc-qr` live report at stage `0x38` (56),
`tb710fu_ramoops_init` returned, and reported no port. This confirms QR recovery
and reaching that early checkpoint. It is the old reporter's last observation,
not a fault PC or proof that execution stopped there. ARG1 is not an established
error code. No partition hash readback binds exact device bytes. BSD/CDC startup,
hardware guard results and USB bytes remain unobserved. The verbatim report is
`docs/evidence/usb-cdc-stage56-device-20261117.json`.

Read-only source/actual-machine-code and packaged-ADT audits are recorded in
`docs/evidence/usb-cdc-stage56-{path,adoption}-audit.json`. The real CDC BL is
later in `bsd_init`, after IOKitInitializeTime and before vfsinit. The current
ADT selects no serial UART; boot_progress is disabled. Neither finding identifies
the no-port cause. The trace extension stays in arm_init before scheduled
bootstrap-thread creation/switching; actual fiq_context_init asserts masked
DAIF. The built-in empty pe_init_fiq does not establish a missing GIC provider,
because the separate official ArmCpu archive is linked.

Tool commit `608c701ab5690f32bc3a36a571a7cba6fc1984c1` adds 17 original-call
wrappers, stages 57–88 for entry/return and 89 for machine_startup entry only.
It does not call the shared-stack/GOP reporter from BSD or a scheduled worker.
The validator independently checks actual parent bounds and BL occurrence,
all 52 prefix spans, 45 trace sites, three compact checkpoints and appended
RX/RW-NX segments. All 13 CDC/storage functions and 7,656 code bytes remain
unchanged. Sixteen mutation tests, all 45 instruction-model wrappers, host QR
frames for all new stages and actual AArch64 live rendering at 61/89 pass.
Packaging tests pass 14 cases with one historical fixture skipped.

The original native/prelink run remains 44088845841 at device c0154c9d5e7048f7896614e1ca8d65be7d4d0e10;
no new native build or CI run occurred. The separate 143,687,680-byte derivative
SHA-256 is `23331f1d379f52c155dd4e479f72cf6786342d8d4ae1f87313f2b665a04aef19`,
label `src-503599a0-plat-qr`. The 17-input manifest SHA-256 is
`5b91185df389ebec90751bcb24b698a903277ba2999dee8e4d5b63d411dce537`.
The final derivative passes the production DarwinBoot parser and FAT/LZMA
readback. Unsigned vendor SHA-256 is
`24132230a6771770d78b6065f0b157992f8f3937d51854ea9c22a571f4761aae`.

Under the continuing explicit local-signing authorization, the new signed set
is `out/usb-cdc-platform-44088845841-signed/`. The matching vendor_boot SHA-256 is
`42f769f62a333449a2ef03ed39597a75564a0e3d6ce166cb5a38f4f729e2d92f`;
vbmeta is `5e7dd99c69a62ce6ce7e73aeb80d060ae996792a41b51301b1e8af8b4fcec87f`.
The existing boot image is unchanged, SHA-256
`a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`.
Independent RSA, salted hashes, unchanged payload, four FAT files, complete
kernel and 22 unrelated descriptors/header/key/rollback checks pass. The ZIP
`out/tb710fu-usb-cdc-platform-44088845841-signed.zip` is 35,559,274 bytes,
SHA-256 `5bbd067261f417a44826b43077404f6db92642147a439e9e21771993cbcba9e7`.
Exact proof/image/signing records are `docs/evidence/usb-cdc-platform-{transform,image,signed}-44088845841.json`.

Normal execution continues after each two-second replay; the new stages add
up to 66 seconds. No hardware guard was bypassed, no device command was run,
and no device flash/reboot/storage operation occurred. This candidate extends
localization, without establishing a no-port fix, actual enumeration/log bytes,
hardware root mount or macOS userspace. The next hardware result must retain
the new label and final complete QR text. Stage 89 alone still does not prove
BSD/CDC worker execution. Earlier entries preserve their historical scope.

## Missing QR reporter restored on the CDC kernel; signed replacement · 2026-11-17

The user reports a white boot band and disappearance of the earlier QR after
testing the CDC signed candidate. No device partition readback binds that
observation to exact bytes, and it does not establish the cause of the stall.
The referenced `codex/diag-fileset-header-fix` ref was rechecked through GitHub:
`c87876e96c69a616bc6be54175d70c0290f2fc2c` is already an ancestor. Its
0006–0009 patches are byte-identical to this branch and the actual run
44088845841 XNU input hashes. The missing QR is a packaging regression:
the original signed CDC kernel has no `__KDIAG`/`__KDATRW`, because its
independent reporter post-link step was omitted. The header-mapping fix alone
cannot create those segments. See `docs/evidence/usb-cdc-qr-regression-44088845841.json`.

The existing reporter now explicitly accepts the exact CDC prelink input
`503599a026ee362e9523aba1808f8af01601ae1323c7404fe7ecd1ed9a5690e2`.
It produces a separate 143,687,680-byte derivative, SHA-256
`2410ec0545996500af0e8c051f4ce33f9941685a6cc73c63b082a51222da0b7f`,
labelled `src-503599a0-cdc-qr`. Independent delta validation reconstructs all
35 instruction/cave spans, 28 original call wrappers, three compact entry/MMU
checkpoints and the two appended RX/RW-NX segments. All 13 CDC/storage runtime
function definitions and 7,656 bytes of their complete code remain unchanged.
The default path preserves original execution and adds no stopping checkpoint.
Twelve mutation tests pass; four extra real-input mutations are rejected.

The packager retains the original 15 prelink/integration inputs and optionally
requires both diagnostic inputs, for 17 total. Original CI reports/hashes are
never rewritten. The actual derivative passes the production DarwinBoot parser,
LZMA roundtrip and all four FAT readbacks. Packaging tests pass 14 cases with
one historical fixture omitted. Exact transform and unsigned-image evidence is
in `docs/evidence/usb-cdc-qr-{transform,image}-44088845841.json`.

Under the existing explicit local-signing authorization, the corrected set is
`out/usb-cdc-qr-44088845841-signed/`. It retains the same recorded `boot.img`
SHA-256 `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`.
The new 100,663,296-byte `vendor_boot.img` SHA-256 is
`a69c64ce476c6c5dc44b8fde29c9569e5ed5468584ee5a86d21c2832787d5680`;
the 8,000-byte `vbmeta.img` SHA-256 is
`f0dfcae10f915977fc768f9fa59de4187299429ff6124fe7dd00ef2be52dbbcd`.
Independent RSA, salted hashes, boot chain key, preserved unsigned payload and
22 unrelated descriptors/header/key/rollback checks pass. The signed ZIP is
`out/tb710fu-usb-cdc-qr-44088845841-signed.zip`, 35,557,611 B, SHA-256
`0a73220da6fa8327029d47048521a7f686e7661ccc101114ef905f2f18768d82`.
See `docs/evidence/usb-cdc-qr-signed-44088845841.json`.

No new native build was needed; this is an audited diagnostic derivative of
the successful CDC/UFS input. No device command, reboot, flash or storage
operation occurred. Actual QR display, CDC enumeration/log bytes and hardware
root boot remain unverified. Restoring diagnostics does not establish a fix
for the reported white-band stall. Earlier candidates remain evidence only.

## CDC test bundle locally signed and independently verified · 2026-11-17

The user explicitly requested local signing for hardware testing after the
unsigned delivery. This authorization supersedes the earlier working-port
condition for this signing step; it does not authorize device commands.
The existing signer and avbtool were used unchanged with the existing local
key. No key material is published or committed.

The signed set is `out/usb-cdc-44088845841-signed/`:

| File | Bytes | SHA-256 |
|---|---:|---|
| `boot.img` | 100,663,296 | `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83` |
| `vendor_boot.img` | 100,663,296 | `80fd313b2c349bb89fa3790dc6caf4fd577ad09f0767ec79c2de7c71f796b279` |
| `vbmeta.img` | 8,000 | `e8a4d1da9db5d60dedeade8ab3d12ae12f45d84be8b7bb036bcba101d2865323` |

Independent public-key RSA-4096/PKCS#1/SHA-256 recomputation passes for all
three AVB structures. Salted boot/vendor hashes, unchanged unsigned payload,
all four FAT files and decompressed kernel match. The top-level vbmeta changes
only the vendor_boot hash descriptor: its other 22 raw descriptors, complete
header, public key and rollback/flags remain unchanged. The boot chain key
matches the included existing signed handoff/LZMA loader; no firmware was rebuilt.

The delivery ZIP `out/tb710fu-usb-cdc-44088845841-signed.zip` is 35,549,927 B,
SHA-256 `a392526ed17436a02b1043f278cbf697d19a9c82da35ac966dd072db86b43df5`.
All ZIP members were read back and hashed; a Chinese README, SHA256SUMS and
signing/verification reports are included. Public evidence is
`docs/evidence/usb-cdc-signed-44088845841.json`.

This completes local signing for testing. No flash, reboot, partition or device
filesystem operation occurred. Actual TB710FU CDC enumeration and USB log
capture remain unverified, as do hardware UFS/root mount and macOS userspace.
The firmware-adoption, BSD/scheduler and active-DMA reset limits below remain.

Entries below retain their original stage and signing scope.

## Native USB CDC linked, prelinked and unsigned image verified · 2026-11-17

Run [44088845841](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44088845841)
at device `c0154c9d5e7048f7896614e1ca8d65be7d4d0e10`, job
`134058374678`, passed the supported macOS arm64e build, actual CDC/storage
integration checks, strict prelink and production DarwinBoot parser. All three
CDC host checks passed under ASan/UBSan on the runner. Artifact `14330634249`
is 38,483,204 B; its independently verified ZIP SHA-256 is
`346ec4a926a72bf6bc52768aaa50546858f966c629db20a820859ea8deef8369`.

The native kernel SHA-256 is
`3daf19507bdf3d7ca5943f165cde4611e710b58131e4dcb956fef8f635628215`.
The prelinked fileset is 142,802,944 B, SHA-256
`503599a026ee362e9523aba1808f8af01601ae1323c7404fe7ecd1ed9a5690e2`, with
79 executable entries, 86 closure components, no missing references or stubs.
The six-object CDC archive is 192,872 B, SHA-256
`97e3ff396c881f4aae0c3f6c33edc7ee69776d52c861fc1a937c50780ffce8df`.
Its bound integration report proves the actual BSD strong reference, final
transport definitions and force-load input. Archive-creation reports precede
the final link and must not be mistaken for its outcome. Local verification
matched all 12 source/header hashes against the exact Git objects, all six
command/dependency records and every archive member. The actual BSD object and
compile record are hashed by CI but not retained as separate artifact members.
See `docs/evidence/usb-cdc-integrated-44088845841.json` and
`docs/evidence/usb-cdc-local-verification-44088845841.json`.

The new storage archive SHA-256 is
`a87e8b55568242ccba39013ab8a1d0b439f5d4dc4580e2a61f1820ecabd628e5`.
Its repeated static ABI audit against the unchanged exact-profile historical
reference passes 326 PAC slots, 26 named methods, class size `0x90` and four
binary consumer dispatch sites. See `docs/evidence/storage-abi-44088845841.json`
and its separate provenance file; this is static evidence, not executed UFS.

The packager at the same device commit verified all 15 inputs and produced
`out/usb-cdc-44088845841/vendor_boot.unsigned.img`, 37,859,328 B, SHA-256
`49b216089db0cec46772cbba068178fb82e12d86af3ba0c5e925cf0a26ed416c`.
Manifest SHA-256 is
`f32068c96c0a635c11ffc088cd77df6463249ffebe54b4fd5bf2242cf64c98b2`.
The actual six CDC objects and 12 strong transport code definitions, three
existing RSA policy signatures, ADT pairing, recorded loader lineage,
production parser, LZMA roundtrip, four FAT readbacks and unchanged outer
vendor bytes all passed. Boot arguments are explicitly
`-v serial=3 native-source-storage=1 native-ufs-root=1 tb710fu-cdc=1`.
See `docs/evidence/usb-cdc-image-44088845841.json`.

This stage is implemented, target-compiled, prelinked and unsigned-packaged.
No TB710FU COM enumeration or captured USB bytes are established. Live
firmware role/clock/PHY/SMMU/DMA checks may refuse takeover; the worker requires
BSD startup and scheduling, so earlier output and final panic text are not
guaranteed. Active-DMA reset/disconnect/reconfiguration recovery is unsupported.
The fileset's `loadable=false` and runtime fields remain unqualified. Hardware
boot, UFS reads, policy handoff, root mount and macOS userspace remain separate
gates. No private key was read, no signing or device operation occurred, and
no firmware was rebuilt. The user's working-port condition for a signed image
is not yet met.

Entries below retain the stage and limitations known at their historical time.

## Native USB CDC transport implemented; target validation pending · 2026-11-17

The independent DWC3 core and XNU polling adapter are now implemented. Patch
0012 starts the opt-in worker before vfsinit; runtime requires `tb710fu-cdc=1`.
EP0 enumeration, line coding/DTR/SerialState, log IN, discard-only OUT, normal
SETUP-only bus reset and completion-before-consume are covered by the actual
production core host fixture with separate CPU/device DMA views. Protocol,
RAM ring/tap, hardware predicates and core tests passed ASan/UBSan locally.
The adapter checks live clocks/PHY controls, device role and a unique identity
SID40 route, then validates permanent contiguous DMA and cache geometry.
Source/hash provenance is in `docs/evidence/usb-cdc-sources.json`.

The device recipe now selects all six real sources. A separate CI gate will
check actual BSD compile/import, final strong definitions and force-load input.
The supported macOS native/prelink target has not yet validated these new inputs.
Run 44084514457 at `fbdd30ab98d16115c16ac7319b861e05af456623` passed the
macOS host tests but stopped at patch 0012 application, before any compiler
invocation. Its leading context was wrong. The corrected patch is actually
applied in an independent Git fixture with verified changed bytes and startup
order; see `docs/evidence/usb-cdc-startup-patch-check.json`. The initial nested
fixture had silently skipped patch paths, so its exit code was insufficient.
Run 44085996576 at `6f4f1ddd7aa3346be94183c3592c888874cadec9` then applied
the complete series and compiled/checked the five portable CDC objects. It
stopped compiling `CdcXnu.c`: BSD `sys/param.h`/`sys/systm.h` conflicted with
the replayed osfmk/MACH_KERNEL_PRIVATE declarations. The adapter now uses
the same `kern/misc_protos.h` declaration boundary as osfmk/startup.c.
The earlier bootstrap undefined providers are expected before archives;
the actual failing stage was archive-06/CdcXnu compilation. Neither final
native integration nor prelink passed in this attempt.
Queued run 44088500248 at `81a72b61c687845ee9980ea056e54eced075e9f0`
was cancelled before runner steps when a local check of a real Darwin archive
found that the new CDC verifier counted `__.SYMDEF SORTED` as an extra object.
The verifier now filters that linker metadata while checking every real object.
No actual TB710FU CDC enumeration or captured USB log is established. Unsupported
active-DMA reset/disconnect stops the session and retains DMA. The scheduled
worker does not promise final panic text after scheduling stops. No signing or
device operation occurred; the older unsigned image below contains no new
transport or enabling boot argument.

## Integrated UFS fileset and unsigned payload verified · 2026-11-17

Run [44075573981](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44075573981)
at device `9f01899957060ffbd81a49c793438ba152760eb9` passed native compilation,
the actual storage integration verifier, strict prelink and the production
DarwinBoot parser. Artifact `14324417743` is 38,354,358 B; its independently
downloaded ZIP SHA-256 is
`df2802dc432b0290e6b8db0c2a1ef27db8fc6469de2b9fb08351e9fd1d9b8016`.

The integrated native input SHA-256 is
`7a1bba0855ca9ea8d9ddc3e3f937209d5e874b1082415c24ac981c26e0231a45`.
The prelinked MH_FILESET is 142,802,944 B, SHA-256
`9e2be0f788f84fd6c582449afaace7784085e460aff5e959a8cfc59d43b886d3`, with
79 executable fileset entries, 86 closure components, no missing references
and no stubs. Its actual storage archive is 1,444,104 B, SHA-256
`e0da7e494ccd11908c3c861ec4a15765e977c45345d02465e3e89dc47142fc80`.
These are new integrated-run hashes, distinct from the earlier standalone run.
The compile report's `linked_into_kernel=false` describes archive creation;
the separate bound integration report proves its subsequent actual link.

Local checks independently matched all eight archive members, arm64e object
types, external late constructors, absence of eager registration/boot-observer
imports, bound report hashes and a single strong startup definition inside
the prelinked native image. CI also checked the actual BSD compile record,
strong BSD reference and saved archive force-load input. The BSD object and
compile record are hashed in CI but not separately retained in this artifact.
See `docs/evidence/ufs-integrated-44075573981.json`.
The new late NativeUfs object passes the fixed exact-profile static ABI audit:
326 PAC slots, 26 named APIs, class size `0x90` and four consumer dispatches.
The retained run 43844823907 ABI-reference fileset is unchanged. See
`docs/evidence/storage-abi-44075573981.json` and its exact provenance file.

The device-owned packager at `abc8503cb68004b4fe14f687a084a97445aab2b6`
then successfully produced
`out/source-storage-44075573981/vendor_boot.unsigned.img`, 37,859,328 B,
SHA-256 `32ed17f15fcf235183f141549b3ef07dc0b1fe4768a10faba8c5c3b197c86807`.
It uses the exact downloaded fileset, three-policy bundle, ADT-correct baseline
and recorded existing LZMA/handoff loader. All twelve input hashes, three real
RSA-3072 signatures, production parser, LZMA roundtrip, FAT geometry/four-file
readback and unchanged bytes outside the vendor ramdisk passed. The sole ADT
addition is zero16 `RootPolicies`; existing identity, UUIDs and seed are preserved.
Boot arguments are explicitly
`-v serial=3 native-source-storage=1 native-ufs-root=1`.
See `docs/evidence/source-storage-image-44075573981.json` and
`docs/SOURCE_STORAGE_PACKAGING.md` for exact inputs and limits.

This is an unsigned candidate, not a newly signed flashable pair. No private key
was read, no signing or device operation occurred, and no new firmware was built.
The built source is XNU `12377.121.6`; Apple's original `26C5035r` KernelCache
uses `13432.60.57`. Firmware policy handoff, UFS DMA takeover/reads, root mount,
hardware boot and userspace remain unverified. USB has a target-compiled RAM
log tap, but still lacks the DWC3 transport/DMA/drain implementation and actual
CDC enumeration. Next implement and target-build that USB transport; hardware
storage and USB acceptance require their own device evidence. The user's
conditional request for a signed image after a working port is not yet met.

Entries below retain the stage and limitations known at their historical time.

## Complete archive passed; native integration selected · 2026-11-17

Run [44071685590](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44071685590)
at device `958a893b368befc145da5aad0c348889e7b31d6d` passed the complete
standalone source-storage build. Artifact `14323717445` is 8,970,853 B,
SHA-256 `4a59e9babc100c99b3df0b6ed7745605a6bad76c7efd56afb9f9eef30343cb74`.
Its eight-object archive is 1,444,104 B, SHA-256
`3d1842e4113d53da81d6e4ce208a4ec136c2dc95a31c646e04b4a27eb1bd2588`.
The downloaded ZIP, member hashes, external late constructors and absence of
eager class registration/boot-observer imports were independently checked.
The 1,712-byte RootPolicies bundle SHA-256 is
`49da45a7bbc6360f50bb3efabae7dc0842513b701bbb1d297cd3bd333923cb68`.
The late NativeUfs object also passes the same 326-slot/26-method/class-size
and four-consumer static ABI audit. See
`docs/evidence/ufs-bridges-44071685590.json` and
`docs/evidence/storage-abi-44071685590.json`.

The device now selects the reviewed integration inputs for the next bounded
`ufs-integrated` native/prelink build. `CONFIG_XNU_SOURCE_STORAGE=y`, while
`CONFIG_XNU_NATIVE_UFS=n` deliberately keeps automatic personalities absent.
An explicitly force-loaded archive and conditional strong `bsd_init` call
after `vfsinit()` replace that automatic startup route. The unique device
wrapper invokes the original device recipe using `UNIXNU_DEVICE`; it neither
replaces an official script nor fetches source during compilation. When the
archive changes, it invalidates only the verified generated actual LD output,
because the supported extra-archive flag alone lacks an incremental dependency.

The interface gitlinks are initialized before native compilation. A new verifier
will require the BSD hook's compile macro and strong reference, exactly one
final startup definition, the real storage/crypto symbols, archive/member
hashes and the actual saved `linkarchives` input. The successful standalone
run above does not establish these new native-link/prelink gates. Owner-root
audit/trust, Data/RAM, GPU, wireless and boot diagnostics remain off.
No new IMG has been signed, flashed or booted; USB CDC enumeration and the
runtime storage/root mount remain unqualified.

## Transactional UFS target compile and source-storage bridge · 2026-11-17

Historical entry before the complete archive result and integration selection above.

Run [44068063538](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44068063538)
at device commit `bf0be756b550558cf5a4097314c0bd1d703d1a89` completed
successfully. Both patched production host tests and the macOS 27 arm64e
NativeUfs object compile passed. Artifact `14322470471` is 8,902,951 B,
SHA-256 `bff7a9402c9a8726dc4a3b6360c7e04a54d58d8ef7bb7fdacfd90b11e453324b`.
The independently downloaded archive is 595,544 B, SHA-256
`bec56cbe2f42e76a0d162183f861da78d5a6336dcd78f36c645137125e6b561d`;
NativeUfs.o is `5ac860852c2f62b8fd1def8bfc6524d72cc4443dda4b067f9b11b04b88e81adb`.
The separate native-only kernel is 25,171,504 B, SHA-256
`d49c59876a11e3d3f8d46f0828dce6b401dec61bac3bac60931e2aa283ec4343`.
The UFS archive is not linked into that kernel; no prelink or hardware test was
performed. `docs/evidence/ufs-source-44068063538.json` records the exact inputs.

The device repository now supplies `package/SourceStorage.cpp`,
`SourceRootPolicies.cpp` and `SourceRootBinding.cpp`, adapted from exact
`UniXNU-sheng@1108068537d5396edb2c63b2f8c6f5b97b0a4bbc` blobs. The policy
bridge copies and protects the firmware handoff, authenticates all policies
before publishing, and releases its allocation on failure. Real RSA-3072
OS/SystemOS/App headers, a full App manifest and malformed bundle tests pass
on the host. The media bridge binds a unique BSD device and UUID through a
retained provider chain to the real NativeUfs authentication routine.

`scripts/build-tb710fu-source-storage.py` is an explicit device-owned minimal
adaptation of the pinned official source-storage recipe, not a replacement of
the fetched script. It compiles the public deferred base, NativeUfs, all three
bridges, the real VerifiedRoot core/XNU crypto adapter and the independent
public owner anchor into a separate archive. Optional RAM/Data storage,
benchmark, file logging, process diagnostics and wireless providers are omitted
coherently. Patch 0003 disables both declaration and call of the optional boot
observer, so no empty diagnostic/security provider is supplied. The new
`ufs-bridges` workflow stage is the next bounded target compile; its result is
not covered by the successful `bf0be75` run above.

A retained target fileset has been found locally, correcting the earlier
limited search. It records the exact profile KernelCache input
`d1003b5143ffd88439b4d45b6017f587171cfe6855660bed05370a2ea5db9fe9` and
contains IOStorageFamily/APFS. The reproducible static dispatch/PAC audit matches
all 326 slots, 26 block API identities/order, class size `0x90`, and four actual
binary consumer call sites; see `docs/SOURCE_STORAGE_ABI.md`. This establishes
static layout compatibility only. No startup hook is selected, both storage configuration switches
remain off, and the archive/link/prelink/runtime gates stay separate. No new
IMG has been packaged, signed or flashed.

Packaging audit correction: `tools/package_kernel_diagnostic.py` already
accepts the ADT-correct `out/kernel-log-region/vendor_boot.img` baseline,
SHA-256 `15846226236b56bd3abb104ae57f45e23307f3ea52321fd03ed153fd2e4e0718`.
Its absence from the allowlist was an earlier mistaken conclusion. The actual
remaining packaging inputs are an integrated/prelinked UFS kernel, a matching
RootPolicies sidecar and ADT handoff, explicit UFS boot arguments, and verified
loader provenance. Baseline allowlisting is not the blocker.

## UFS integration and image gate audit · 2026-11-17

Historical audit before run 44068063538; the current result is above.

The first board-specific UFS transaction patch is now in
`patches/ufs/0002-transactional-handoff.patch`, based on pinned
`ShengUfs@99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0`. It plans and snapshots
the apps-SMMU and firmware queue before writes, stops the idle request queue,
configures the stream transactionally, then installs and starts the new request
list. Failed configuration restores the saved SMMU state and original queue;
failed replacement startup first proves DMA is stopped before rollback. W1C
interrupt status is cleared only after the replacement queue is ready. An
unverifiable rollback panics while retaining DMA ownership. The patch also
checks firmware UFS power/clock state and refuses Sheng-only Wi-Fi/DPU SMMU
boot arguments on this board. It also refuses the legacy firmware-list
adoption probes, which zeroed the firmware UTRL before proving it idle.

Both device patches apply in order to a clean worktree at the pinned source
revision. The SMMU and production queue/command fault-injection tests pass with
WSL Debian `g++ 14.2.0` and `clang++ 19.1.7`
(`-Wall -Wextra -Werror`). Queue snapshot validation includes HCE/HCS ready
bits, interrupt mask, RUN_STOP and the original UTRL base. SMMUv2 sCR0 bit 0 is
CLIENTPD and must be clear; bit 1 is global fault reporting, not an enable bit.
The adapter requires IDR0.SMS and refuses EXIDENABLE because this path uses
SMR.VALID rather than extended-ID S2CR validity. A stale SMMU plan after queue
stop forbids resuming firmware DMA. Rollback rechecks the whole plan, including
stream aliases and context-bank ownership. This is host-only validation; the patched kernel object has not yet
been compiled on the pinned macOS runner or exercised on TB710FU hardware.

The official pinned arm64e `ShengUfs` recipe routes to
`build-sheng-source-storage.py`, which also requires device-owned
`SourceStorage.cpp`, `SourceRootPolicies.cpp`, and `SourceRootBinding.cpp`,
plus a separate macOS 27 IOStorageFamily/APFS ABI audit. The fixed DarwinBoot
payload path can carry the signed `RootPolicies` envelope and make its physical
range available under `/chosen/memory-map`; the target profile notes that this
handoff itself has not yet been exercised. The bridge inputs remain absent
from the clean device repository. The existing adapter workflow only compiles
one `NativeUfs.o` archive and explicitly reports
`linked_into_kernel=false`, `root_policy_ready=false`, and
`runtime_qualified=false`; the board keeps both storage config switches off.

No integrated kernel has been built or prelinked, so there is no new UFS kernel
payload to package. The existing generic kernel packager accepts the
ADT-correct `vendor_boot` baseline but does not provide a UFS root-argument or
RootPolicies sidecar input. The existing signed UEFI-UFS boot image is a
different artifact and does not contain the NativeUfs XNU driver. No new image
was packaged or signed. Next: run the bounded `ufs-source` workflow for the
macOS 27 object compile. Then implement the real source-storage bridges and
audit the IOStorageFamily/APFS binary ABI against the target macOS 27 kernel
before enabling the component, building a fresh integrated kernel and prelink,
or preparing a matching signed image pair.

The device adapter exports the exact pinned commit, applies both patches in a
temporary tree and runs the host tests there before the object compile. This
avoids using non-existent test paths in the device repository and ignores local
source-cache edits and line-ending differences. CI collects the host logs and
reports even if the adapter step fails.

## Official record and pinned UFS source review · 2026-11-17

Updated the existing root `porting.toml` storage area while preserving its
other device records. Storage remains `planned`; its findings and evidence now
record the standalone source archive without implying kernel integration or
hardware qualification. The pinned validator passed for all 15 areas:
`python3 .work/UniXNU/scripts/porting-record.py check --device .`.

Reviewed `ShengUfs@99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0`, the exact source
revision used by successful run
[44041029817](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44041029817).
The build report's upstream `NativeUfs.cpp` SHA-256 is
`8e99199aa87dc2ccebfe3366a0f6896fc8937d57670117af66c10ee52b1b28cd`; the
SM8650 compatible patch SHA-256 is
`8697b355a151c3cdb4aae8b595c58c05f760b4c3ddd6ce458ac5c8a1571b7993`. The
controller, GCC, and UFS clock register addresses align with the current board
inventory. The driver still assumes firmware initialized the controller/link,
requires an idle request queue, maps contiguous DMA through SID `0x60`, and does
not implement qualified reset, clock, or PHY initialization. The pinned
same-device Linux source at `4d04410798b538fbe4b50664e3a66d9500a2ed31` supplies
the static declarations: IRQ SPI 265, active-low reset GPIO 210, storage/PHY
rails, clock/GDSC/reset topology, SID `0x60`, two lanes, and the SM8650 QMP PHY
table. Linux cold-start resets and calibrates the PHY before selecting UniPro;
ShengUfs currently adopts the firmware-running link. Firmware's live clock,
reset, supply, link, queue, and SMMU states remain unknown. No kernel run was
dispatched because this source crosswalk changes no kernel inputs. The separate
takeover-order and integration audit is recorded above.

## USB/UFS adapter build prepared · 2026-11-17

The USB debug path now has a device-owned 64 KiB RAM tap. XNU `_cnputc` records
the console after newline normalization, and the direct `PE_kputc` serial
paths append without touching MMIO. The XNU serial callback under `_cnputc`
is not tapped separately, avoiding duplicate characters. A later worker can
drain the ring. Host tests cover the tap, but this is not a USB transport or
host-visible COM port: DWC3 endpoint/TRB handling, SID `0x40` DMA ownership,
startup and enumeration remain unimplemented. The official
`QcomDwc3` component is Apple XDCI/XHCI stack integration, not CDC-ACM, and its
PHY headers have a separate GPL-2.0-only license.

The `ufs-source` workflow runs the device-owned builder after the minimal
native kernel, using that build's XNU records and pinned framework interfaces.
Run [44027734178](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44027734178)
exposed stale XNU patch 0010 context after patches 0001-0009. The patch was
corrected and locally applied against the saved post-0009 snapshots in commit
`fa7b62f`.

After the user made the repository public, run
[44041029817](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44041029817)
completed and uploaded artifact `tb710fu-ufs-source-44041029817` (8,620,413
bytes, SHA-256
`61bb0852cc4d14a679ee2fd3bf88626376db2d16eeac535007b568a4990ab575`). The
artifact was downloaded to `.work/ufs-source-44041029817/`; the ZIP digest,
kernel record, and UFS archive digest all match their reported hashes.

The native-only kernel is 25,171,504 bytes with SHA-256
`0bb0d07e118ea4261613536b185a4dd80aaf90b97df75653c634c2e9bc54f948`.
Prelink was skipped and hardware boot was not verified. The standalone
`libTb710fuNativeUfs.a` is 541,448 bytes with SHA-256
`56a28627693c848f450614f293f3661f1632112ca1801cf2bdc004f341768c3c`; it uses
ShengUfs revision `99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0` and XNU compile
record SHA-256 `5b0cb4d43b31292ed6db2e73ea0cc8ea0c99a5dc9bc064f16b60797ee2e29d64`.
Its report marks `linked_into_kernel=false`, `root_policy_ready=false`, and
`runtime_qualified=false`. This is a successful source build, not a linked or
hardware-tested UFS driver.

The 14.1 GB userdata artifact was deleted after explicit authorization; its
byte-identical local ZIP remains. Runs 44038743847 and its second attempt had
failed before runner assignment. The current repository is public. The full
source-storage route is still not connected:
the board disables `CONFIG_XNU_SOURCE_STORAGE`, and the clean device repository
lacks the `SourceStorage.cpp`, `SourceRootPolicies.cpp`, and
`SourceRootBinding.cpp` bridge files required by that recipe.

## Owner policies and userdata `.vroot` rebuilt · 2026-11-17

Using the user-provided run `43724852936` artifact, OS, SystemOS and App were
extracted and matched to the pinned macOS 27.3 profile. The VerifiedRoot C host
verifier authenticated the RSA-3072/SHA-256 signatures and every 4 KiB block:
OS 3,538,432; SystemOS 712,192; App 13,312. All three policies share owner-key
SHA-256 `c2d8d462d0a790056bef2dddc30ae60ce210dbf0ee1cd4350a21e864db3a7d7e`.

The rebuilt `SHNGROOT v2` image contains the original windows plus raw
`OS.vroot`, `SystemOS.vroot` and `App.vroot` hash tables. The final 1 MiB object
padding, manifest checksum, all six row hashes, and zero-filled gaps passed a
full read-back. Image size is 17,603,493,888 B; SHA-256 is
`35b1c581c7074c626b4b3750af93901a988425e42671f1aaeafd7fd6e13de199`. Public
signed headers and trusted keys are in `scripts/policies/26C5035r/`; the private
key stays in the ignored local build tree with a restricted Windows ACL. This
is host-only validation: no flash, boot, or root mount has been tested.

## USB/UFS driver review · 2026-11-17

The official [UFS porting guide](https://unixnu.com/zh/porting/ufs) does not
require a separate register-probe build. It orders static hardware inventory,
implementation/adaptation and build integration first; the real driver then
checks controller, firmware, SMMU, DMA and queue ownership as part of
initialization. [`docs/UFS_PORT.md`](docs/UFS_PORT.md) records the known TB710FU
resources and remaining source-review gaps. The 4 KiB LBA 1 TWRP sample is a
later acceptance baseline, not a prerequisite for beginning the adapter.

No standalone XNU USB/UFS register-snapshot hook is selected. Host CDC/UFS
tests, `python3 tools/ci_kernel.py configure`, and the structured porting-record
check pass. There is still no DWC3 endpoint transport, DMA/SMMU ownership path,
CDC enumeration or XNU log hook.

There is no TB710FU XNU UFS build or hardware read. The pinned
`ShengUfs@99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0` contains real queue,
SMMU and command code, but its catalog metadata targets SM8550/iOS 17. The
macOS profile already supplies IPSW build `26C5035r` and the OS/SystemOS/App
image hashes and UUIDs; the official kernel context merges them and exports
`UNIXNU_IPSW_BUILD`. A direct component-script invocation omits that context.
The owner policies and matching `.vroot` userdata tables are now host-verified;
`docs/USERDATA_BUILD.md` records their hashes. The public-source storage route
still requires IOStorageFamily/APFS ABI inputs and remains unbuilt and
unqualified upstream, so it is not a drop-in macOS 27 driver. No standalone UFS
probe is required before adaptation.

## Mainline integration and Meizu21 audit · 2026-11-17

At the time of this earlier mainline audit, local `main` included the diagnostic
bring-up path, the Ace 5 audit, and the CDC protocol/log-ring host slice from
`1c6217ba46033419fd35f576f47b67ddc7ed9b98`. It was eleven commits ahead of
`origin/main` and had not been pushed. Those commits do not claim that the root
volume or macOS userspace boots.

The user-supplied [Meizu21 repository](https://github.com/XiaoChen8371/unixnu-meizu21)
targets SM8650 but is a Darwin 22 transition: `board.toml` selects only
`DarwinBoot`, the kernel provider is retained iOS 16.5/XNU, and
`evidence/kernel-snapshot.json` says `kernel_rebuilt=false`. Its
`meizu_ufs=1 root_apfs=ufs_super` arguments feed that retained Apple path; the
repository contains no native UniXNU UFS, DWC3, EUD, or CDC driver to transplant.
Its transition/rollback and protected-`super` workflow is useful process
reference, but its memory map, 16 KiB pages, ADT, and board addresses are not
TB710FU inputs.

## Ace 5 SM8650 USB/UFS reference audit · 2026-11-17

The [Ace 5 repository](https://github.com/tycykp/UniXNU-OnePlus-Ace5) is a much
closer reference than Meizu21. Its dated hardware record reports a real
Darwin 22 / XNU 8796.141.3 `fastboot boot`, USB CDC on Windows `COM21` (8,802
status bytes, zero drops), and an 8 KiB UFS read matching Android's CRC. A later
device record reports a read-write HFSX `iOSRoot` mount and launchd/system
service progress; SpringBoard/backboardd repeatedly crashed and no GUI appeared.
This is native XNU hardware integration, not an OEM kernel retained behind the
UniXNU menu.

Its CDC/UFS work derives from a pinned SM8550 Vermeer integration adapted for
Ace 5 SM8650. It is the best source for TB710FU USB/UFS bring-up, but controller
role, PHY, clocks, SMMU stream, memory layout, and XNU 8796 interfaces need
separate comparison with TB710FU/XNU 12377. The Ace 5 `board.toml` and
`hardware/sm8650-ufs.json` retain stale statements that no storage driver/DMA
was validated; the later run-bound read and root-mount records establish the
narrower tested behavior and should be cited with their image/kernel hashes.

## CDC protocol and log-ring host slice · 2026-11-17

Commit `1c6217ba46033419fd35f576f47b67ddc7ed9b98` adds the local device kit,
USB 2 CDC-ACM descriptor/request validation and a nonblocking bounded log ring.
The implementation is independent of the Vermeer source; test-only VID/PID
values are not device identifiers. The WSL host test
`CC=cc python scripts/check_usb_cdc_protocol.py` passed with warnings-as-errors
and AddressSanitizer/UndefinedBehaviorSanitizer. It covers descriptor layout,
malformed requests, ring wrap, full-buffer behavior and writer-lock contention.

The pinned UniXNU resolver also configured the `tb710fu_macos_defconfig` and
resolved the device-local package kit. This was a configuration check only;
there has been no new XNU native build for this portable source slice because
it is not yet linked into the kernel. No DWC3 register access, XNU log hook,
USB enumeration or UFSHCI/SMMU callback is implemented. TWRP FDT resource and
hash facts, the read-only UFS GPT sample and the host-only guard/state-machine
slices are in
[`USB_CDC_PORT.md`](docs/USB_CDC_PORT.md).

## Read-only UFS GPT sample · 2026-11-17

While the device was in TWRP, ADB confirmed `/dev/block/by-name/userdata` maps
to `/dev/block/sda14`; `/dev/block/sda` reports 4096-byte logical blocks. One
read-only 4096-byte block at user-LUN LBA 1 had the `EFI PART` signature and a
valid 92-byte GPT header CRC (`0d00f8ae`). CRC-32/ISO-HDLC over the full block
is `44b9c49a`; SHA-256 is
`03f7bbae3cd21bdce06c3f21bf24254c4e297629be14533993c0ab5d46976617`.

The raw block and device serial were not retained. This is a Linux/TWRP storage
baseline for a future read-only XNU probe, not evidence that XNU can access UFS;
queue ownership, SMMU and DMA remain untested. Full capture metadata is in
[`ufs-gpt-lba1-twrp.json`](docs/evidence/ufs-gpt-lba1-twrp.json).

## Host-only USB/UFS probe contracts · 2026-11-17

Commit `919b4bd` adds a CDC transport preflight guard and a one-block UFS
READ(10)/GPT sample state machine. Both are device-owned C slices; no
unlicensed Vermeer implementation was copied. WSL host tests passed with
AddressSanitizer/UndefinedBehaviorSanitizer:

- `CC=cc python scripts/check_usb_cdc_protocol.py` covers USB descriptors,
  setup requests, the nonblocking log ring and the fail-closed DWC3 guard.
- `CC=cc python package/Tb710fuUfsProbe/Test/run_host_tests.py` covers the
  READ(10) command, GPT/CRC logic, default-off behavior, SID/DMA refusal,
  queue restore and timeout quarantine.

These are preflight/transport-independent contracts only. No XNU 12377
hardware reader, DMA allocator, UFSHCI queue/SMMU adapter, DWC3 endpoint engine,
XNU log hook or startup personality is connected. No full XNU build or hardware
read/enumeration was performed. The current branch therefore does not yet
provide a USB port or an XNU UFS driver.

## Guard11 stage56 hardware checkpoint · 2026-11-17

The user supplied `XNU returned: tb710fu_ramoops_init` with
`build src-eff36305-guard11`, `STAGE=0x38` (56), and `ARG1=0x93ff00000`.
This confirms execution returned from the ramoops initialization call after
`arm_vm_init`; it is far beyond the early XNU entry/MMU markers. It does not
yet prove that the persistent zone header and console bytes were written, and
the device partition hash was not read back, so exact image attribution is not
confirmed.

There is still no live USB COM/XNU console for TB710FU. `serial=3` cannot create
a missing UART, and TB710FU EUD/CDC enumeration plus its XNU output driver
remain unverified. The portable CDC protocol layer has no controller or log
hook yet. The current
UEFI QR viewer reads the old `0xac340000` zone; guard11 writes to
`0x93ff00000`, so that viewer cannot yet export guard11's zone. A TWRP raw
memory read of `0x93ff00000..0x93ff40000` is the immediate post-reboot capture
path, subject to `/dev/mem` access and DDR retention. Do not treat stage56 as
proof that this read will contain a valid console until the zone header is
checked.

## Guard11 rebuilt and signed · 2026-11-17

The fileset-header correction is now in the signed diagnostic candidate. XNU
native arm64e build, macOS 27 prelink, and the production DarwinBoot fileset
parser passed in run [43917723572](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43917723572).
The exact 0006 patch SHA-256 is
`7be986c3744b7d9c9ca77f0d6b09e8e20b9ba74e587e527ac26281f9717a1a6c`.
Prelinked kernel SHA-256 is
`eff36305a28a6bf89c453734a07a9fe0e878e9041773864aeb205fc88483c3e3`; the
strict mapping report has `mapping_complete=true`, `missing={}`, and
`stubbed={}`.

The post-link reporter label is `src-eff36305-guard11`, with kernel SHA-256
`5d860a638dde58911cc35d66181f049689535f416c329bcbc27d93987ca8e09c`.
The signed pair is in `out/kernel-ttbr1-guard11/`: `vendor_boot.img` SHA-256
`10c3affb7b051b0b3f2ea1406c7246ee62bf257d128e8afc5db6a36dde63533d` and
`vbmeta.img` SHA-256
`d2f87a085be2319e3382d80678b6e3cccd8318a7b70eee6ea96fc449a406e4d2`.
Independent RSA verification and decoded kernel/ADT/TOML readback passed; the
vbmeta header, key and 22 unrelated descriptors are unchanged.

The exception/wrapper model, stage 47/48 3200x1080 rendering, marker-root
model, OneSend single-frame receive and rendered stage-47 QR decode passed. The
user later supplied a stage-56 report labelled `src-eff36305-guard11`, showing
`tb710fu_ramoops_init` returned at `0x93ff00000`. This confirms progress through
that reported checkpoint; no device partition hash binds it to the exact signed
pair, and the ramoops bytes, root-volume mount and macOS userspace remain
unverified. No device operation was performed by this task. Full provenance is
in [guard11 evidence](docs/evidence/kernel-ttbr1-guard11-build.json).

## Guard10 follow-up: switch returned; reporter mapping is missing · 2026-11-17

The `src-045bfc7c-guard10` screen shows `XNU returned:
vm/set_mmu_ttb_alternate_probed#1`, then `ESR=0x86000007` at
`ELR=FAR=0xfffffff00f82ba10`. This is a current-EL instruction abort with an
L3 translation fault. That PC is `diag_live_text+0x284`, inside `__KDIAG`; the
expected instruction is `CMP X22, X27`. Therefore the marked TTBR1 helper
returned. The following reporter instruction was not mapped by the active
TTBR1 root, so this report does not show a failure in the kernel's switch
helper.

The root cause is in diagnostic patch 0006: `getsegdatafromheader()` searched
`_mh_execute_header`, the embedded `com.apple.kernel` MH_EXECUTE. The post-link
reporter appends `__KDIAG` and `__KDATRW` load commands to the outer MH_FILESET
header, so those lookups returned no segments and silently skipped mapping.
The locally verified input and guard10 candidate both have this nested layout.
Patch 0006 now selects `PE_get_kc_header(KCKindPrimary)` for fileset kernels
and retains `_mh_execute_header` for standalone kernels. It was subsequently
built and signed as guard11; see the next status entry and
[guard11 evidence](docs/evidence/kernel-ttbr1-guard11-build.json).

CDP is not an available plug-and-play kernel log channel established by this
project. The live tree has a Qualcomm EUD node and the firmware contains EUD
serial code, but USB enumeration and an XNU EUD/CDC driver are unverified.
UniXNU's documented USB terminal is the giulia-specific protocol; changing USB
IDs does not add that protocol. See [log channel audit](docs/LOG_CHANNEL.md)
and [official debugging guide](https://unixnu.com/zh/guide/debugging).

The signed guard10 image remains at `out/kernel-ttbr1-guard10/` as the
superseded candidate. Guard11 contains the fileset-header correction. Device
partition hash readback is absent, so exact flashed-image attribution remains
unverified. No device action was performed by this task.

## Marker fault localized; guard10 signed · 2026-11-17

The new device report (`src-045bfc7c-qr8`) captured a real exception inside
`_set_mmu_ttb_alternate_probed`: `ESR=0x96000045` is a same-EL write data abort
with a level-1 translation fault, `FAR=0x93ffff000`, and `ELR=0xfffffff00bb8b374`.
The helper starts at `0xfffffff00bb8b360`; offset `0x14` is its first marker
`STRB`. `SWITCH_GUARD=2` only proved that the reporter/vector BRK self-test
worked. The then-tested guard9 map did not include the marker page, so this was
a diagnostic guard fault before the TTBR1 write, not evidence of an XNU
TTBR1-table failure. The report is consistent with guard9, but the device
partition hash was not read back, so exact flashed-image attribution remains
unverified.

guard10 adds and checks a writable, non-executable TTBR0 identity mapping for
`0x93ffff000`, and carries it through the temporary GOP root. The T1 walker also
now uses APTable bit 61 for EL1 write denial; bit 62 only restricts EL0. The
source is the same verified prelink from run [43844823907](https://github.com/limingerf/UniXNU/actions/runs/43844823907),
SHA-256 `045bfc7cfad9ed2539a586043dd43211263099734f4978abd8092c213b7d7d95`;
XNU was not rebuilt.

The signed pair is in `out/kernel-ttbr1-guard10/`: `vendor_boot.img`
SHA-256 `5e6f8b70369066b1b99ee5b4b69c2616449efc45df6394aaaf6be10365ac7b06`,
`vbmeta.img` SHA-256 `72e4807ec637f94d66195e0321f4e0c76dfb0960ed85fe11fd8f4099910da6df`.
Independent RSA and payload readback checks pass; the original ADT/TOML,
vbmeta header/public key and 22 unrelated descriptors are preserved. The
report label is `src-045bfc7c-guard10`.

The guard10 reporter build, exception/wrapper model, APTable host tests, stage
47/48 3200x1080 render model, marker stores under guard/GOP/restored roots,
OneSend stage-47/48 transfer, and stage-47 rendered QR decode pass. This remains
a diagnostic image: no flash or reboot was performed. The later stage-48
screen confirms the helper returned, but the reporter then faulted because its
segment was absent from the new TTBR1 root; later kernel progress remains
untested. Full machine record:
[guard10 evidence](docs/evidence/kernel-ttbr1-guard10-build.json).

## 内核停点再审计与二维码诊断修正 · 2026-11-16

用户此前看到 `XNU returned: vm/qsort#1`，它只说明 `_qsort` 已返回，不表示 qsort 崩溃。
用户随后提供 QR5 和 QR8 两份阶段 47 OneSend 报告。QR8 标签 `src-045bfc7c-qr8`
与最新版报告器一致；`ARG0=0x8c88fd000` 是新 TTBR1 根表参数，位于 2 GiB 管理窗口内。
报告明确在标记 helper 的 `BL` 前绘制，helper 尚未运行；旧 QR5 的
`marker helper entry` 文字已改为准确的 `before marker helper call`。
两份报告都不是故障 PC，不能证明 TTBR1 已写入或二维码之后内核停止。
用户尚未提供设备分区镜像哈希读回，因此不能确认具体签名包，底层停点仍未确诊。
源码审计确认标记 helper 保留了 XNU 原有 `DSB; MSR TTBR1_EL1; ISB; RET` 顺序；
目前没有可由 QR8 证明的 XNU 功能缺陷，也没有据此修改 TTBR1 切换代码。
此前 `0x8c88f9000` 的软件 walk 是模型根表，不是当前设备 `0x8c88fd000` 的映射读回。

## 单次页表快照与低地址异常捕获候选 · 2026-11-16

用户提供的 TCR `0x00080022a51a251a` 解析为 `T1SZ=26`、`TG1=2`，候选 TTBR1 根从 L1 开始，
使用 4 KiB 页。新 post-link 候选在阶段 47 读取本次 `ARG0` 根，逐级输出 L1/L2/L3 原始描述符，
检查 helper、返回 PC、内核栈、向量、报告器续点、`_set_mmu_ttb` 入口，并扫描全部 reporter RX 与 RW-NX 页。
读取前用 `AT S1E1R` 核对旧 TTBR1 下的物理别名，候选根本身只读。

同一候选为 `__KDIAG`/`__KDATRW` 建立独立的低地址 TTBR0 身份映射，描述符属性来自正在运行的旧 TTBR1 映射，
不依赖待测候选根；检查代码、状态区、栈和向量翻译，并在切换前执行可返回的 `BRK #0xb712` 自检。
阶段 47/48 的 GOP 临时表保留这份低地址映射；
helper 返回后先恢复 XNU VBAR，再恢复原 TTBR0/TCR。任何准备、翻译或自检失败都会恢复旧根并沿原向量路径继续。

输入仍是 run [43844823907](https://github.com/limingerf/UniXNU/actions/runs/43844823907) 的预链接内核，
SHA-256 `045bfc7cfad9ed2539a586043dd43211263099734f4978abd8092c213b7d7d95`；没有重编译 XNU。
guard9 post-link kernel SHA-256 `274917979bb503a18ce24e87ecaf8f5f186583ffb3bbf08cea1e0676f5863a6e`，
LZMA SHA-256 `cb19df3c4945d0bb92fd21c1d47567a16732162963b07401332f2cd98fca27d6`。
阶段 47 报告 591 字节，export-pause 报告 606 字节；阶段 47–56 的 OneSend 单帧接收和重复帧去重均通过。
AArch64 页表模型、4 种异常现场、低地址 BRK/ERET 与 TPIDR 恢复自检、28 个调用包装器、47–56 渲染模型均通过。

**边界：** 这是诊断版内核，不是 XNU 功能修复；设备实际页表结果、实机异常捕获和摄像头扫码仍未验收。
用户随后提供一份阶段 47 页表报告（`ROOT=0x8c8905000`）：helper、XNU 返回点、栈、向量和下一条 `_set_mmu_ttb` 均为 `+`；候选根缺 `__KDIAG` 12 页及 `__KDATRW` 200 页，报告续点 `C` 为无效 L3，分别显示 `X=0/12`、`D=0/200`。这发生在 helper 调用前，不证明 helper 已运行或 XNU 切换失败。
该报告中的 `G=3` 是当时 guard 错误地从待测候选根取 reporter 页造成的，不是硬件根因。guard9 改为从 `O1` 旧 TTBR1 读取 reporter 物理页，再建独立 TTBR0 映射；还把旧映射权限收紧到 RX/RW-NX。guard9 已在 `out/kernel-ttbr1-guard9` 生成并签名，独立 RSA、AVB 和 payload 读回检查通过。
`vendor_boot.img` SHA-256 `f9f185bec93bcb4e01f9da2fe5d489324c98a64a62a6d01cfd06b00e7f0a14f8`；
`vbmeta.img` SHA-256 `a0daf10e828b690bc0aa7162d4c007080113ab30715c81df3adc566f3d8657e3`。
本轮未刷写、重启或改分区。机器记录见
[TTBR1 快照与 guard 证据](docs/evidence/kernel-ttbr1-snapshot-build.json)，设计和字段说明见
[TTBR1 快照文档](docs/KERNEL_TTBR1_SNAPSHOT.md)。

已修正诊断路径：0006 成对映射连续对齐的 RX `__KDIAG` 与 RW-NX `__KDATRW`；
0009 保留标记 helper 为 noinline 符号。run [43844823907](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43844823907)
在提交 `e83bd00d94fc947ecdefd3fafdd471711561dff3` 上完成 native arm64e、严格 prelink
和固定 DarwinBoot 生产解析器。native 输入 SHA `670f2576…`，prelinked kernel
SHA `045bfc7c…`，86 个组件闭包，`mapping_complete=true`、`missing={}`、
`stubbed={}`、`allow_missing=false`。artifact SHA-256
`8063bc02fc5d405cbb7e20524a21e05a844c8937495b69b77e8dedc3597a33cd`。

最终报告器标签为 `src-045bfc7c-qr8`，可见阶段为 47/48、49/50、51/52、54、55/56。
分别覆盖 TTBR1 helper 调用前/返回后、切换后首个调用、`pmap_bootstrap`、
抢占重新开启后的映射完成点，以及 `arm_vm_init` 返回后的 ramoops 入口/返回。
抢占关闭期间不绘制 QR；普通检查点四帧轮播约 2 秒后继续。
九阶段 AArch64 模型、九张画面 ZXing 解码、固定 OneSend 接收器单帧文件还原均通过。

最终候选 `out/kernel-switch-qr8-label-signed/vendor_boot.img` SHA-256
`b5274a747ac23d31f9bb9648738076fe78d12f333ffc9852d9927592562f5280`，匹配
`vbmeta.img` SHA-256 `88dd4989330fad6bc9f0e6edeca38ee9996081bc2691c842ea2412e9c4af991b`。
独立 RSA 复算、payload/ADT/TOML 读回、vbmeta header/公钥和另外 22 个描述符检查通过。
用户已提供 QR8 阶段 47 报告文本（SHA-256 `b141c41e…`），但没有设备镜像哈希读回，
也没有阶段 48 及后续报告；不能确认刷入的 vendor_boot/vbmeta 是否为下方签名候选。
底层故障仍未确诊。
机器证据见 `docs/evidence/kernel-switch-qr-build.json`。
ARM64e per-core PAC 仍待硬件验证。详见
[TTBR1停点修正](docs/KERNEL_SWITCH_QR_FIX.md)和[官方流程对照](docs/UNIXNU_OFFICIAL_AUDIT.md)。

参考项目 `bmk-keyeast/UniXNU-tb710fu` HEAD `23235249d71604f8a6f743680ef8fd1003fe7189`
使用 iPadOS 17.7.11 / XNU 10063.141.1；没有当前 XNU 12377 的 TTBR1 路径。
只借鉴 ramoops 格式测试和分阶段探针经验，不复用其窗口外的 `0xac340000`
日志地址。当前状态入口中的旧 A/B 产物哈希与云 run 历史照常保留。

> 并行交付完成（2026-11-16）：两份可刷镜像均已签出并独立验签，均未上机验收。
>
> **(A) 切换内标记** `out/kernel-switch-markers`（`fafb1f96…`/`b64e33bc…`）。
> 云 run 43819437775，内核 `860834c5…`。0007 在 `set_mmu_ttb_alternate`
> 的 MSR 前后各写一个恒等映射字节（`0x93ffff000` / `+1`），走 TTBR0 V=P，
> 不依赖新页表；机器码逐条核对夹住 MSR。实机屏幕停在
> `XNU returned: vm/qsort#1`、无 QR，正是标记版预期（说明
> `TB710FU_SWITCH_MARKS=1` 生效、探针的 stage 47 那条 BL 未走）。
> 读标记需进 TWRP（Android `CONFIG_STRICT_DEVMEM` 拦 `/dev/mem`），
> `devmem 0x93ffff000 8`：01 00=MSR/ISB 没完成，01 02=切换完成。
> 见 [切换内标记](docs/KERNEL_SWITCH_MARKERS.md)。
>
> **(B) 窗口内日志区** `out/kernel-log-region`（`15846226…`/`e4cffaf3…`）。
> 云 run 43827454902，内核 `4ad08f1b…`。0008 把 ramoops 默认 PA 从不可达的
> `0xac340000`（比 physBase 低 32.3 GiB）改到 span 内 `0x93FF00000`+256KiB，
> 拒绝 span 外 override；机器码确认常量、区域几何 7/7 PASS。镜像同时含 0007。
> 注意 ramoops_init 在 arm_vm_init 返回之后，本次 stage 47 停点仍靠 0007 标记，
> 0008 只为越过切换后的启动提供持久日志。见 [日志区交付](docs/KERNEL_LOG_REGION.md)。
> 未刷写、未重启、未改分区。

> 待验交付（2026-11-16）：**段映射改为不依赖引导参数**。
> 上一版把映射挂在 `tb-diag-segment=` 上，反汇编证实编译器生成的守卫是对的
> （`cbz w0` 跳过整块），所以失败原因是**参数从未到达内核**，映射静默未执行——
> 与设备始终只见 `set_mmu_ttb_alternate#1` 完全吻合。
> 本版改为按段名无条件查找：不存在的段返回 NULL 且 size=0，代码要求两者非零，
> 因此**普通内核零映射**（5 情形矩阵全 PASS）。
> 云 run 43813385537 success（head `fbe75be`），内核 `6c71e39c` 中
> `tb-diag-segment` 字面量已为 **0**、`__KDIAG` 为 1。
> 交付 `out/kernel-diag-unconditional`（`67b19c55…` / `759bd40d…`），
> 引导参数已清理，ADT 身份从成品 payload 直接读出确认为 `Mac17,11`。
> 见 [无参数段映射交付](docs/KERNEL_DIAG_UNCONDITIONAL.md)。

> 上一交付（2026-11-16）：第一份内核自带 0006 的可刷镜像
> `out/kernel-diag-past-switch`（`2c35ccc4…` / `dc03f3b3…`）。云 run 43806716346
> success，内核 `6adc7e15…`，0006 在 `xnu-inputs.json` 第 132/132 项且哈希一致。
> 补丁**确已编入机器码**（`tb-diag-segment` 字符串与 `_arm_vm_prot_init` 对它的
> 引用各 1 处，基线均 0 处；切换点位移 `0x88`）。注意总字节数未变——增长被段内
> 填充吸收，但 7,297,495 字节不同、5,650 个符号位移，不是缓存复用。
> 探针追踪点 25 个，新增 `vm/set_mmu_ttb#1` 位于切换点之后，可把"切换失败"
> 与"切换后失败"彻底分开。签名经独立 RSA 复算 + 生产解析器 PASS。
> 并行线 (b) 已证**不可行**：ADT 窗口任何调整都无法让 ramoops 地址可达
> （在 span 下方 32.3 GiB）。见 [切换后探针交付](docs/KERNEL_DIAG_PAST_SWITCH.md)。

> 历史分析更正：T1SZ 软件遍历使用模型根表 `0x8c88f9000`，输入包括推定的
> `topOfKernelData=0x8c88f8000`；它不是设备表项读回，也不是后来 QR8 的
> `ARG0=0x8c88fd000`。该结果只说明模型路径，不否定当前实际根表可能缺少返回点映射。
> 参数门控版 0006 的 212 页空隙是旧 reporter 布局问题，后续已由无参数版修正。
> QR8 的阶段 47 在 helper 调用前，当前仍无阶段 48 或设备物理标记值；不能把旧模型
> “返回地址可执行”当成硬件切换已通过的证据。
> `patches/xnu/0006`：切换**前**把 `tb-diag-segment=` 指定的段映射为 RX。
> 默认启动零映射（行为矩阵 4/4 PASS，连段查找都不做），不需要改官方构建脚本
> （曾误改 `build-xnu-4k-baseline.sh`，已回退并验证与 HEAD 逐字节一致）。
> 见 [切换后探针](docs/DIAG_PAST_SWITCH.md)。

> 修复件（2026-11-16）：已用修补后的 `board.toml` 重新生成带 macOS 身份的 ADT
> （`Mac17,11` / `K114cAP` / `/product` 35 项 / `/chosen` osenvironment+
> boot-uuid+root-matching），只替换 payload 里的 `.adt`，内核 `9daf5064…` 与
> `unixnu.toml` 逐字节未变；签出 `out/kernel-adt-fix`（`eb09e141…`/`a7472fe5…`）。
> 签名经**独立 RSA 复算**（不调 avbtool）+ 4 例反篡改对照通过。
> **边界**：ADT 在 `arm_init:380` 即被读取，而停点在 `arm_vm_init:586`，
> 所以这是客观缺陷但**未证明是绿条成因**。见 [ADT修复交付](docs/KERNEL_ADT_FIX.md)。

> 上一审计（2026-11-16）：对 out/kernel-pr249-final 逐字节复核，交付本身自洽
> （vendor_boot `92232f89…`、内核 `9daf5064…`、PR249 的 22 页确在机器码中）。
> 但两点由此定量排除/确认：① PR249 与本机绿条卡点无关——2 GiB 窗口只需 8 页，
> 旧 16 页池已够，且实机已越过该检查到 set_mmu_ttb_alternate。② **真正的缺陷是
> 交付的 kernel.macho.adt 不是 macOS 设备树**：根 model 仍是
> `Qualcomm … Pineapple MTP`、`/product` 为空、`/chosen` 缺 osenvironment/
> boot-uuid/root-matching，内核匹配不到任何平台专家。已补 board.toml
> `[payload.adt]` 并新增身份门禁，重新生成 9 项全 PASS；未重编译/未刷机。
> 见 [绿条审计](docs/GREENBAR_AUDIT.md)。

> 上一交付（2026-11-16）：PR249 的16→22页容量修复已适配到本机
> xnu-12377补丁序列。完整native/严格prelink云run43763928457成功，
> 独立核对机器码22页、依赖闭包、生产解析器与artifact哈希通过。
> 原生启动候选已本地签名，out/kernel-pr249-final；无附加诊断段和
> 导出暂停入口。原卡启动尚未证实修复，实机待验。见 [内核交付](docs/KERNEL_PR249.md)。

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
| 绿条交付逐字节复核 | 通过 | `92232f89…`/`9daf5064…`/22页机器码；见 [绿条审计](docs/GREENBAR_AUDIT.md) |
| ADT是否为macOS设备树 | **失败（已定根因并修复输入）** | 交付ADT根model仍为Qualcomm MTP、`/product`空、`/chosen`缺macOS键；补`[payload.adt]`后重新生成9项全PASS |
| PR249对绿条卡点的相关性 | 已排除 | 2 GiB窗口仅需8页，旧16页池已够；实机已越过该检查到set_mmu_ttb_alternate |
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


固定报告轮播的独立源码云检查 run43759850376 success，源提交
b166a08f1718e5698a7524a8e6e5af3bda6f7bda。AARCH64对象、主机轮播帧
与未修改的固定OneSend实际收件/文件还原通过；下载artifact SHA-256与
GitHub digest一致，接收源与报告源身份匹配。非native/prelink或实机验收。


## 纳入 PR249 容量修复，回到完整内核构建 · 2026-11-16

用户授权纳入 PR249 并继续修内核。PR 当前仍 open，固定 head
9b9db9c9ff13a595492d241366426383c88bf236；其旧 xnu 补丁的16→22页
改动适配为设备0005，放在现有 xnu-12377 序列之后。没有改官方缓存。
多占24KiB；大型内存34页和其他8页分支不变。实际应用和9GiB边界检查通过。

对旧cc690c9a机器码执行arm_vm_init的有界模型，使用用户bootargs PA
8c88f4000与推定topOfKernelData=8c88f8000，生成root恰为8c88f9000。
切换函数/返回地址/向量可执行、真实SP可写、集合头可读；按新根重新映射
后模型到达pmap_bootstrap。DT/熵/特权指令/PAC有模型限制，不当作实机
切换成功，也未据此加入猜测性的MMU补丁。下一份完整构建不附加__KDIAG
或主动导出暂停；PR249容量修复与原故障修复分开验收。


完整native/prelink云run43763928457 success（源f95c34942e9317e97e8bba819e364e75a6a80b41）。
原生SHA 7b263aeee5f78af52ff6a9286d0a9e5f175212850114ba5696461fa5f537e9cf；
严格预链接SHA 9daf5064924a2a085b1e8ee4e0871df5b9fc86dac516c8d7de9a85484f2ca8ee。
mapping_complete=true、missing={}、stubbed={}、allow_missing=false；生产
DarwinBoot解析通过。实际启动清零机器码为22页，未含__KDIAG，arm_init和
panic原始PACIBSP保留。artifact SHA/digest匹配，LZMA原始流读回一致。

新交付 out/kernel-pr249-final/vendor_boot.img（92232f89…）与匹配
vbmeta.img（89243b22…）沿用已有本地签名授权生成；签名和引用通过，
其他22描述符/header/公钥不变。FAT32 66512簇，原ADT和默认菜单哈希
均未变，没有导出暂停参数或新探针。未操作设备，原故障实机待验。
