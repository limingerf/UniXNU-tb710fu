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
