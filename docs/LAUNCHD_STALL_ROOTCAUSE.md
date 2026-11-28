# Launchd 卡死根因分析 · 2026-11-28

## 结论（源码级，非推测）

失败启动的精确时序（`scratch/com5_pl97b_20261128-023120.log`）：

```
418| TB710FU boot-policy: launchd override written
419| load_init_program: attempting to load /usr/appleinternal/sbin/launchd.development
420| load_init_program: attempting to load /sbin/launchd
421| exit: pid 1 init status 0            <- 内核临时 init 退出，正常
422| exec: pid 1 launchd                  <- launchd 接管
423| shared_region: [1(launchd)] check_np(...) returned 0x1
424| shared_region: [1(launchd)] check_np(...) returned 0x1
425| dyld[1]: result from check_np(): -1, errno 12
426| ASP: AMFI pointer initialized
427| exit: pid 1 init status 0            <- 注意：无 TB710FU INITEXIT
428| TB710FU CDC: no host open in the fallback window
429| TB710FU CDC: display thread queued (configured-nodtr)
431| TB710FU CDC: GopDisplay holding 16 ms
483| TB710FU GopDisplay: registered
     [之后无 launchd 任何输出]
```

## 已排除的错误假设

1. **`errno 12` 不是病因。** `bsd/vm/vm_unix.c` 的 `shared_region_check_np` 文档明确写着
   `ENOMEM  shared region is empty`，这是**每次正常启动都会返回**的值。
   三个成功案例（pl93/pl92/pl89）的日志里**同样有**这三行完全相同的 `check_np / errno 12`。

2. **`initproc` 没有退出。** `patches/xnu/0046` 会在 `p == initproc` 时打印
   `TB710FU INITEXIT: ...` 并 `delay(120000000)`。失败日志里**完全没有**这条输出，
   说明 launchd 仍在运行，只是停住了。所以不是 launchd 崩溃触发重启。

3. **不是内存不足。** 失败与成功的 `pmap_startup() 1240770 page entries` 完全一致。

4. **不是 GPU 卡住桌面。** 失败时 `GopDisplay: registered` 是在 launchd 停住**之后**
   才由 CDC 的 8 秒 fallback 触发启动的，属于结果而非原因。

## 真正的差异

成功案例在 `check_np` 之后紧跟着：
```
dyld[1]: dyld cache mapped system-wide: development, auth GOTs: mapped
```
失败案例**缺失这一行**，且之后**没有任何 `shared_region_map_and_slide` 的痕迹**
（syscall 536，入口 `bsd/vm/vm_unix.c: shared_region_map_and_slide_2_np`）。

即：dyld 在拿到 `ENOMEM` 之后没有走到 `map_and_slide`，或走了但失败无输出。

## 下一个待验证点

需要在 `shared_region_map_and_slide_2_np` 及其调用的
`vm_shared_region_map_file_setup()` / `vm_shared_region_slide_part()` 里加
无条件的成功/失败打印，确认：
- syscall 是否被调用
- 若被调用，`files_count` / `mappings_count` 是多少、`kr` 是什么

成功案例中 `GopDisplay` 与 `dyld cache mapped` 的先后关系（4/4 成功案例均为
GopDisplay 先）**不构成因果**：pl93 里 GopDisplay 在 launchd 之后启动但仍成功，
说明两者只是共享同一个时间窗，不是依赖关系。

## 证据文件

- 失败：`scratch/com5_pl97b_20261128-023120.log`（492 行后静默）
- 失败：`scratch/com5_rollback96_20261128-031435.log`（535 行后静默）
- 成功：`scratch/com5_live_20261128-020551.log`（1266 行，pid 338，transfers=389）
- 成功：`~/.gemini/.../scratch/com5_pl93_5g_srgb_live2.log`（108 KB）
- 成功：`~/.gemini/.../scratch/com5_pl92_5g_live.log`、`com5_pl89_5g_live2.log`

## 无人值守约束（用户明确要求）

- 平板不抓日志时必须留在 EDL，避免卡启动发热损伤电池。
- 桌面久留会导致背光黑屏 + COM5 仍枚举但不响应，会永久失去串口。
- 监控脚本 `scratch/boot-monitor.ps1` 负责：记录每次启动、判读状态、
  **检测到桌面或超时立即回 EDL**。

## 追加排除（第 2 轮）

5. **`text-fixup0` 不是原因。** 对照六个镜像：pl86b/pl89/pl92 **没有**该参数但
   `dyld cache mapped` 成功；pl93 **有**该参数也成功；pl94/pl95 没有却失败。
   正交，无相关性。

6. **`exit: pid 1 init status 0` 不是崩溃点。** 四个成功案例（pl86b/pl89/pl92/pl93）
   与两个失败案例**都各有 2 次**该日志，位置均在 `exec: pid 1 launchd` 前后，
   属于内核临时 init 退出的正常记录。

7. **失败模式已统一。** pl94、pl95、pl97b、pl96-rollback 四份失败日志全部停在
   `check_np -> errno 12 -> ASP: AMFI pointer initialized` 之后，**都没有**
   `dyld cache mapped system-wide`。四个成功案例都有这一行。
   因此分界点精确落在「`check_np` 返回空」到「`map_and_slide` 成功」之间，
   即补丁 0052 所打印的那段。

