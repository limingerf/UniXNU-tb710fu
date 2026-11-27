# TB710FU macOS 27 移植 · 当前真实状态与交接

**更新**：2026-11-28 04:05
**目的**：记录实际验证到的事实，以及**已被证伪的假设**，避免后来者重走。

---

## 一、当前阻塞（最重要）

平板自 2026-11-28 03:47 刷入 `pl100-srmap` 后，**18 分钟以上完全没有 USB 枚举**：

- 无 `COM5`（CDC）
- 无 `9008`（Qualcomm EDL）
- USB 总线上连"未知设备"都没有

这与用户描述的"卡初始化会自己重启"**不符**——重启循环里至少会短暂出现
`COM5` 或 `9008`。可能原因（无法远程区分）：

1. 停在 UEFI 菜单等待选择（菜单不枚举 USB）
2. 电池耗尽（刷机前电量 88-90%，且长期高负载）
3. `pl100` 内核在 USB 初始化前就挂死

**需要人工**：看一眼屏幕；若黑屏先长按电源确认是否开机。

---

## 二、已确认为事实的结论

### 2.1 pl49 曾经真正跑起过 WindowServer

`scratch/com5_pl49_ps3.log` 有 117 个进程的完整清单：

```
TB710FU PS: pid=1   ppid=0 name=launchd       thr=9  fp=11564736
TB710FU PS: pid=164 ppid=1 name=WindowServer  thr=2  fp=2237632
TB710FU PS: pid=166 ppid=1 name=loginwindow   thr=2  fp=1655808
```

这不是僵尸进程，是完整 macOS 用户空间。**所以问题是 pl49 之后的回归，
不是板子固有缺陷。**

### 2.2 判据：`unnest of range` + `dyld cache mapped`

已验证 9 份完整启动日志，无例外：

| 日志 | unnest 行数 | mapped | 结论 |
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

工具：`scratch/classify-boot.ps1`

### 2.3 失败模式的精确位置

失败启动停在（`scratch/com5_pl97b_20261128-023120.log`，492 行后静默）：

```
420| exec: pid 1 launchd
423| shared_region: [1(launchd)] check_np(...) returned 0x1
424| shared_region: [1(launchd)] check_np(...) returned 0x1
425| dyld[1]: result from check_np(): -1, errno 12
426| ASP: AMFI pointer initialized
     [之后没有 dyld cache mapped，也没有 unnest]
```

---

## 三、已被证伪的假设（不要再试）

| 假设 | 证伪依据 |
|---|---|
| `errno 12` 是错误 | 它是 `shared_region_check_np` 文档化的 "shared region is empty"，**5 个成功案例全都有** |
| `initproc` 退出了 | 补丁 0046 会在 `p==initproc` 时打印 `TB710FU INITEXIT` 并 hold 120s，失败日志里**都没有** |
| `exit: pid 1 init status 0` 是崩溃点 | 成功案例与失败案例**各有 2 次**，位置都在 launchd exec 前后 |
| 内存不足 | 失败与成功的 `pmap_startup() 1240770 page entries` **完全一致** |
| GPU 卡住桌面 | `GopDisplay` 在 launchd 卡住**之后**才由 CDC 8 秒 fallback 触发，是**结果**非原因 |
| `text-fixup0` 有影响 | pl86b/pl89/pl92 **没有**它照样成功；pl93 **有**它也成功 |
| 4K slide 补丁漂移 | pl49(176) 与 pl55(177) 补丁集 `Compare-Object` **只差 0051**（纯诊断） |
| `ml-timeout-sched-preemption=0` 等调度参数 | pl93 **带着**它也成功 |
| 数据卷参数 | 所有镜像都是 `no-reset`/`sheng-persist=2`/`data-persist=2` |
| `GopDisplay` kext 加载铺路 | 失败案例里 GopDisplay **同样完整跑完** `kext load=0` → `registered` |
| 固定延时 settle 窗口 | 我写的 `d90e9a4` 直接卡死内核丢 COM5，已 revert（`4c28463`） |

---

## 四、已就位的诊断（设备一通就能用）

**补丁 `0052-tb710fu-shared-region-map-trace.patch`**
在 `shared_region_map_and_slide_2_np()`（syscall 536）入口/出口无条件打印：

```
TB710FU SRMAP[<pid>]: enter files=<n> mappings=<n>
TB710FU SRMAP[<pid>]: exit kr=0x<x> files=<n> mappings=<n>
```

已独立复核 `out/pl100/kernel.macho` 含这两条格式串。`printf` only，无副作用。

**它能一劳永逸回答**：dyld 到底有没有调用 `map_and_slide`；若调用了，`kr` 是多少。

---

## 五、可用镜像

| 镜像 | 用途 |
|---|---|
| `out/tb710fu-pl16-46058104010-pl100-srmap-signed.zip` | 含 0052 诊断（**已刷入，当前卡住的那个**） |
| `out/tb710fu-pl16-46058104010-pl102-minimal-signed.zip` | 内核同上，但 boot-args 无任何可选子系统，用于区分"菜单等待"vs"内核早死" |
| `out/pl16-46021085431-pl93-5g-srgb-signed/` | 最后一次确认进桌面的版本（583 帧 0 拒帧） |
| `out/pl16-45623265343-pl49-cdc-signed/` | WindowsServer 确认跑起来的版本 |

---

## 六、无人值守铁律

1. **不抓日志时平板必须在 EDL**（`9008`）。卡启动会持续发热伤电池。
2. **绝不让桌面久留**：该板在渲染异常下久留会背光黑屏，COM5 仍枚举但不再响应，
   串口永久丢失。
3. 监视器 `scratch/boot-monitor.ps1` 每代启动结束**强制发 `edl`**。

---

## 七、下一步（设备恢复后，按序）

1. 刷 `pl102-minimal` 或 `pl100-srmap`，观察是否出现 COM5
2. 一旦有 COM5，**立即**抓日志并搜 `TB710FU SRMAP`：
   - 有 `enter` + `exit kr=0x...` → 直接定位失败码
   - 有 `enter` 无 `exit` → 卡在 `map_and_slide` 内部
   - **无任何 SRMAP** → dyld 根本没走到 syscall 536，问题在更早的缓存解析/读文件
3. 用 `scratch/classify-boot.ps1` 判定是否进用户空间
4. 拿到数据前**不要再提新假设**——本轮已连错三次（调度参数、数据卷、GopDisplay）
