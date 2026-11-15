# 交接检查点固件：一次测试 · 2026-11-15

本版新增绿条之后的橙/黄/青/白检查点，区别于之前只有绿条的固件。
用户已明确允许仅在本地使用现有测试密钥签名；没有自动刷写或重启设备。

已签名文件：`out/handoff-stages/boot.img`（100,663,296 字节）。
SHA-256：`a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`。
内嵌 FD：`5722252b40e37d6c94e252c8daeddd9f3499b5483bca44f0cba93c795fb78a71`，
来源云 run43729209898，构建源码 `0ea7095d5d99a8d66b4ff8b77bbdf9268e6cfd52`。

RSA 签名、内容 hash、签名前后 payload 不变、内嵌 FD 读回、现有 boot chain
公钥匹配均通过。测试密钥不入库；沿用现有 rollback index `1736035200`。
设备行为尚未验证。

在之前刷镜像所用的 **fastbootd** 中，只需刷 `boot_b`：

```powershell
& "C:\Program Files (x86)\platform-tools\fastboot.exe" flash boot_b "D:\unixnu-tb710fu-clean\out\handoff-stages\boot.img"
```

沿用当前匹配的 boot chain，vendor_boot、vbmeta、根卷不需要随本变体更新。
手动启动回 B 槽 UEFI 后选原来的 XNU 项，观察同一条顶部色带最后停在哪个颜色，
最好从选中前开始录像；直接记录该次停屏，不依赖强制重启后的 DDR 内容。

| 最后颜色 | 下一段需要排查的区间 |
| --- | --- |
| 绿 | 数据缓存维护循环/屏障 |
| 橙 | 指令缓存失效/屏障 |
| 黄 | 关闭 MMU、后续取指 |
| 青 | TLB 维护/屏障 |
| 白 | 内核跳转或内核最早期；需再用独立的内核入口探针细分 |

这些颜色缩小停点，不能单独证明某条指令故障。若没有变化，应先核对安装的文件身份。
本变体没有新增定时重启，也没有替换 XNU；原有内核参数中的 120 秒复位机制保持原样。

需要回到旧固件时，已有 `out/log-viewer/tb710fu-boot-uefi-logviewer.signed.img`
（SHA-256 `3413c36b…`）为原来的只读文字日志查看器载体。
完整验证记录见 `docs/evidence/uefi-handoff-stages-build.json`。