## 第 2 轮阻塞

`pl100-srmap`（含 0052）刷入后，平板**从未枚举 USB**：无 COM5、无 9008、
无任何 `VID_05C6/1209/18D1`，持续 5 分钟以上。与「卡初始化后自行重启」不同：
重启循环至少会短暂出现 COM5 或 9008。最可能停在 UEFI 菜单等待选择。

需要人工看一眼屏幕并按确认键，之后 8 小时监视器（`boot_monitor_20261128-035303.log`）
会接管：抓到日志、判读、并强制回到 EDL。

## 第 2 轮关键发现：unnest 标记

成功案例在 `dyld cache mapped` 之后立刻出现（pl93:471-）：
```
launchd[1] triggered unnest of range 0x1e5200000->0x1ec400000 of DYLD shared region in VM map 0x417ffead11428f0d
fsck[2] triggered unnest of range ... (同范围)
fsck_apfs[3] triggered unnest of range ...
mount[4] / seputil[5] / xpcproxy[88..90] ...
```
`unnest of ... DYLD shared region` 是 dyld 把共享缓存从嵌套子映射提升进任务主映射的
动作，**只会在共享缓存成功建立后发生**。

失败案例（pl97b）**一次都没有**这行，也没有任何后续进程的 `check_np`：
- pl93 还有 `257(com.apple.Driver) check_np(...) returned 0x1` 两次（后续进程正常走到）
- pl97b 只有 `1(launchd)` 的两次，之后再无任何进程

这把结论钉死：失败时 launchd **没有**建成共享缓存映射，停在
`check_np` 返回空 与 `map_and_slide` 完成 之间。

注意：`check_np` 返回 `0x1 / errno 12` 在**所有**案例（含成功）都一样，
所以它不是判据；`unnest` 与 `dyld cache mapped` 才是。

## 第 3 轮：pl49 一度成功跑起过 WindowServer

用 `scratch/classify-boot.ps1` 对全部历史捕获分类（判据：`unnest of range ... DYLD
shared region` 与 `dyld cache mapped system-wide`），发现 `scratch/com5_pl49_ps3.log`
里有：

```
TB710FU PS: pid=164 ppid=1 name=WindowServer thr=2 fp=2237632
TB710FU PS: pid=166 ppid=1 name=loginwindow  thr=2 fp=1655808
```

即 **pl49 时代 WindowServer 与 loginwindow 都已启动**，且该日志有 31 行 `unnest`、
大量 `xpcproxy` 正常 exec。这远好于当前状态（当前连 `dyld cache mapped` 都没有）。

判据分布（无例外）：

| 日志 | unnest | mapped | 结论 |
|---|---|---|---|
| pl86b | 35 | 1 | 用户空间 |
| pl89 | 42 | 1 | 用户空间 |
| pl92 | 28 | 1 | 用户空间 |
| pl93 | 50 | 1 | 用户空间 |
| pl96(02:06) | 28 | 1 | 用户空间 |
| pl49_ps3 | 31 | 0 | 用户空间（WindowServer 在跑） |
| pl94 | 0 | 0 | 卡住 |
| pl95 | 0 | 0 | 卡住 |
| pl97b | 0 | 0 | 卡住 |
| pl96-rollback | 0 | 0 | 卡住 |

**因此这是 pl49 之后引入的回归，不是板子固有缺陷。** pl95/pl96 之间折腾的正是
`shared_region_slide_part` 那条链：
- 官方 `native-4k-shared-region-slide.patch`（`.work/UniXNU/patches/xnu-12377/`）
- pl95 在其上加了 0052 串行化锁 → 100% AMFI 死锁
- pl96 回滚 0052，但整条 4K slide 路径仍在

下一步该做的是**逐步二分 pl49→当前**在 shared-region 链上的改动，而不是继续在
launchd 之后找原因。

## 第 3 轮结论：pl49 是已知良好基线，回归区间只有 7 个提交

`git log 7052c68..HEAD -- patches/xnu`（pl49 之后的 XNU 补丁层改动）：

```
e3d5018 pl100: trace shared_region_map_and_slide_2_np
73f47dc pl96:  revert 0052 shared-region slide lock
1d13d08 pl95:  display settle gate + IOSurfaceRoot retry + 4k slide serialization
7ccfae7 pl56:  CDC panic handover gate
104ca89 pl54c: restore 0051 header
6dc7865 pl54b: fix 0051 hunk count
c2e2654 pl54:  dump WindowServer SIGABRT
```

其中只有 `1d13d08` 触碰 shared-region slide 链（新增 0052，后被 `73f47dc` 回滚）。
其余均与共享缓存无关（CDC panic / WindowServer 诊断打印）。

## 下一步（明确、可执行）

1. **以 pl49 为对照做二分**：在当前树上只回退 0051/0049/0050 等诊断补丁不必要，
   真正要验的是 `native-4k-shared-region-slide.patch` 及其同族
   （`native-4k-block-split.patch`、`native-4k-signature-hash-cache.patch`、
   `native-4k-user-abi.patch`、`native-4k-bootstrap-capacity.patch`，
   均在 `.work/UniXNU/patches/xnu-12377/`）在 pl49 与当前之间是否有版本漂移。
