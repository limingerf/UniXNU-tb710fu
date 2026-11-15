# 白条之后：XNU 内核入口测试 · 2026-11-15

用户已报告加载器顶部色带到白色。它确认 DarwinEnter 已完成缓存/MMU/TLB 操作，
并到达内核跳转前的检查点，尚不能证明 XNU 入口或初始化完成。
本轮不猜测是哪条内核指令出错，先交付真正位于 XNU 中的入口探针。

## 本次镜像

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `out/kernel-entry-test/vendor_boot.img` | 100663296 | `93c81e7d85c2dd9a0ca6b168923ff9b67b1458e366c7ba95596e53e94b4a45df` |
| `out/kernel-entry-test/vbmeta.img` | 8000 | `58aad8707106e03399b17a3680ffa896888a5f5cdde158bd85c900f9555d2dfc` |

XNU 由现有 ramoops 预链接内核 `cc690c9a…` 制作诊断副本，固定官方入口工具只改
两处核实过的跳转与两段空填充区（共 129 字节），副本 SHA-256 为 `e2156f42…`。
没有新编译 native 内核或重做 prelink，不将诊断副本冒充已通过原 prelink 哈希的产物。
来源和原始检查见 [XNU_ENTRY_PROBE.md](XNU_ENTRY_PROBE.md)。

## 一次测试

保留已经能到白条的 `boot_b`。在 fastbootd 下刷入**本次成对的**两个文件：

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-entry-test\vendor_boot.img'
& $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-entry-test\vbmeta.img'
```

回到 UEFI 后选原来的 XNU 项，建议从选中前开始录像。**顶部白条会保留**，
观察的是它下面的第二条、第三条：

| 可见条带 | 本轮能确认的事情 |
| --- | --- |
| 仍只有白条 | 尚无核内入口探针执行/显示证据；仍需查入口跳转、boot_args 读取、探针绘图等 |
| 白条下新增蓝条 | XNU 入口探针已执行；继续查 start_first_cpu 至 MMU 开启前这一段 |
| 蓝条下又新增紫条 | 已到达 XNU 开启 MMU 前，下一阶段查 MMU 开启及后续初始化 |

色条的正向证据定位到阶段；缺少色条不能单独证明某一条指令出错。
不需要先刷根卷，也不依赖强制重启后才能读取的 ramoops。
本探针不新增自动复位；原来的 ramoops 参数保持不变。

## 校验与授权范围

沿用用户已有的仅本地签名授权，`tools/sign_kernel_probe_pair.py` 使用现有 avbtool
和明确选择的 WSL system OpenSSL，不修改工具源文件。
生成新 vendor_boot AVB footer，并**原位替换**原 vbmeta 的 vendor_boot hash descriptor。
完整 256 字节 vbmeta header（含 flags、rollback 等）、公钥、另外 22 个 raw descriptor
及它们的次序逐字节不变。描述符仍是 13 prop、3 hash、4 hashtree、3 chain。
两份镜像签名通过，vendor_boot 内容 hash 和 vbmeta 引用相匹配。

从已签名 vendor_boot 重新读出 FAT 中的 LZMA 内核，raw 流解压后的哈希为
`e2156f42d6d30b9b91d04b8be1d1d86a03d84fdecba560d9587f70b1ce5db524`，
与守卫检查后的副本一致；ADT、unixnu.toml 哈希未变，FAT32 66512 簇检查通过。
同版本生产 Mach-O 解析器对副本通过。原始内核文件未改。

本轮没有操作设备，实机结果仍待用户测试；不是“内核故障已修复”的结论。
机器记录见 [kernel-entry-test-build.json](evidence/kernel-entry-test-build.json)。

需要回到原 payload 时，成对使用原来的 `out/lzma-keys/vendor_boot-ecc0.signed.img`
（`836ca997…`）与 `out/lzma-keys/vbmeta-ecc0.img`（`3992de82…`）。
