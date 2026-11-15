# 固定报告与 OneSend 轮播导出

## 实机依据与接收复现

2026-11-16 用户照片确认到达 XNU enter: vm/set_mmu_ttb_alternate#1，
并进一步确认二维码存在，但静止显示时 OneSend 未完成收件。该标记仍是
上一版的切换前观察边界，不能由此判断切换后内核是否继续。照片未完整
包含二维码，未从照片恢复报文；仅保存哈希及转录，不上传照片。

使用固定 makerjackie/onesend@39e95ce59df2f2dc38d2556dd4239cec6a0ca35f
接收实现重放：旧版最后一帧独立输入时成功还原 .log，K=1 单帧即可完成。
同一个接收器先接收较早阶段，再接收最后阶段时，最后阶段因传输配置/
会话改变被忽略。此复现证明旧设计不适合在多个阶段连续发送独立文件，
尚不能认定它就是用户此次相机/应用接收失败的全部原因。
证据见 [照片与复现](evidence/onesend-receiver-lock-audit.json)。

## 本版行为

沿用 cc690c9a 预链接输入，仅生成诊断副本。未重新 native/prelink，
没有修改官方缓存或设备文件系统。自检的终止式报告仍使用已验证的路径。

- 早期阶段只更新文字；到最后的 set_mmu_ttb_alternate 前才生成一份报告，
  避免接收端先锁到前面阶段的文件。报告补充原 SCTLR/TCR/TTBR0/TTBR1，
  该检查点的 ARG0 是将传给切换函数的新页表参数。
- 同一 LMS1 信封、会话、文件校验与单数据块生成四张不同二维码，序号
  0/1/2/3，均为合法 K=1 报文。四张是同一份文件的序号变体，任意一张
  完整获得即可收件，无须凑齐四张。每张有独立帧 CRC32。
- 每 500 ms 更换二维码，显示 FRAME 1/4 至 4/4。提前缓存位图，每帧只绘制；
  编码和等待均使用原页表，只有绘制时短暂安装 GOP 映射并立即恢复。
- 正常 XNU 项轮播 16 次，约 8 秒后继续走原内核调用；真实卡死后同一 CPU
  无法继续刷新。本版没有给真实卡死增加后台绘制线程或看门狗。
- 新增 Export VM checkpoint (animated; intentional pause) 项，使用
  tb-report-export=1，明确有意停在切换前，持续轮播供扫码；此项用于导出，
  不执行页表切换，也不能作为切换后内核运行证据。原默认项不变。
- 阶段二维码放到左侧文字下方；3200×2000 模式为 968×968 像素，M 级纠错，
  覆盖固定顶部 1080 行。小屏自动缩放，不累积色条。自检报告布局保留。

## 安装与接收

保留当前 boot_b；在 fastbootd 由用户刷写配对文件：

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-animated-qr-final\vendor_boot.img'
& $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-animated-qr-final\vbmeta.img'
```

先在 OneSend 重新开始接收，清掉上一轮会话，再选择 Export VM checkpoint
(animated; intentional pause)，观察 FRAME 编号持续变化并接收
TB710FU-live-stage.log。导出项保持暂停；普通 XNU 项约 8 秒后继续。
收到的是切换前真实检查点参数，不是实际故障 PC 或完整控制台日志。
不需要为这次诊断先刷根卷。

## 验证与边界

接收测试使用未修改、SHA-256 固定的实际 OneSend packFrame、
OpticalReceiver.consume 和 decodeTransferFile，验证任意变体、丢失首帧、
重复帧以及最终文件字节完全一致。独立光学解码器验证实际 AARCH64 绘制
位图并把报文送入真实接收逻辑；相机、显示面板和用户应用版本仍待实机。

普通路径需验证轮播后返回；明确暂停路径需验证持续刷新且每次等待前恢复
原 TTBR0/TCR。原调用/私有栈/原 LR/VBAR、异常自然对齐、生产加载器、
LZMA/FAT 与本地签名均单独记录。云端仅检查诊断源、编码与收件，
不重新编译整个 XNU，不使用密钥。

交付哈希、检查结果与云端提交见
[构建证据](evidence/kernel-animated-qr-build.json)。本版实机轮播/收件待验，
原始内核问题未宣称已修复。

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| vendor_boot.img | 100663296 | 28ac87c3ce9754950454a2c6d446285448306789d98f8ef2492bfaa2399c37ea |
| vbmeta.img | 8000 | 0198289981b0db6440e0c4f3f193724bf43b36a6591c0e40c8d5e202e54489bb |