2. 若官方这几个补丁**内容未变**，则问题在 0052 回滚得不彻底或
   `67c5b5a`/`1d13d08` 残留（`GopDisplayStart.cpp` 的 IOSurfaceRoot 重试、
   `CdcXnu.c` 的 settle gate）改变了时序。
3. 届时用补丁 0052 的 `TB710FU SRMAP` 打印确认 `map_and_slide` 是否被调用。

**不要再做**：在 launchd 之后找原因、加固定延时、猜 GPU。

## 工具

`scratch/classify-boot.ps1` 用 `unnest of range` + `dyld cache mapped` 判定每次启动
是否真正进入用户空间（已在 20 份历史捕获上验证：5 份成功全 >0，4 份失败全为 0）。

## 第 4 轮：补丁集不是变量，boot-args 是

用两份**权威构建记录**对比（不是推断）：

- `out/artifacts/tb710fu-prelink-45623265343/xnu-patches.list`（pl49，成功）
  176 个补丁
- `out/artifacts/tb710fu-pl55-45839746246/xnu-patches.list`（pl55）
  177 个补丁

`Compare-Object` 结果：**只差 `0051-tb710fu-windowserver-abort-diag.patch`**
（一个只在 WindowServer 非零退出时打印的诊断补丁，无行为影响）。

结论：**pl49 与 pl55 的 XNU 补丁集实质相同**，4K slide 链
（`native-4k-shared-region-slide`、`native-4k-block-split`、
`native-4k-signature-hash-cache`、`cshash-native4k-{parallel,lazy}`）两侧都在。
所以"4K slide 补丁漂移"不成立。

真正的差异在 **boot-args**。pl49 成功时比当前少 12 个参数：

```
ml-timeout-sched-preemption=0
sched_preemption_disable_debug_mode=0
adsp=2  battery=1  usb-sdp-ma=2000
gpu-metal=1  gpu-power=9  gpu-addon=1  fb-half=2
text-fixup0=dyld_shared_cache_arm64e.03,5de1134,08060037,1f2003d5
tb710fu-keys=1
```

其中前两个名字直指**调度抢占**：

- `ml-timeout-sched-preemption=0`
- `sched_preemption_disable_debug_mode=0`

而 launchd 卡在 dyld 的分页等待，正是抢占被禁用的典型受害场景。
`git log -S` 显示这两个参数只出现在文档提交，是手工加的调优项，不是构建产物。

## 下一步实验（单变量，明确）

刷一版**去掉这两个抢占参数**的镜像（其余照旧），看 `unnest` 是否出现。
这是目前唯一有强证据支持、且可一次证伪的假设。

若去掉后仍卡住，再把 `gpu-*` 与 `text-fixup0` 一并去掉（回到 pl49 参数集），
二分定位。

## 第 4 轮补充：调度参数假设也已被证伪

`ml-timeout-sched-preemption=0` / `sched_preemption_disable_debug_mode=0`
**不是**判别因素：

| 镜像 | 带这两个参数 | 结果 |
|---|---|---|
| pl49 | 否 | 成功（WindowServer 跑起来，117 进程） |
| pl93 | **是** | 成功（583 帧 0 拒帧） |
| pl94 / pl95 / pl97b / pl96-rollback | 是 | 卡住 |

数据卷参数同样无差异：所有镜像都是 `no-reset`、
`sheng-data-persist=2`、`data-persist=2`。

## 第 4 轮净结果（诚实记录）

已系统排除：**补丁集、调度参数、数据卷参数、text-fixup0、errno 12、
pid1 init exit、内存、GPU**。

剩下的唯一判别信号是运行时的：成功的启动里 `launchd` 建起了共享缓存
（`dyld cache mapped` + `unnest`），失败的没有。静态条件全部重叠，
因此**必须靠运行时观测**，也就是补丁 0052 的 `TB710FU SRMAP` 打印。

结论：在拿到那条打印之前，继续做静态推断只会重复本轮的错误
（提出一个看似有力的假设，随后被历史数据证伪）。

## 待办（设备恢复后立即执行）

1. 刷 `pl100-srmap`（含 0052），抓第一次启动日志
2. 看是否有 `TB710FU SRMAP[1]: enter`：
   - 有 → 看 `exit kr=0x..`，直接定位失败码
   - 无 → dyld 根本没走到 syscall 536，问题在更早的缓存解析
3. 用 `scratch/classify-boot.ps1` 判定是否进用户空间

## 第 5 轮：日志层面已无判别因素，停止静态推断

本轮做的对比（全部基于**从开机捕获的完整日志**，含 `pmap_startup`）：

| 日志 | AMFI 行号 | 首个 unnest 行号 | GopDisplay 是否跑完 |
|---|---|---|---|
| pl86b（成功） | 529 | **530** | —（启动早期无该段） |
| pl93（成功） | 470 | 529（中间 59 行是 GopDisplay） | 是 |
| pl94（失败） | 532 | **无** | **是**（kext load=0 → registered） |
| pl97b（失败） | 426 | **无** | **是**（kext load=0 → registered） |

