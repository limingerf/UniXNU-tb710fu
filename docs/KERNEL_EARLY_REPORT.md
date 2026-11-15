# 核内早期异常报告与 OneSend

用户已报告白/蓝/紫/青/黄，证明 XNU 入口、开启 MMU 后和 arm_init KVA 入口探针
执行。黄色在原始 PACIBSP/栈序言之前，不能据此确认 C 初始化或重定位完成。
此版本用异常信息取代继续堆色条。

## 交付和一次测试

保留当前已出白条的 boot_b，在 fastbootd 刷本次两个文件：

    $fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
    & $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-report-final\vendor_boot.img'
    & $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-report-final\vbmeta.img'

| 镜像 | 字节 | SHA-256 |
| --- | --- | --- |
| vendor_boot.img | 100663296 | 77af03cd895a51461ebae9dd69f5f475391fe4b18949764a9096e00572f11091 |
| vbmeta.img | 8000 | 8edf12371a4c954a428ba285928adbbbd9a2075c0feddccf1f94b3502ff8461e |

先选原来的 XNU 项。启动时清掉旧五条色带，早期阶段只覆盖顶部 48 像素区域。
发生异常后，屏幕改为寄存器报告和一个静态 OneSend 二维码。
用 OneSend 扫码导出 .log，或直接拍下文字，尤其 ESR/FAR/ELR/SP/LR。
不依赖复位后 ramoops 是否保留，无需先刷根卷。

报告标题区分：

- XNU EARLY EXCEPTION：真实异常。若正文注明 panic，X0 是 panic 格式指针；
  此时 ESR/FAR/ELR 不一定是本次 panic 的直接原因，不能当作已判定的故障地址。
- XNU CHECKPOINT (intentional stop)：主动 BRK 检查点 b710；原始序言、fileset
  重定位及其后到首个 PE_init_platform 调用前的代码已走过，属于诊断暂停。
- REPORTER SELFTEST (intentional stop)：单独菜单项 Reporter self-test
  (intentional stop)，通过 tb-report-selftest=1 在最早 C 入口主动 BRK b711，
  只验证捕获/显示/扫码，不代表 C 初始化通过。

正常 XNU 项若不出报告，可手动重启后选 Reporter self-test，区分“报告通道不通”
和“内核无异常死循环”。它会停住，不自动重启；同一次停屏读取状态和二维码。
本版在平台初始化前主动停止，故不会继续尝试根卷或用户态。

## 报告与协议

这是异常快照，不是完整连续内核日志。捕获全部 31 个通用寄存器、原 SP、
ESR_EL1、FAR_EL1、ELR_EL1、SPSR_EL1、SCTLR_EL1，文件和屏显选取关键字段。
不解引用故障地址、原栈或 panic 字符串，避免报告器再次触发同类异常。

沿用板级 UEFI OneSend：LMS1 version 2 信封，文件名 TB710FU-early-fault.log，
MIME text/plain；OS version 2 单帧，block length 720、序号0、block count1，
正文/信封/帧分别 CRC32，帧总长752。固定微软 QrEncodeData Byte 模式生成
121×121 小位图后整数倍放大。

报告约0.5 KiB，一帧即可容纳，静态二维码不需要速度切换。
本次未实现核内按键、长日志分帧或五档控制；这些要另适配实际输入和日志来源，
不能调用 ExitBootServices 之后已结束的 UEFI 事件/USB/内存分配服务。

## 来源和实现边界

输入仍是 native run43709117408 → prelink run43710067378 的 cc690c9a 内核。
守卫工具只接受确切 SHA-256，并只读加载固定 UniXNU
0c1bac3d51a3009dca1708b15a5198f4b43d057a 的官方解析、符号和分支工具。
二维码源码固定 microsoft/mu_plus@b8d46c71610318e36a661819e1e3c0b8bdda413a，
与 UEFI Common/Mu gitlink 一致，逐文件 SHA 核实后原样编译。
来源清单 diagnostics/early-report/qr-sources.json。字体来自同版 UniXNU Font.c，
保留上游版权。Compat.h 是显式 freestanding 适配，不调用 BootServices；
512 KiB 单次有界 arena 提供分配，FreePool 对该终止式 arena 不回收。

在 fileset 附加 __KDIAG 段，含报告器、固定 QR 源、独立16 KiB栈和arena。
保留原 PACIBSP 重放，不屏蔽 PAC 或安全检查；在 arm_init 入口安装 EL1 向量，
在原始异常处理器保存栈前捕获。报告路径不依赖可能已损坏的原栈。
终止路径改动线程寄存器、清理诊断段缓存、关闭 MMU/缓存并切到独立物理栈；
修正本段 PIE relative 指针后直接写物理屏幕，最后停机，不返回原内核。
panic 入口也仅为报告而替换。主动 BRK 只替换核实过的首个 PE_init_platform BL。
原文件 prefix 变化独立核验限于记录的 header、分支和三个 compact 空填充区；
其余变更均在新段。不修改 fetched source cache。

这是诊断构件，不是新的原生链接/严格预链接验收，也不是永久修复；它会扰动
布局和异常路由，真实故障要结合原版结果复核。

## 验证

- 实际新 Mach-O 经过固定 DarwinBoot 5243c68e 生产解析器，通过段边界和入口检查。
- 变更范围、新段、LZMA/raw 往返、FAT66512簇和签名后读回通过。
- Unicorn 指令模型核验31寄存器、SP、异常字段、SP0/SP1、独立栈及24项 relative
  fixup。控制寄存器/缓存显式模拟，PACIBSP跳过，不能代替实机 MMU/PAC/异常路由验证。
- 主机C/QR代码生成异常fixture、主动检查点、自检和panic四类帧，各级CRC通过；
  独立ZXing-C++解码回完全相同的二进制OS帧。测试fixture不是设备日志。
- 编译器全部静态C frame之和1520，加2 KiB余量小于16 KiB独立栈。
- AVB签名、vendor_boot hash和vbmeta引用一致；header、公钥、其余22个raw
  descriptor及次序不变。原默认项/条目不变，只追加自检入口，ADT不变。

沿用用户已有仅本地签名授权，未操作设备或根卷。实机捕获和OneSend接收待验。
异常或主动检查点才会出报告，普通死循环不会自动产生异常。
独立云工作流 early-report.yml 只编译诊断源和测试传输，不重新native/prelink、
不使用密钥。机器记录见 [kernel-early-report-build.json](evidence/kernel-early-report-build.json)。

## 云端源编译复验

[run43744001120](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43744001120)
成功，源提交018a7d7a94a94ec53f9b927621e9252c40124f83，artifact为
tb710fu-early-report-43744001120。实际AARCH64对象和主机二维码/CRC传输测试通过，
没有把独立对象编译当成整个XNU的native/prelink构建。

前轮43743753427的AARCH64对象已编译，失败在macOS主机动态库测试不能使用ELF
段名.data.state。诊断清楚后仅对TB_REPORT_HOST_TEST采用本机默认data段。
修改前后实际freestanding Report.o逐字节一致，SHA-256为
9b5a4df34cfa7e6ab6fb6904a69830685dd35cd354d1e23b6641baf2799a18d2，
因此交付签名镜像未变化。此失败不是XNU编译失败。
