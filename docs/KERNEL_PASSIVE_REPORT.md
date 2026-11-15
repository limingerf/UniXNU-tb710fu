# 继续原内核，只在异常时报告

用户照片中的 ESR=f200b710、ELR=fffffff00c28ff68 对应旧版人为设置的 BRK，
说明 C 序言与重定位已完成；它不是原始故障。按用户要求，本版删除默认路径的
主动检查点，继续执行 PE_init_platform 和 arm_vm_init，不再用人为停点冒充卡死。
只有单独 Reporter self-test 菜单仍可主动 BRK。

## 本次镜像

保留当前 boot_b，在 fastbootd 刷本次成对文件：

    $fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
    & $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-passive-report-final\vendor_boot.img'
    & $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-passive-report-final\vbmeta.img'

| 镜像 | 字节 | SHA-256 |
| --- | --- | --- |
| vendor_boot.img | 100663296 | 9e8920b0947c7f3dd585de5bc13d3a11d9d280ab959ed5be032dbf1ce4e3516e |
| vbmeta.img | 8000 | c75712b85ff4194d2f0ca8f0c19bb76be10a620ec48706497a90c0da7ff36f0d |

选原来的 XNU 项，让它继续。早期同步异常或 panic 时显示寄存器文字、
最后进入/返回的函数名和 OneSend 单帧。正常 IRQ/FIQ/SError 路径转发回原向量。
旧色带会清除，报告前只用顶部48像素，不向下叠条。

这不是死循环看门狗：一直自旋、硬件等待或异常路由/页表已更换而通道不可达时，
不能保证自动报告，更不能把“最后阶段”称为故障PC。
当前范围是早期启动；arm_vm_init 更换页表后的报告能力尚未实机验证。
内核本身若主动 panic，panic 格式指针保存在 X0；ESR/FAR/ELR 可能是旧状态，
应结合 panic 标签和调用阶段判断。

## 二维码诊断修正

第一张实机图只有文字，没有可见二维码，具体原因尚未确认。
源码发现终止式报告关闭MMU后仍保留KVA向量地址，已改成独立物理终止向量，
使报告器自身发生的第二次异常能被捕获。第二次报告注明 REPORTER SECONDARY
EXCEPTION、REPORT_PHASE、FIRST_ESR/FIRST_ELR，并跳过QR以免递归失败。

新增 QR: PREPARING / ENCODING / READY 可见状态，二维码优先放屏幕右侧。
若仍无二维码，应拍下状态行和寄存器。仅“READY”提示或主机测试不等于实机扫码成功。
LMS1/OS/CRC32/微软QrEncodeData协议不变，单帧无需调速。

## 代码及验证

输入仍为原 cc690c9a 预链接内核，附加段、地址/指令守卫和固定源验证同
[旧报告器说明](KERNEL_EARLY_REPORT.md)，本版诊断内核SHA-256为
e5465b66ea057a49ae41296c88deee402a09cf1ddd648f5c615b1a88ec1ae16e。

对14个原始直接调用加进入/返回记录，包含平台、定时器、CPU拓扑、线程引导、
startup和arm_vm_init。包装仍调用原函数，保留参数、结果、栈和原返回地址。
默认调用点均为到包装的 B，没有新增主动BRK；工具可选
--checkpoint-before-vm 仅供明确要求的实验，交付镜像未使用该选项。
不屏蔽PAC/安全检查，不添加空硬件实现，不把诊断器当永久修复。

验证：原始变更范围、向量转发目标、14个调用包装的指令模型、原31寄存器/栈/
relative重定位模型、同版DarwinBoot生产解析器均通过。额外执行实际AARCH64
报告和QrEncodeData机器码到READY，独立ZXing将QR还原为完全相同的752字节OS帧。
模型使用平坦内存并显式模拟控制寄存器/缓存，不能替代实机MMU/PAC/中断行为。
签名后payload不变、两份签名与引用及其余22描述符保持检查均通过。

本轮沿用用户已有仅本地签名授权，没有设备操作或根卷修改，没有重编native/prelink。
报告器源单独云编译检查，不冒充整个内核重建。当前仍待用户实机反馈。
记录见 [kernel-passive-report-build.json](evidence/kernel-passive-report-build.json)，
旧图记录见 [first-arm-init-report-photo.json](evidence/first-arm-init-report-photo.json)。