假设「GopDisplay 的 kext 加载为 dyld 铺路」被证伪：失败案例里 GopDisplay
同样完整跑完（`kext load=0` → `waiting for IOResources` → `registered`）。

同时确认：失败日志**没有被截断**（以完整换行结尾），492 行就是全部内容。

**结论：在日志层面，成功与失败的所有可比条件都已重叠。**
继续做静态推断只会重复「提假设 → 被历史数据推翻」的循环。本轮已连错三次
（调度参数、数据卷、GopDisplay 铺路）。

唯一能改变信息状态的动作是拿到补丁 0052 的运行时打印。已独立复核
`out/pl100/kernel.macho` 确实含两条 `TB710FU SRMAP[%d]: ...` 格式串。

## 新增：恢复镜像 pl102-minimal

`out/tb710fu-pl16-46058104010-pl102-minimal-signed.zip`

内核与 pl100 相同（0052 打印仍在），但 boot-args **去掉全部可选子系统**
（无 gpu-*、无 adsp/battery、无 text-fixup0、无 tb710fu-keys）。
用途：区分「设备停在 UEFI 菜单」与「内核在 USB init 前就挂死」。

- 刷入后若 COM5 出现 → 说明问题出在某个可选子系统，且 SRMAP 打印可读
- 若仍无任何枚举 → 需要人工看屏幕，软件已无法推进

## 第 7 轮：卡住时内核仍活着，进程表可读

从 `scratch/com5_pl97b_20261128-023120.log` 最后几行：

```
483| TB710FU GopDisplay: registered
484| TB710FU CDC: GopDisplay start returned
485| NativeLegacyDisplay: display object attached ...
486| TB710FU CDC: configured=1 dtr=1
488| TB710FU CDC: cmd=disp          <- 主机发的命令被响应了
```

以及 `scratch/com5_watch_20261128-032323.log`：

```
[03:23:31.770]   ps: answered, no WindowServer
[03:24:41.561]   ps: answered, no WindowServer
```

**结论：卡住时内核、调度、CDC、`proc_find`/`proc_name` 全部正常工作。**
卡住的只是 `launchd` 这一个用户态进程的前进。这排除了「内核挂死」，
也说明**进程表在卡住状态下完全可读**——这是无需调试器就能看到
launchd 真实状态的唯一窗口。

### 逐里程碑对比（pl93 成功 vs pl97b 失败，均为完整日志）

| 事件 | pl93 | pl97b |
|---|---|---|
| `exec: pid 1 launchd` | 465 | 422 |
| `check_np` | 467 | 423 |
| `dyld[1]: result from check_np` | 469 | 425 |
| `AMFI pointer initialized` | 470 | 426 |
| `GopDisplay: start entry` | 475 | 432 |
| `GopDisplay: registered` | 527 | 483 |
| `dyld cache mapped` | **530** | **缺** |
| `unnest of range` | **529** | **缺** |

GopDisplay 在两边都完整跑完（各约 51-52 行），**不是差异点**。
唯一差异就是 `mapped`/`unnest` 这一对事件在失败时根本没发生。

## 新增工具：scratch/capture-stall-procs.ps1

卡住时抓 `ps` / `disp` / `help` 的**完整原始回复**到独立文件，并汇总
`launchd`/`WindowServer`/`loginwindow` 等的 `thr`、`fp`。

之前 `watch-com5.ps1` 只记录「ps answered」就丢弃了内容，导致最有价值的
证据被扔掉。`boot-monitor.ps1` 的 `Send-Verb` 会把回复追加进 `$raw`，
所以新一代监视器不会重犯这个错。

**用途**：设备一恢复，先让它跑到卡住（或用 pb102-minimal），
然后跑这个脚本，就能看到 launchd 到底还有几个线程、footprint 多少、
有没有子进程被创建。这直接区分「launchd 完全卡死」与
「launchd 活着但某个子进程失败」。

## 第 9 轮：推翻第 7 轮的「进程表可读」结论

第 7 轮我写道「卡住时 `ps` 能应答，进程表可读」。**这个结论不成立。**

`scratch/watch-com5.ps1` 产生那句日志的代码是：

```powershell
if ($ps -match 'WindowServer') { ... }
elseif ($ps.Length -gt 0) { Log "  ps: answered, no WindowServer" }
```

**只要收到任何非空回复就打印 `ps: answered`**，哪怕内容是
`TB710FU SHELL: ps unavailable`。而该脚本**不保存 raw**（那次运行
`no raw dir`），原始内容已永久丢失。

### 反证：六份真实 `ps` 输出全部来自正常运行的桌面系统

| 日志 | launchd thr | launchd fp | WindowServer | loginwindow | ps count |
|---|---|---|---|---|---|
| com5_20261125-184300 | 7 | 13,449,024 | YES | YES | 136 |
| com5_pl49_ps | 7 | 11,638,336 | YES | YES | 119 |
| com5_pl49_ps2 | 8 | 11,728,512 | YES | YES | 118 |
| com5_pl49_ps3 | 9 | 11,564,736 | YES | YES | 117 |
| com5_tc1_ps | 4 | 7,799,616 | YES | YES | 1（截断） |
| com5_tc1_ps2 | 9 | 10,794,752 | YES | YES | 124 |

