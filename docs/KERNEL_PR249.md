# PR249 页表容量修复与原生内核交付

2026-11-16 按用户要求纳入上游 PR249 的修复。核查时 PR 仍 open，
head 为 9b9db9c9ff13a595492d241366426383c88bf236。没有合并整个
主仓库或更新固定依赖；只把其非 Apple 4 KiB 页表池16→22页改动
适配为设备补丁0005，放到选中的 xnu-12377 序列最后。

新增6页共24 KiB。启动页表的分配、genassym导出、清空与耗尽检查
继续使用同一 BOOTSTRAP_TABLE_SIZE。ARM_LARGE_MEMORY 的34页及
其他平台8页分支保留。2 GiB窗口所需页数在旧预算内；此补丁解决
扩大内存窗口时的已知容量问题，不能解释现有的最后屏幕。

## 构建结果

完整 macOS native→严格 prelink 云 run43763928457 success，源提交
f95c34942e9317e97e8bba819e364e75a6a80b41。原生内核
7b263aeee5f78af52ff6a9286d0a9e5f175212850114ba5696461fa5f537e9cf，
预链接内核9daf5064924a2a085b1e8ee4e0871df5b9fc86dac516c8d7de9a85484f2ca8ee，
142753792字节。mapping_complete=true、missing={}、stubbed={}、
allow_missing=false；同版生产DarwinBoot实际解析通过。
下载artifact的SHA-256与GitHub digest完全匹配。

独立机器码检查确认启动清零计数为22页。没有附加 __KDIAG 段，
arm_init/panic 原始 PACIBSP 保留。交付沿用原默认XNU菜单，不加入
导出暂停、自检或VM调用包装；保留匹配ADT和原来的ramoops参数。
没有改变固件boot_b、根卷或设备分区。

## 现有问题边界

用户已实机收件reporter4的切换前报告，ARG0=8c88f9000。旧内核实际
arm_vm_init机器码的有界模型在使用bootargs PA=8c88f4000和推定
topOfKernelData=8c88f8000后生成相同根地址。新根覆盖切换函数、
返回地址、栈、向量和集合头；模型按新根重映射后到达pmap_bootstrap。
这只能排查一部分确定的映射错误。DT/熵、特权寄存器和PAC有模型
限制，不能证明真实CPU已经跨过切换，更没有证明原卡启动已修好。

此版是完整重编译的原生启动候选，容量修复与原始卡点分别验收。
需要设备验证，编译/预链接通过不等于硬件启动成功。

## 刷写

使用fastbootd，保留当前boot_b，刷配对vendor_boot和vbmeta：

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-pr249-final\vendor_boot.img'
& $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-pr249-final\vbmeta.img'
```

选择原XNU启动项。镜像中不包含intentional export pause路径。
本轮没有执行设备刷写、重启、切槽或设备文件系统操作；本地签名沿用
已有授权。哈希及全部验证证据见
[构建记录](evidence/kernel-pr249-build.json)。

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| vendor_boot.img | 100663296 | 92232f89a2bba35b015f104800e35698c6e1c5ea35ef2c5311c62f9cc62f7a14 |
| vbmeta.img | 8000 | 89243b22dfc247cedd6fad7bfd1329d21c6f270540b24b3f2f4cfa4fff062136 |
