# 正常启动阶段二维码与 VM 检查点

> 本页为 reporter 3 历史交付。静态接收问题与新版固定文件轮播见
> [轮播导出](KERNEL_ANIMATED_QR.md)。

## 实机依据

2026-11-16 用户确认 reporter 2 自检能显示并扫出 OneSend，内容为
REPORTER SELFTEST / ESR=f200b711 / ELR=fffffff00f8280bc。严格对齐
修复的自检路径已有实机证据。普通启动最后函数名为 arm_vm_init，但用户
没有逐字提供 enter/returned 前缀，不能据此断言函数内部已死锁。
原正常路径只画文字，异常才生成 QR；没有异常时自然不会生成阶段 QR。
转录见 [实机记录](evidence/live-qr-hardware-feedback.json)。

## 变更与来源

本版沿用 cc690c9a0bde84c49e9cebccc288f393393d16da56620d580108f0a62692b31c
预链接输入，仅生成带诊断的副本；未重新 native/prelink。二维码源码仍为
microsoft/mu_plus@b8d46c71610318e36a661819e1e3c0b8bdda413a 原文件。
所有 freestanding C 保留 -mstrict-align。

- 从进入 arm_vm_init 前开始，每个检查点在继续运行前生成并显示阶段 QR，
  不需要异常或 BRK；单个静态 LMS1 v2 / OS v2 帧，752 字节，CRC32。
- 报告明确写 XNU LIVE CHECKPOINT (continues)，记录阶段、调用参数、原 SP、
  boot_args PA 与 GOP PA。它是最后观察点，不是故障 PC 或完整控制台日志。
- 在 arm_vm_init 内细分 10 个调用：early_random、SecureDTLookupEntry、
  两次 SecureDTGetProperty、arm_vm_outside_add、首次 getsegdatafromheader、
  vm_set_page_size、arm_vm_prot_init、qsort、set_mmu_ttb_alternate。
  实际调用地址由固定内核符号和 BL 指令验证，见机器记录的 call_trace。
- 更新同一块区域。3200×2000 显示模式使用顶部 701 行；小屏按尺寸缩放。
  QR 带白色边界；其余早期阶段仍只覆盖顶部 48 行，不累积色条。
- 编码/绘制使用独立 16 KiB 诊断栈与有界堆，每帧重置堆。临时 TTBR0
  设备映射覆盖实际绘制区域，保持 MMU/缓存开启，完成后恢复 TTBR0/TCR。

固定 Apple XNU ac9718fb1af618d5ce8678d0dc6e8a58f252216f 的
osfmk/arm64/arm_vm_init.c 显示该函数会建立新页表并调用
set_mmu_ttb_alternate。只读参考文件 SHA-256 为
9e17da181e72144d27df4ce256988226bfef0e41c0744fc9ef6466f0844c2a32
（精确值以机器记录为准）。具体本机指令布局以 cc690c9a 输入为准。

附加 __KDIAG 段在新页表下的可执行性尚未验证。因此本版在 arm_vm_init
外层与最终切换调用前仅记录进入，随后以原 LR/原栈尾转移到真实函数，
不要求切换后返回附加段。最终切换前恢复原内核 VBAR；保留原 panic
入口字节，不再让切换后的 panic 跳进附加段。

如果最后报告是 vm/set_mmu_ttb_alternate#1，它表示诊断器已到观察边界，
此后没有探针更新；不能据此认定切换函数卡死。切换后的异常/进度需要
下一阶段的内核内建诊断。无默认主动停机、无死循环看门狗。

## 安装与反馈

保留当前 boot_b；在 fastbootd 由用户刷写新配对文件：

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-live-qr-final\vendor_boot.img'
& $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-live-qr-final\vbmeta.img'
```

选原 XNU 项，扫描正常启动画面留下的二维码。若尚未到首次二维码阶段，
记录完整顶部文字；若有异常则扫描异常报告。根卷无需为这次诊断先刷。

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| vendor_boot.img | 100663296 | 644cfe58d909ac691c3490effefbeb9a46ccaa036016274d84b4b6510eb668c1 |
| vbmeta.img | 8000 | 37de2e0fa57c3049bebc9d9b817854f009ad443f50d915118448e1758220b745 |

镜像哈希与验证记录见 [构建证据](evidence/kernel-live-qr-build.json)。
自检已在上一版实机通过；本版正常路径阶段 QR、VM 边界和原始卡点仍待
实机确认。未执行设备刷写、重启、切槽或更改设备文件系统。

云端独立源码检查 run43755493448 success，源提交
67893c542757b1b2e40c97c30903b63bb020b013。AARCH64 对象、主机重复
阶段帧测试及下载产物 digest 核对通过；云端无密钥，未重新 native/prelink。