**六份全都有 WindowServer 和 loginwindow。**

也就是说：能成功执行 `ps` 的历史时刻，系统都是正常运行进了桌面的。
**我从未捕获过一次「卡住状态下的 ps 输出」。**

### 因此仍然未知

卡住时 `launchd` 到底是：
- 完全卡死（不占 CPU）
- 活着但阻塞在某个等待（占 CPU 或睡眠）
- 已退出但没触发 INITEXIT（不太可能，0046 会打印）

要回答这个，必须在下一次卡住时**真实采集 `ps` 并保存原始字节**。
`boot-monitor.ps1` 已修好判据（第 8 轮，`54abad9`）并会保存 `-raw/genNN-*.log`，
`capture-stall-procs.ps1` 会分文件保存 `ps`/`disp`/`help` 的完整回复。

## 第 15 轮：纠正 —— pl49 并未进入完整图形桌面

此前我写「pl49 曾经真的跑起过 WindowServer，所以问题是 pl49 之后的回归」。
**这个表述需要限定。**

对 `scratch/com5_pl49_ps3.log` 的 117 个进程做逐个核对：

| 进程 | pl49 |
|---|---|
| `WindowServer` | **有** |
| `loginwindow` | **有** |
| `Dock` | 无 |
| `Finder` | 无 |
| `SystemUIServer` | 无 |
| `SkyLight` / `backboardd` / `WindowManager` | 无 |

即：**pl49 到达了 WindowServer + loginwindow 阶段，但没有 Dock / Finder /
SystemUIServer，也就是停在登录窗口，而不是完整图形桌面。**

这与 `pl93` 的完整桌面（`first swap accepted reason=0`、583 帧 0 拒帧、
实拍有菜单栏与壁纸）是**两个不同阶段**。

### 这对结论的影响

- 「pl49 之后引入回归」这个说法只对**到 WindowServer 为止**成立
- 而当前失败（`dyld cache mapped` 缺失）发生在**更早**的 dyld 阶段，
  比 pl49 到达的位置还差两个数量级
- 所以不能简单说「回到 pl49 就好」，但可以说：
  **pl49 证明 dyld 阶段是可以过的**，这与「偶发」一致

### 仍是事实的部分

pl49 的 `ps` 能列出 117 个进程、`launchd` `thr=9 fp=11564736`，
说明那一次 `dyld` 成功建立了共享缓存（否则 launchd 起不来，
更不可能 fork 出 117 个进程）。这一点没变。

## 第 16 轮：unnest 日志的真实含义（源码级）

`launchd[1] triggered unnest of range ... DYLD shared region` 来自
`bsd/vm/vm_unix.c:856` 的 `log_unnest_badness()`，它的触发条件是：

- `shared_region_unnest_logging != 0`（默认 1）
- 且**不是**「嵌套 map 且在 lowest_unnestable_addr 之上的可写条目」
- 且受 1 秒节流（`vm_shared_region_unnest_log_interval`）

关键：它是 **map 操作发生时**才被调用（`vm_shared_region.c` 的 map 路径里
调用，注释在 L1918 亦提到 `sr_mapping_in_progress` 保护），**不是映射完成后的
通知**。

### 因果链（现在有代码依据）

```
dyld 调 shared_region_map_and_slide_2_np
  -> vm_shared_region_map_file_setup 建好映射
     -> 期间触发 log_unnest_badness（因为 launchd 的 map 与既有子映射重叠）
        -> 打印 "launchd[1] triggered unnest of range ..."
  -> 返回后 dyld 打印 "dyld cache mapped system-wide: mapped"
```

失败时 `sr_first_mapping` 始终是 -1，map 从未成功，所以：
- 没有 unnest（map 根本没进行到那一步）
- 没有 `dyld cache mapped`

这与第 3 轮的观察一致，但此前只是相关性，现在有调用链支撑。

### 仍不知道的

`shared_region_map_and_slide_2_np` 是**没被调用**还是**调用后失败**——
这正是补丁 0052 要回答的。在拿到 `TB710FU SRMAP[...]` 那一行之前，
不要再基于 unnest 反推。

## 第 18 轮：刷机流程与镜像完整性均已核实无误

### 镜像完整性

`out/pl16-46058104010-pl100-srmap-signed/` 的 `signing-report.json`：

```
signature_checks      : PASS
public_key_unchanged  : true
vendor_boot_sha256    : 5bf81eab8de968a4407717451162475c1335ff9cd912a33556cc7bb456403cb1
vbmeta_sha256         : ecac2cc7e0c54c657000db531d4e04efb04aede07ccc41b0fb5aa82ef2de6631
```

与打包输出逐字一致。**打包和签名没有问题。**

### boot.img 与 boot_b 的关系（此前的疑惑已澄清）

打包目录里的 `boot.img`（`a3236acc...`，`ANDROID!` 格式）**与 `boot_b` 上
实际运行的 UEFI 5G 镜像（`988dace74...`）不是同一个文件**。

