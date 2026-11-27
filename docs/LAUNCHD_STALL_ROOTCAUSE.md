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
