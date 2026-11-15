# 自检二维码对齐修复与实时阶段文字

## 照片确认的缺陷

自检图为 REPORTER SECONDARY EXCEPTION，REPORT_PHASE=2。
FIRST_ESR=f200b711 是主动自检；第二次 ESR=96000061 表示当前EL数据访问的
alignment fault。FAR=8c88b1eae，SP=8c88b1e90，ELR=8c8829fc0。
对应交付ELF的指令是：

    fffffff00f829fc0: stur w8, [sp, #0x1e]

它正好写 SP+0x1e，即FAR的地址，而地址模4为2。编译器把LMS1中连续两个16位
长度字段合并成32位未对齐写入；报告器已关闭MMU，该访问在实机产生对齐异常。
这不是XNU原来的故障，解释了自检不出二维码。原来平坦内存模拟允许该访问，
因此旧测试不足以说明实机编码安全。

修复：所有freestanding报告器和未修改的微软QR源码均增加 -mstrict-align，
保留所有正常访问检查，不通过关闭对齐检查或返回假成功来掩盖问题。
新增自然对齐访问回归：旧机器码在同一PC重现4字节未对齐写入，新机器码完成
整个报告/QR路径且没有未对齐访问。新QR独立解码回精确OS帧。
原始照片只记录SHA和字段，未上传仓库；见
[reporter-alignment-fault.json](evidence/reporter-alignment-fault.json)。

## 实时状态，而不是再加色条

早期默认路径不停机，只在顶部48行覆盖显示：

    XNU: arm_init entry (running)
    XNU enter: PE_init_platform
    XNU returned: PE_init_platform
    ...

每次调用前/返回后都更新同一块区域，停住时可拍最后函数名，不依赖异常已经发生。
它显示最后经过的阶段，不能把该名字当成精确故障PC或认定函数内部必然有错。
若出现异常，仍显示寄存器及OneSend；单帧静态QR无需调速。

为避免回到早期“不映射GOP就无法写屏”的问题，实时渲染临时使用独立TTBR0的
L1/L2表，只映射顶部GOP所在的设备内存块；代码、数据和栈仍在TTBR1上运行，
不关闭MMU或缓存，不改原内核页表和MAIR。完成后恢复原TTBR0/TCR并维护TLB。
参数、结果、X18及原返回路径保留。该方法会扰动映射与时序，属于早期诊断，
不是长期显示驱动；arm_vm_init换页表后诊断段可达性仍未实机验证。

## 安装与反馈

保留当前boot_b，在fastbootd刷本版两份文件：

    $fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
    & $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-live-report-final\vendor_boot.img'
    & $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-live-report-final\vbmeta.img'

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| vendor_boot.img | 100663296 | f8688874dede5c2a74478b457a6d9f8f32ca3118b6fc30b714560964c1301175 |
| vbmeta.img | 8000 | eb126c615e094c598b704b75d7c5bb2c7bf1a473e614ff940421401320a574d2 |

先选原XNU项，拍最后停住的顶部文字；若出现异常则拍寄存器或扫码。
菜单自检可单独验证QR修复，但自检成功不等于原内核故障已解决。
不用先刷根卷，也没有新增默认主动停点或死循环看门狗。

## 验证和阶段边界

原输入仍为cc690c9a预链接内核；诊断副本为
39a1c98d38e65593c9586d0b83b7d1363f301c84e657fe47ecbcb166f347d3d9。
它不是重新native/prelink后的正式验收镜像。

实际指令模型检查捕获/物理向量/14个调用包装与参数栈保留；单独执行实时
渲染机器码，验证页表描述符、只写顶部48行以及TTBR0/TCR恢复；执行完整
AARCH64报告/微软QrEncodeData并核验所有数据/栈自然对齐，达到QR READY。
这些测试显式模拟寄存器和缓存，不能当成硬件MMU/PAC/页表切换验证。
同版生产Mach-O解析器、LZMA/FAT读回、两份签名及vbmeta引用通过，其他22个
描述符/header/公钥不变。沿用已有本地签名授权，无设备操作。

自检故障已由照片和同一指令回归定位；新镜像的实机修复效果、实时阶段文字和
XNU原始停点仍待反馈。记录见
[kernel-live-report-build.json](evidence/kernel-live-report-build.json)。


源编译云检查 [run43749763327](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43749763327) success，提交7076a0036a87a633cba367292c3cc31c774287d3。AARCH64对象与主机传输检查通过，独立产物匹配；不是新native/prelink。实机QR和实时文字仍待验。