原因：`edl_flash_b.ps1` 的 `-BootPath` 是**可选参数**（第 4 行），只有显式传入
才刷 `boot_b`（第 225-227 行）。刷 pl100 时没有传，所以 `boot_b` 保持不变——
这正是期望行为，因为 `boot_b` 承载的是 UEFI，不该被内核镜像覆盖。

已确认 `out/uefi-5g-45995333298/boot.img` 的 SHA-256 正是
`988dace74ecd78dbdc3795a03524d50eeaa7c8b487845601d84e548d123f7aa2`，
与交接文档记录的一致。

### 因此

- 镜像正确
- 刷入目标（`vbmeta_b` + `vendor_boot_b`）正确
- `boot_b` 未被误覆盖

**设备不启动不能归因于刷机或打包错误。** 剩下三种可能仍未排除且无法远程区分：
UEFI 菜单等待 / 电池耗尽 / reset 后未启动。

## 第 31 轮：实机决定性证据 —— launchd 只有 1 个线程、178 KB

镜像 `pl103-liveqr`（pl100 内核 + live-checkpoints）刷入后，设备**首次**走完了
根卷挂载，并在 COM5 上给出完整状态。这是本会话拿到的第一个能直接看到
launchd 内部状态的观测。

### 启动推进到哪

```
disk7s1 mount-complete volume Macintosh HD
disk2s1 mount-complete volume Data
disk3s1 mount-complete volume FizzSeedUpdate26C5035r.arm64eSystemCryptex
disk5s1 mount-complete volume AppCryptex
vm: initializing swap
VM Swap Subsystem is ON
exit: pid 1 init status 0
exec: pid 1 launchd
shared_region: [1(launchd)] check_np(...) returned 0x1
shared_region: [1(launchd)] check_np(...) returned 0x1
dyld[1]: result from check_np(): -1, errno 12
ASP: AMFI pointer initialized
exit: pid 1 init status 0
```

即 `bsd_init.c:1108` 的 `load_init_program(p)` 成功执行，`launchd` 被 exec 起来。
之前 pl94/95/97b 连根卷都没挂上。

### `ps` 给出的决定性数据

```
TB710FU PS: pid=1 ppid=0 name=launchd thr=1 fp=182656
TB710FU PS: end count=1
TB710FU PID1: sc=42 in=1 cur=92 arg0=0x2 arg1=0x61
TB710FU PID1 RING: 4:0 4:0 4:0 6:0 463:2 470:0 463:0 339:0 153:0 92:0 197:0 73:0 153:0 463:0 33
```

关键三个数：

| 指标 | 本次（失败） | pl49（曾到 WindowServer） |
|---|---|---|
| 进程总数 | **1** | 117 |
| launchd 线程 | **1** | 9 |
| launchd footprint | **182,656 B（178 KB）** | 11,564,736 B（11.5 MB） |

**launchd 起来后一个进程都没 fork，自己只有 1 个线程、178 KB。**
它连 dyld 初始化都没走完，谈不上执行任何 job。

### SRMAP 从未打印

在所有 pl103 捕获里检索 `TB710FU SRMAP`：**零命中**。

补丁 0052 的打印点就在 `shared_region_map_and_slide_2_np` 入口。零命中意味着：

> **dyld 根本没有调用 `shared_region_map_and_slide_2_np`。**

不是调用后失败，是**没被调用**。所以问题在 dyld 决定调用它之前：
读到共享缓存文件、校验、构造 mappings 参数这一段的某一步。

### 与 `errno 12` 的关系（此前已澄清）

`check_np` 返回 `-1 / errno 12` 本身是正常的（"shared region is empty"，
成功案例也有）。正常流程是：拿到 ENOMEM 后 dyld 去调 `map_and_slide` 建立映射。
本机的情况是：拿到 ENOMEM 后**没有走到那一步**。

### 结论

失败边界从"`check_np` 与 `map_and_slide` 之间"进一步收紧为：

> **dyld 收到 `check_np` 的 ENOMEM 之后、调用 `map_and_slide` 之前。**

下一步该查的是 dyld 这段前置逻辑：从哪个路径读 dyld shared cache
（`/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/` 之类）、
文件是否存在、可否打开、header 校验是否通过。

### 证据文件

- `scratch/pl103_com5_131701.log`（23,393 B，完整启动）
- `scratch/pl103_ps_snapshot.txt`（ps 快照与解读）

## 第 32 轮：实机排查 —— dyld 共享缓存文件本身完全健康

设备停在 launchd（`thr=1 fp=182656 count=1`）期间，通过 CDC shell 实测：

### 缓存目录存在且完整

```
ls /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld  -> entries=86 eof=1
ls /System/Library/dyld                                      -> entries=86 eof=1  (firmlink 正常)
```

含 `.01`..`.81`、`.dylddata`、`.dyldreadonly`、`.dyldlinkedit`、`.map`、`.atlas`。

### 主缓存文件 stat

```
dyld_shared_cache_arm64e      type=reg mode=0755 uid=0 gid=80 size=753,664
dyld_shared_cache_arm64e.03   type=reg mode=0755 uid=0 gid=80 size=131,268,608
```

### 主缓存 header 逐字节核验（CDC cat，转义后还原）

