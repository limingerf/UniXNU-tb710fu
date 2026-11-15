# 内核日志诊断与"下一步是不是根卷" · 2026-11-15

## 实机日志已取回（本轮）

设备 `_a` 槽 TWRP，pstore 两份 zone 都通过 `adb pull` 取回：

| 文件 | 字节 | 内容 |
| --- | --- | --- |
| `console-ramoops-0` | 132,614 | **Android/TWRP 的 Linux GKI 内核 `6.1.177-android14` 完整启动日志** |
| `pmsg-ramoops-0` | 55,181 | 同一 Android 系统的 userspace 日志（init、keystore2、qwesd、healthd…） |

clean 文本存于 `.local/pstore/console-clean.txt` 与 `pmsg-clean.txt`。

## 关键诊断：XNU 日志没有出现

对 console 全文做关键词检索，结果全部为 **0**：

```
TB710FU / UniXNU / Darwin Kernel / arm_vm_init / IOKit / AppleARM /
panic(cpu / root_device / apfs / IOPlatform / PE_init / com.apple
```

日志从 `[0.000000] Linux version 6.1.127 ... KernelPatch` 开始，到 41 秒的
healthd/dwc3 结束，中间是 qcom 模块列表、F2FS-fs (sda14) 挂载失败、audit、
AVC denial、NVT 触摸驱动、init 的 `Control message`。日志尾部
`ECC: 56 Corrected bytes, 1029 unrecoverable blocks` 说明 zone 有位翻转，
但主体仍是完整可读的 Linux 日志。

**结论：这次 pstore 里是 Android 内核本次启动自己写的 console，不是 XNU 写的。**
XNU 侧 ramoops 补丁的 banner `=== TB710FU XNU boot (UniXNU ramoops) ===`
在 zone 里完全没有残留。

## 可能原因（按概率，需逐个排除）

1. **本次复位后 Android 内核的 pstore 驱动重新初始化了 prz**：Linux 挂载 pstore
   时读取旧内容导出为 `console-ramoops-0`，然后重置 zone 供本次 console 写入。
   我们看到的 132KB 就是 Android **本次**写满的，XNU 那次的内容即便写过，
   也可能在 Android 启动时被当作"上一轮"消费/覆盖。
2. **XNU 的 `ml_io_map(0xac340000, 0x40000)` 没有成功映射**，
   `tb710fu_ramoops_init()` 里 `ramoops_zone_open` 返回失败 → `zone = NULL`
   → 所有 putc 落到 early buffer，从未写进物理内存。
3. **这次根本没进 XNU**（比如复位后直接进了 A 槽 Android/TWRP）。

`ramoops_reset_after=120` 是冷复位（PSCI SYSTEM_RESET），如果它把设备
带回 A 槽 Android，则 1 的情况几乎必然发生。

## 因此：现在还不到"编译根卷"

用户问"下一步是不是要编译根卷"。基于日志和工程事实的回答是 **不是，还差得远**：

### 根卷不是"编译"出来的

macOS 27 的根卷由三部分组成（见官方 `research/unixnu/macos.md` §4–§7）：

| 组成 | 来源 | 本地是否具备 |
| --- | --- | --- |
| 系统卷磁盘镜像（OS/SystemOS/App 三个窗口） | `UniversalMac_27.3_26C5035r_Restore.ipsw` | ❌ 无 IPSW |
| 根策略（签名 manifest + 哈希表） | `scripts/make-root-policies.py` 基于上面镜像生成 | ❌ 无输入镜像 |
| Data seed（`/System/Volumes/Data`） | 以 root 在**真实 macOS 系统**上用 `make-macos-data-seed.py` 制作 | ❌ 无 macOS 主机 |
| userdata 根镜像 | `package.sh` 组装三个窗口 | ❌ 未生成 |

本地只有生成脚本，**没有任何输入数据**。`board.toml` 也没有 `[payload]` /
`[payload.adt]` / `[root]` 配置（此前 ADT 是手工从 live FDT 派生的）。

### 真正的顺序（按依赖）

1. **先拿到 XNU 自己的日志**（当前阻塞）——否则连"内核走到哪一步"都不知道，
   无法判断是停在平台专家、根卷匹配，还是更早的 MMU/PCI 阶段。
2. 内核能跑到"找根卷"这一步之后：
   - ADT 补 `root_match_volume_uuid`（profile 里是
     `84581E99-85BE-4957-AC53-2947E887B3E4`，我们当前 ADT 没有）
   - 构建 userdata 根镜像（需要 IPSW + macOS 主机制 Data seed）
3. 再验 `OS`/`SystemOS`/`App` 三个窗口发布、Data 挂载、cryptex、launchd。

### 最小下一步建议

先解决 ramoops 通道本身，推荐用补丁自带的 `ramoops_selftest=1`：
它让 XNU 一进 `arm_vm_init` 之后就写一条
`ramoops selftest: PSCI reset now, warm=0` 并立即复位，**不依赖内核后续能否跑**。
若复位后 pstore 里出现这行，证明"写入 + 跨复位存活"通了；
若仍不出现，问题在 `ml_io_map` 或复位类型，而非内核走到哪。

同时建议：读取 pstore 必须在 XNU 复位后**立刻**进 fastboot/TWRP 且
阻止 Android 正常启动写 console，否则 Android 的 132KB 会盖掉 XNU 的短日志。
