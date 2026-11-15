# 绿条停屏与入口探针 · 2026-11-15

用户本轮确认：屏幕实际有绿色横条；最后文字包含 DarwinBoot 的
`CurrentEL`、CPU ID、`Exiting boot services` 和 UEFI 的
`LockCapsuleInterface`。返回 UEFI 使用长按电源强制重启，中间没有进入
Android/TWRP；日志查看器报告 `sig 4107024 start 1072 size 6808`。
签名按用户原文记录，未擅自补零或认定其进制。

## 当前能确认的阶段

同版本 DarwinBoot `5243c68ec332a227fe3680cd5da6d5a0e127035c` 中：

- `DarwinBoot.c` 的寄存器/CPU ID 输出在 ExitBootServices **之前**，属于加载器。
- `LockCapsuleInterface` 是 ExitBootServices 的 UEFI 回调，本身不是 XNU 输出。
- ExitBootServices 成功后才调用 `DarwinEnter`；`Entry.S` 在该函数中先画绿条，
  然后做缓存维护、关闭 MMU、跳转内核。

所以**实际绿条证明 ExitBootServices 已返回、DarwinEnter 已开始**，
尚不能证明 `br x1` 已执行或 XNU 入口已运行。只有提示文字而没有实际绿条时，
甚至不能仅凭该提示断定 ExitBootServices 已返回。

此前把加载器的 `CurrentEL` / CPU ID 输出当作 XNU 执行证据的推断撤回。
旧文声称的独立 `Darwin Kernel Version` 横幅若要作为另一轮实机证据，需单独
核对原始屏幕和镜像身份；不能把本轮所抄文字算成该横幅。

## 为什么暂不刷根卷

根设备等待、根卷匹配/挂载失败是更后面的阶段。目前没有已到该阶段的证据。
先做入口定位，避免把存储和用户态引入尚未定位的交接问题。

`sig` 不是预期 `0x43474244`，因此日志查看器拒绝解码是符合检查逻辑的；
这不证明内核没有执行。长按电源复位不保证 DDR 保留，且现有 ramoops 只有在
`arm_vm_init()` 返回后才初始化。未到那里或复位后数据丢失都可能得到无效头，
目前没有证据在两者间作出确定归因，单改 ECC 不能修复错误的签名。

## 诊断副本

复用已记录的预链接内核 `cc690c9a…`，不重新 native 编译或预链接：

```bash
python3 tools/prepare_entry_probe.py \
  --framework .work/UniXNU \
  --kernel .local/prelink-ramoops/kernel/kernel.macho \
  --sha256 cc690c9a0bde84c49e9cebccc288f393393d16da56620d580108f0a62692b31c \
  --output out/entry-probe-ramoops/kernel-handoff-probe.macho --lzma
```

工具调用固定官方 `scripts/patch-xnu-entry-probe.py --no-reset`，不编辑原脚本。
唯一适配是过滤 `llvm-nm` 的 fileset 标题行，原有指令、空填充区和跳转距离
检查均保留；缺少匹配的指令便拒绝生成。源内核保持不变。

| 可见色带 | 能确认的阶段 |
| --- | --- |
| 只有绿色 | 加载器 DarwinEnter；尚无 XNU 入口探针执行证据 |
| 新增蓝色 | XNU 的入口探针已执行 |
| 再新增紫色 | 到达 XNU 开启 MMU 前；不等同于 MMU 已成功开启 |

不新增自动复位探针，不依赖 ramoops 的跨复位保留。颜色应在同一次停屏时观察，
不是长按电源之后观察旧帧。若没有新色带，还需排除探针镜像未安装或绘制未能显示，
不能单凭缺色就把某条指令认定为故障点。

## 本地验证与产物

- 原始内核 SHA-256：`cc690c9a0bde84c49e9cebccc288f393393d16da56620d580108f0a62692b31c`。
- 诊断内核 SHA-256：`e2156f42d6d30b9b91d04b8be1d1d86a03d84fdecba560d9587f70b1ce5db524`。
- 独立逐字节检查：129 字节变化全部位于两处已核实的跳转指令和两段原本为空的填充区。
- 同版本、未打 LZMA 补丁的生产 Mach-O 解析器直接解析诊断产物通过。
  本地曾先用含 LZMA 扩展的验证副本运行旧测试，测试提取范围包含了额外函数而失败；
  改用固定原始源码后通过，这不是诊断内核的编译失败。
- LZMA 编码前后及 raw 流往返一致，长度头供现有 EDK II 解码器使用。
- 从当前 ecc0 vendor_boot 复制 payload，仅替换其中的 kernel 文件；配置、ADT
  逐文件哈希不变，FAT32 66,512 簇检查及 kernel 读回通过。

本地产物位于 `out/entry-probe-ramoops/`：`kernel-handoff-probe.macho`、
`kernel-handoff-probe.lzma`、`payload-entry-probe.img`、
`vendor_boot-entry-probe.unsigned.img`。最后一个仅去掉外层旧 AVB 元数据，
保留原始 header、DTB、table、bootconfig 等其余有效载荷字节；它是**未签名输入**，
安装前需要现有 AVB 流程生成新签名和匹配的 vbmeta vendor_boot hash。

本轮未签名、未刷写、未重启或切换设备槽位。探针尚未实机验证，不是新的正式
预链接验收产物。机器证据见 [xnu-entry-probe.json](evidence/xnu-entry-probe.json)。