```
0000: 64 79 6c 64 5f 76 31 20 20 61 72 6d 36 34 65 00   "dyld_v1  arm64e"
0010: 38 02 00 00              mappingOffset     = 0x238
0014: 01 00 00 00              mappingCount      = 1
0018: 00 00 00 00              imagesOffsetOld   = 0
0020: 00 00 00 00 00 00 00 00  dyldBaseAddress   = 0
0028: 00 40 0b 00 00 00 00 00  codeSignatureOffset = 0xb4000
0030: 00 40 00 00 00 00 00 00  codeSignatureSize   = 0x4000
0058: 8b 1e 17 c6 72 d6 30 17  uuid
0068: 02 00 00 00              platform = 2

mapping_info[0] @0x238:
  fileOffset = 0x0000000180000000   (新格式的跨子文件全局偏移)
  size       = 0x00000000000b4000   (737,280 ≈ stat 的 753,664 减签名)
  vmOffset   = 0
  maxProt/initProt = 5 / 5
```

**结论：dyld 共享缓存文件、目录、header、mapping 全部正常。**
不是"文件缺失/损坏"这类问题。

### 排查手段的边界

`tb710fu_shell_cat` 只从 offset 0 读 4096 字节
（`vn_rdwr(..., gShellCatBuf, TB710FU_SHELL_CAT_BYTES /*4096*/, 0, ...)`），
**没有偏移读能力**，因此无法直接核验 `text-fixup0` 指向的
`.03 @ 0x5de1134` 处字节（已确认该偏移在文件范围内：
98,439,476 < 131,268,608）。

### 下一步

既然文件层面全健康，而 `map_and_slide` 从未被调用，
值得直接做一次单变量实验：**去掉 `text-fixup0` 重打包**
（它改写的正是 dyld 缓存代码段，若目标字节不匹配可能让 dyld
在构造 mappings 时提前放弃）。

## 第 33 轮：单变量实验确认 text-fixup0 是启动退化的直接原因

### 实验设计

pl103（能过 launchd）与 pl100（停在 swap 后）之间差很多 boot-args。
本轮只加回**一个**参数，其余全部保持 pl103：

| 镜像 | text-fixup0 | 其他差异 |
|---|---|---|
| pl103 | 无 | 基准（首次过 launchd） |
| **pl104** | **有（相同值）** | 与 pl103 完全一致 |

`text-fixup0=dyld_shared_cache_arm64e.03,5de1134,08060037,1f2003d5`

### 结果（决定性）

| 标记 | pl103 | pl104 |
|---|---|---|
| 根卷 + cryptex 挂载 | ✅ | ✅ |
| `VM Swap Subsystem is ON` | ✅ | ✅ |
| **`exec: pid 1 launchd`** | **✅ 有** | **❌ 无** |
| `check_np` | ✅ | ❌ |
| `AMFI pointer initialized` | ✅ | ❌ |
| `TB710FU SRMAP` | ❌ | ❌ |

只多了这一个参数，launchd 就从"能 exec"退化为"根本起不来"。

### 为什么 text-fixup0 会造成这个后果

该参数让内核在 dyld 共享缓存 `.03` 文件的 **偏移 0x5de1134** 处：

- 期望读到 `08 06 00 37`
- 改写为 `1f 20 03 d5`（NOP）

如果设备上 `.03` 在该偏移的实际字节**不是** `08060037`，那么：

1. 改写要么不匹配（fixup 拒绝/无效），
2. 要么改坏了真实指令，使 dyld 在解析共享缓存时遇到无效代码，
3. 结果就是 dyld 在建立 mapping 之前就放弃 —— 与"`map_and_slide` 从未被调用"完全吻合。

### 已知边界

`tb710fu_shell_cat` 只读 offset 0 的 4096 字节，**无法直接核验该偏移的实际字节**。
但结论不依赖这个核验：去掉 fixup 启动推进、加回 fixup 启动退化，
这一组对照本身已足以判定该参数在本机是有害的。

### 结论与下一步

**`text-fixup0` 应当从基线中移除。** 它原本的用途是把
`IOMFBDisplay::framebuffer_pixel_format` 的 `'RGhA'` 分支 NOP 掉以强制 8-bit BGRA，
属于显示优化；它不该以牺牲启动为代价。

下一步：以 **pl103 的参数集为基线**（无 text-fixup0、无 gpu-*、无 tb710fu-gop），
在此基础上逐步加回 GPU/显示相关参数，每次只加一组，用 `exec: pid 1 launchd`
与 `unnest of range` 作为判据。

### 证据文件

- `scratch/pl104_com5_133708.log`（23,386 B，停在 VM Swap Subsystem）
- `scratch/pl103_com5_131701.log`（23,393 B，过了 launchd）

## 第 30 轮：纠正 —— 4K 共享区域补丁从未参与构建

### 被推翻的说法

第 29 轮我写过：「`pl49` 之后引入了 4K 共享区域适配的补丁，
如果它们把 dyld 的映射路径改坏了，就能完美解释现状。」

**这是错的。** 逐个查 `patches/xnu/series`：

| 补丁 | 在 series 中 |
|---|---|
| `native-4k-shared-region-slide.patch` | **否** |
| `native-4k-block-split.patch` | **否** |
| `native-4k-signature-hash-cache.patch` | **否** |
| `native-4k-user-abi.patch` | **否** |
| `native-4k-bootstrap-capacity.patch` | **否** |
| `darwin26-shared-region.patch` | **否** |
| `macos-shared-region-size.patch` | **否** |

这些文件存在于 `patches/xnu/` 目录，但**没有一条被 series 引用**，
因此从未参与任何一次构建。不能把它们当作回归来源。

### series 里与启动/共享区域相关的实际只有四条

```
5:  0005-native-4k-bootstrap-pool-9g.patch
20: 0020-tb710fu-bootstrap-phase-checkpoints.patch
23: 0023-tb710fu-bootstrap-suboperation-checkpoints.patch
52: 0052-tb710fu-shared-region-map-trace.patch
```

- `0005` 是 bootstrap 内存池（9 GiB），与 dyld 映射无关
- `0020` / `0023` 只 instrument `osfmk/kern/startup.c` 的阶段检查点，
  输出 `TB710FU boot: <stage> <event>`，与 dyld 无关
- `0052` 是本会话新增的追踪补丁，只在 `map_and_slide` 入口打印

series 共 **52 个补丁**。

### 因此

「pl49 能跑、现在不能跑」这个差异**不能归因于 4K 共享区域补丁**。
要为这个差异找解释，得去比对 pl49 与当前内核的**实际二进制差异**
或**运行时环境差异**（Data 卷内容、缓存文件本身），而不是补丁集。

### 一条尚未验证但可查的线索

`pl49` 的 boot-args 里有 `no-lc=1`，当前镜像没有。
但已查证它作用在 `mac_proc_check_launch_constraints`（exec 时的启动约束），
而 `launchd` 的 exec 已成功（日志有 `exec: pid 1 launchd`），
所以 `no-lc` **不太可能**是 dyld 阶段卡死的原因。保留为低优先级。

## 第 31 轮：确认 4 KiB 页内核 + dyld 要求 16 KiB 对齐

### 4K 页已在镜像里实测确认

用 `scratch/check-page-size.py` 从 `.local/prelink-kernel.macho` 读符号初值：

```
symbols found:
  arm_hardware_page_size   0xfffffff0071a11b0
  PAGE_SHIFT_CONST         0xfffffff007dda120

arm_hardware_page_size: file 0x19d1b0 in __PRELINK_TEXT  u32=4095 (0xfff)
```

**`arm_hardware_page_size = 0xFFF = FOURK_PAGE_MASK`。** 这台设备确实是
4 KiB 页内核。这一点此前一直被当作既定前提，现在有镜像内实测值了。

（`PAGE_SHIFT_CONST` 读到 0，因为它在 `__DATA_CONST` 里由
`__mod_init_func` 启动时填充，静态文件里尚未写入——与之前读
`gMetaClass` 遇到的情况相同。）

### dyld 自身要求 16 KiB 对齐

从设备上 `cat /usr/lib/dyld` 取 FAT header 解析：

```
fat magic: 0xcafebabe   nfat_arch: 3
  arch0: cputype=0x01000007 (x86_64)  offset=0x4000    size=0x11a610  align=2^14 (16384)
  arch1: cputype=0x0100000c (arm64e)  offset=0x120000  size=0x16c530  align=2^14 (16384)
  arch2: cputype=0x0100000c (arm64e)  offset=0x290000  size=0x1644c0  align=2^14 (16384)
```

三个切片全部要求 **2^14 = 16 KiB** 的文件偏移对齐。

### 已检查且不构成阻断的两处

1. `vm_unix.c:2045-2047` 的 slide 掩码在 arm64 上固定用
   `~SIXTEENK_PAGE_MASK`，`SIXTEENK_PAGE_MASK` 定义正确为 `0x3FFF`
   （`osfmk/vm/vm_protos.h:182`）。slide 随机值仍是 16K 对齐，符合 dyld 期望。

2. `vm_unix.c:2115` 的 offset 校验：
   ```c
   if (__improbable(0 != (offset & vm_map_page_mask(map)))) {
           return KERN_INVALID_ARGUMENT;
   }
   ```
   运行时 `vm_map_page_mask` 是 `0xFFF`（4K），而 dyld 传的 offset 是 16K
   对齐的，16K 对齐天然满足 4K 对齐，**该检查会通过**。

### 仍未解释的核心事实

补丁 0052 在 `shared_region_map_and_slide_2_np` 入口打印，**输出为零**。
所以无论上面这些检查是否通过，**这个函数从未被调用**。

`launchd` 的 syscall 环停在
`openat → fstat64 → pread → fcntl(F_ADDFILESIGS_RETURN) → mmap → munmap`，
六步全部返回 0，然后重来。

即：dyld 在**自己内部**就决定了不调用 `map_and_slide`，而不是内核拒绝了它。
矛盾点在于 `mmap` 成功了它为什么还要重来——这只能靠记录 `mmap`
的实际参数与返回值来解释，也就是补丁 0053 要做的事。

### 工具

- `scratch/check-page-size.py` — 从预链接镜像读页大小常量
- `scratch/decode-dyld-fat.ps1` — 解析 CDC 抓回的 dyld FAT header
