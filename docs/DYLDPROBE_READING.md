# DYLDPROBE 结果判读表

刷入 pl109 后执行 `ps`，会多出 `TB710FU DYLDPROBE:` 行。
本文件预先列出各种可能结果及其含义，避免拿到数据后再临时推理。

---

## 一、先看有没有数据

### 情况 0：只有 `idx=0`，没有任何记录

```
TB710FU DYLDPROBE: idx=0 groups=0
TB710FU DYLDPROBE: end
```

`launchd` 没走到这四个系统调用，或者 recorder 未被调用。

可能原因：
- `launchd` 卡在更早的位置（比如 `bsd_init` 之前）
- `#if DEVELOPMENT || DEBUG` 没生效（内核配置变了）
- 探针代码根本没被执行到

**排查**：先看 `ps` 里 `launchd` 的 `thr`/`fp`。若 `thr=1 fp≈182656`，
说明还是老状态，但记录没进环形缓冲 → 探针链接/调用有问题。

### 情况 1：有数据但只有 MMAP/MUNM

```
TB710FU DYLDPROBE: MMAP ...
TB710FU DYLDPROBE: MUNM ...
```

这正是 pl107 的问题（openat/fcntl 分支被优化）。pl109 已用 noinline 修复，
**若仍出现，说明 noinline 在构建时未生效**，需要检查补丁是否真的应用了。

---

## 二、有完整数据时怎么读

一条典型的 dyld 加载序列应该长这样（按时间顺序，最旧在前）：

```
OATN a=<path> b=<flags> c=<fd>     打开缓存文件
MMAP a=<prot<<32|flags> b=<fd> c=<addr>   映射
MUNM a=<addr> b=<len>              解除映射
FNCT a=<fd> b=<cmd> c=<ret>        挂签名
```

### 关键判据 A：`MMAP` 返回地址的对齐

`dyld_shared_cache_arm64e.map` 里所有 mapping 的地址都落在 0x4000（16 KiB）边界：

```
mapping  EX  124MB 0x180400000 -> 0x188068000
mapping  EX  208KB 0x188068000 -> 0x18809C000
```

若 `MMAP` 的 `c`（返回地址）**低 14 位不是 0**（即不是 16K 对齐）：
→ dyld 要求 16K 对齐而 4K 页内核只给 4K 对齐，dyld 拒绝并 `munmap` 重来。
**这是最可能的根因。**

若 `c` 是 16K 对齐的：→ 对齐不是问题，看判据 B。

### 关键判据 B：`FNCT` 的 cmd 与返回值

`FNCT a=<fd> b=<cmd> c=<ret>`：

- `b == 0x61`（97）→ `F_ADDFILESIGS_RETURN`，符合预期
- `c == 0` → 签名挂载成功
- `c == 非0` → 签名失败，`c` 是 errno

若 `b` 不是 0x61，说明 dyld 用的是别的方式验证缓存，需要重新理解流程。

### 关键判据 C：`OATN` 打开的是哪个文件

`OATN` 的 `a` 是 path 的**用户态指针**，不是字符串内容——探针只存了指针。
要确定是哪个文件，需要配合 `fd`：

- 若 `c`（返回的 fd）与后续 `FNCT`/`MMAP` 的 `fd` 一致 → 是同一个文件
- 若要确定具体路径，需要在探针里额外 copyin 路径字符串（当前没做）

**这是探针的一个已知局限**：能确认"同一个 fd 被反复打开/签名/映射"，
但不能直接说出文件名。若要文件名，需再加一个 `copyinstr` 到探针里。

### 关键判据 D：`MMAP` 的 prot/flags

`MMAP` 的 `a` 是 `(prot << 32) | flags`：

- 解出 `prot = a >> 32`，`flags = a & 0xffffffff`
- 常见值：`prot=0x5`（READ|EXEC），`flags=0x4002`（MAP_PRIVATE，含 0x4000 = MAP_JIT 或类似）

若 `flags` 含 `MAP_FIXED`（0x10）而地址未对齐 → 会与判据 A 相互印证。

---

## 三、交叉验证

探针数据应与既有观测一致：

| 既有观测 | 应保持一致 |
|---|---|
| 补丁 0052 的 `TB710FU SRMAP` 零输出 | 数据里**不应有** 536 号系统调用（`map_and_slide`） |
| syscall ring 的六步循环 | `OATN/STAT/PREAD/FNCT/MMAP/MUNM` 应反复出现 |
| `launchd thr=1 fp` 不再增长 | 记录会持续增加（`idx` 递增），但 `fp` 不变 |

若 `idx` 在两次 `ps` 之间**不增长**，说明循环停了——那是新信息，需要重新分析。

---

## 四、拿到数据后的三条分叉

1. **地址不对齐** → 改内核的 `mmap` 路径，或让 dyld 接受 4K 对齐
2. **地址对齐但 dyld 仍重来** → 问题在 `FNCT` 返回值或 flag 组合
3. **没有 `FNCT` 记录** → dyld 根本不做签名验证，流程理解有误

**不要在没有数据前预设哪一条是对的。** 前 27 轮里我预设过三次，全错。

## 第 49 轮：pl109 首次实机运行 —— 探针工作正常，但停在更早的位置

### 实测数据（15:40:07）

```
TB710FU PS: begin watchdog_policy=-1 aes_kat=1 aes_reg=1 aes_done=3 aes_nonce=0
TB710FU PID1: sc=0 in=0 cur=0 arg0=0x0 arg1=0x0
TB710FU PID1 RING:
TB710FU DYLDPROBE: idx=0 groups=0
TB710FU DYLDPROBE: end
TB710FU PS: pid=1 ppid=0 name=init thr=1 fp=18624
TB710FU PS: end count=1
TB710FU battery: published 35% 3774 mV -1983 mA charging=0
```

### 探针本身工作正常

`DYLDPROBE: idx=0 groups=0` + `end` **是正确输出**：探针链接成功、
dump 路径执行了、只是环形缓冲还没有数据（因为 launchd 还没跑起来）。

这证明第 45-46 轮做的两件事都成功了：
- recorder 编译进内核（pl108 验证）
- dump 代码编译进内核并被执行（本轮实机验证）

### 但这次启动停在比 pl105 更早的位置

| 标记 | pl105 | pl109（本次） |
|---|---|---|
| `VM Swap Subsystem is ON` | ✅ | ✅ |
| `exec: pid 1 launchd` | ✅ | **❌** |
| `load_init_program` | ✅ | **❌** |
| `TB710FU GPU: bringup` | ✅ stage9/9 | ✅ |
| 进程名 | `launchd` thr=1 fp=182656 | **`init` thr=1 fp=18624** |

`name=init` 而不是 `launchd`，`fp` 只有 18 KB（对比 182 KB），
说明 `bsd_init` 走完了卷挂载和 swap，但**没走到 `load_init_program`**。

### 两个可疑点

1. **`watchdog_policy=-1`**
   前几次 successful 启动这里是 `watchdog_policy=0`。
   `-1` 表示 `tb710fu_watchdog_policy` 写入失败。而 boot-args 里有
   `disable-watchdogd=1`，一旦策略没写成功，watchdogd 仍会启动，
   而它的 brk #1 退出会 panic 内核（0050 补丁就是为这个加的）。

2. **电量 35%，-1983 mA 放电中**
   比上次记录的 26% 回升了，但仍在大电流放电。不排除供电不稳。

### 与 pl105 的关键差异

pl109 与 pl105 的 boot-args 完全相同（`--extra-args` 逐字一致），
内核只差两处补丁内容：
- 0053 的 recorder + dump（本次新增）
- `BootWatchdogPolicy.c` 的 dump 代码（本次新增）

**需要排除是这两处新增代码导致启动退化。** 但 note: 0053 只在
syscall 返回路径上对 pid 1 记录，dump 只在 `ps` 时执行，
两者都不该影响 `load_init_program` 之前的行为。

更可能的解释是这次启动运气不好（偶发性），或者电量/供电问题。
需要再启动一次看是否复现。

## 第 50 轮：探针首次拿到真实数据 —— dyld 的完整调用序列

### 原始数据（15:44:49，pl109 实机）

```
TB710FU DYLDPROBE: idx=24 groups=6
TB710FU DYLDPROBE: OATN a=0x16f4f24a0 b=0x0 c=0x0
TB710FU DYLDPROBE: OATN a=0x16f4f24a0 b=0x0 c=0x1
TB710FU DYLDPROBE: FNCT a=0x1 b=0x61 c=0x0
TB710FU DYLDPROBE: MMAP a=0x500040002 b=0x1 c=0x9a0000
TB710FU DYLDPROBE: MUNM a=0x1009a0000 b=0x4000 c=0x0
TB710FU DYLDPROBE: OATN a=0x16f4f24b8 b=0x0 c=0x2
TB710FU DYLDPROBE: end
```

同时 `ps`：

```
TB710FU PS: pid=1 ppid=0 name=launchd thr=1 fp=182656
TB710FU PS: end count=1
TB710FU PID1: sc=42 in=1 cur=92 arg0=0x2 arg1=0x61
TB710FU PID1 RING: 4:0 4:0 4:0 6:0 463:2 470:0 463:0 339:0 153:0 92:0 197:0 73:0 ...
```

### 字段解读

| 记录 | 字段 | 值 | 含义 |
|---|---|---|---|
| `OATN` #1 | a | 0x16f4f24a0 | path 用户态指针 |
| | b | 0x0 | flags = O_RDONLY |
| | c | 0x0 | 返回 fd = 0 |
| `OATN` #2 | a | 0x16f4f24a0 | **同一路径**（指针相同） |
| | c | 0x1 | 返回 fd = 1 |
| `FNCT` | a | 0x1 | fd = 1（与上面第二次 open 一致） |
| | b | **0x61** | **`F_ADDFILESIGS_RETURN`** ← 确认 |
| | c | **0x0** | **签名成功** |
| `MMAP` | a | 0x500040002 | prot=0x5 (READ\|EXEC)，flags=0x40002 |
| | b | 0x1 | fd = 1 |
| | c | **0x9a0000** | 返回地址 |
| `MUNM` | a | **0x1009a0000** | 被解除的地址 |
| | b | 0x4000 | 长度 = 16384 = **16 KiB** |
| `OATN` #3 | a | 0x16f4f24b8 | 另一个路径（指针差 0x18） |
| | c | 0x2 | fd = 2 |

### 已确认的事实

1. **`F_ADDFILESIGS_RETURN` 确实被调用**（`b=0x61`），且**成功返回 0**
2. dyld 用 **同一个路径** 打开了**两次**（获 fd 0 和 fd 1）
3. 对 fd 1 做了签名挂载，成功了
4. 用 fd 1 做了 `mmap`，prot=READ|EXEC，flags=0x40002
5. `munmap` 的长度是 **0x4000 = 16 KiB**

### 两个待解的矛盾

**矛盾一：MMAP 返回地址 0x9a0000，MUNM 解除的却是 0x1009a0000**

差值恰好是 `0x100000000`（4 GiB）。可能性：

- (a) 探针记录错位：`MMAP` 的 `c` 是返回值，`MUNM` 的 `a` 是用户传的地址 —— 若 dyld
  传的地址带了某个 slide 基址，两者本就不同
- (b) dyld 映射到低地址，然后 unmap 一个高地址的旧映射
- (c) 我对 `uu_rval[0]` 的解释有误

**不能凭这一个样本下结论。** 需要更多记录确认这个偏移是否恒定。

**矛盾二：`OATN` 的返回 fd 是 0 和 1**

fd 0/1/2 通常是 stdin/stdout/stderr。dyld 在 `launchd` 里拿到 fd 0 和 1 说明：
它可能关闭了原有 fd，或者这个进程的 fd 表本来就是空的。

### 下一步

1. 再查几次 `ps`，看 `idx` 是否增长、记录模式是否重复
2. 若 `MMAP c` 与 `MUNM a` 的差值恒定 → 说明是固定的编码差异，不是问题
3. 若不恒定 → 探针的字段解释需要修正

**这次的判据在第 49 轮的判读表里已经预设过，现在按表执行，不临时改解释。**

## 第 51 轮：纠正"循环已停止"的误判，并发现 fd 递增模式

### 我的判断错了

第 50 轮我写"`idx` 从 24 涨到 56 后完全停止，32 秒内三次采样都是 56，循环停了"。

**这是错的。** 15:56 再采样时 `idx=312 groups=78`，比之前多了 64 组。

当时三次采样（15:45:15 / 15:45:31 / 15:45:47）都得到 56，是因为
**那 32 秒里它确实没动**，但之后又继续了。我把一个暂时的停顿
当成了永久停止。

教训：判断"停止"需要更长的观察窗口，或者配合其他指标
（如 `fp` 是否变化）。32 秒不够。

### 真正重要的发现：fd 单调递增

78 组记录的尾部（ring 保留最新 16 组）：

```
FNCT a=0x10 b=0x61 c=0x0
MMAP a=0x500040002 b=0x10 c=0x9a0000
MUNM a=0x1009a0000 b=0x4000 c=0x0
OATN a=0x16f4f24b8 b=0x0 c=0x11
FNCT a=0x11 b=0x61 c=0x0
MMAP a=0x500040002 b=0x11 c=0x9a0000
MUNM a=0x1009a0000 b=0x4000 c=0x0
OATN a=0x16f4f24b8 b=0x0 c=0x12
...
OATN a=0x16f4f24b8 b=0x0 c=0x14
```

**`fd` 从 0x10 一路涨到 0x14，每次 +1。**

### 这解释了整件事

dyld 在循环里做的是：

1. `openat(path_B, O_RDONLY)` → 拿到**新 fd**（每次都新）
2. `fcntl(fd, F_ADDFILESIGS_RETURN)` → 给新 fd 挂签名，**成功**
3. `mmap(..., fd, ...)` → 映射，返回**同一个地址** 0x1009a0000
4. `munmap(0x1009a0000, 0x4000)` → 解除
5. **不 close(fd)**，回到第 1 步

**fd 泄漏**。`launchd` 的 fd 表在持续增长。

### 为什么这会导致永久卡住

`launchd` 的 fd 数量有限。循环每轮消耗一个 fd，最终会耗尽。
一旦 `openat` 失败，dyld 就走不下去，而 `launchd` 停在原地。

`idx=312` 时 fd 已到 20（0x14）。若上限是几百，它会继续跑一阵再死。

### 这与之前的观测一致

- `thr=1` 从未增加 → launchd 只有一个线程在跑这个循环
- `fp=182656` 恒定 → 循环不分配用户态内存（mmap/munmap 成对，净零）
- `exec: pid 1 launchd` 后有 `check_np` 但无 `dyld cache mapped` → dyld 卡在这个循环里

### 下一步

1. 等 `idx` 涨到 fd 耗尽，看那时打印什么错误
2. 现在应该能看到 `OATN` 返回失败（c 为负或 0xffffffff）
3. 那个失败点就是真正的障碍

**这次不再预设原因，等数据。**

## 第 52 轮：结构化分析确认 —— dyld 在完美重复同一个失败

### 分析结果（scratch/analyse-probe-loop.py）

对 ring 里保留的 16 条记录做分类统计：

```
OATN records: 4   path 0x16f4f24b8: opened 4x, fds 0x11..0x14
MMAP records: 4   distinct return addresses: ['0x9a0000']
                  distinct (prot<<32|flags): ['0x500040002']
                  distinct fds mapped: ['0x10','0x11','0x12','0x13']
MUNM records: 4   distinct unmap addresses: ['0x1009a0000']
                  distinct unmap lengths:   ['0x4000']
```

**每个字段在所有记录里完全相同**（fd 除外，它单调递增）。

### 这排除了"逐步推进"的可能

如果 dyld 在逐步加载（比如映射完 A 段再映射 B 段），参数会变化。
**参数恒定意味着：dyld 请求同一件事，得到同样的结果，判断不满足，重来。**

这是一个纯粹的重复失败循环，不是加载进度。

### 同时纠正第 51 轮的"fd 泄漏"表述

第 51 轮我写"fd 泄漏"，暗示这是**原因**。更准确的表述是：

- fd 确实单调递增（0 → 0x1b），且从不 close
- 但它是**结果**而非原因：dyld 每次重试都 open 一次，失败后不 close 就重来
- fd 迟早会耗尽，那时 `openat` 失败、循环终止。但**耗尽只是终点，不是病根**

病根在"为什么同一个请求总是被拒"。

### 已被数据排除的解释

| 假设 | 证据 | 结论 |
|---|---|---|
| mmap 返回地址不对齐 | munmap 地址 0x1009a0000 低 14 位为 0 | **已排除**（16K 对齐） |
| munmap 长度不是 16K | 每次都是 0x4000 | 已排除（正是 16 KiB） |
| dyld 在扫描多个文件 | 只出现过 2 个 path，之后固定 1 个 | 已排除 |
| prot/flags 每次不同 | 每次都是 0x500040002 | 已排除 |
| F_ADDFILESIGS_RETURN 失败 | 每次 c=0x0 | 已排除（成功） |

### 剩下的唯一方向

所有内核侧调用**都成功**，参数**完全相同**，dyld 仍拒绝。

这意味着判断发生在**用户态**：dyld 拿到成功的返回值后，检查某个它期待的东西，
发现不符，于是 munmap 重来。候选：

1. **缓存 header 的某个字段与 dyld 的预期不符**
   （例如 `dyldCacheHeaderSize`、`platform`、`cacheSubType`）
2. **mmap 出来的内容 dyld 读不懂**（比如 offset 语义在 4K 内核下错位）
3. **`F_ADDFILESIGS_RETURN` 返回的 end offset 与 header 的
   `codeSignatureOffset/Size` 对不上**

第 3 条现在最有可能是可验证的：探针记录了 fcntl 的 `c`（返回值），
但只存了 64 位合并后的值，没存 `fs_blob_size` 之类的输出结构。
若要做，需要扩展探针去 copyout 那个结构。

### 一个更直接的验证思路

`prot=0x5` = READ|EXEC，`flags=0x40002`。解出来：
- prot = 5 = PROT_READ(1) | PROT_EXEC(4)
- flags = 0x40002 = MAP_PRIVATE(0x02) | 0x40000

**0x40000 是 MAP_NOCACHE 还是别的？** 需要查 sys/mman.h 确认这个 flag。
如果是非常规 flag，可能正是 dyld 后续读取失败的原因。

## 第 53 轮：发现一个抓取顺序上的失误

### 问题

所有 pl109 日志都**没有启动序列**：

| 日志 | exec | check_np | dyld cache mapped |
|---|---|---|---|
| pl109_probe_154027.log | 无 | 无 | 无 |
| pl109_reboot_154257.log | 无 | 无 | 无 |
| pl109_live_154449.log | 无 | 无 | 无 |
| pl109_full_154615.log | 无 | 无 | 无 |
| pl109_final_155609.log | 无 | 无 | 无 |

原因：内核 console 环形缓冲容量有限。`ps` 的输出（探针 16 组 + 进程表）
本身就有一百多行，**把之前的启动序列挤掉了**。

### 正确的抓取顺序

刷机后应该：

1. **先抓启动序列**（不发任何命令，纯读 30-60 秒）
2. **再发 `ps`** 取探针数据
3. 两次分别存不同文件

我这次反了：先 `ps`，于是启动序列丢失。

### 这意味着什么

我手上**只有探针数据，没有这次启动的 dyld 阶段信息**。
所以无法确认：

- `check_np` 是否返回了新值
- `dyld cache mapped` 是否出现过
- `unnest` 是否出现
- `AMFI pointer initialized` 是否到达

**这些都是判定"启动走到哪一步"的关键标记，现在全丢了。**

### 已有数据仍然有效

探针记录的 dyld 调用序列是真实的（它不依赖启动日志）：
- `openat` → `fcntl(0x61)` → `mmap` → `munmap`，参数恒定，fd 递增
- 说明 dyld 确实在跑，确实在循环

但**不知道它在循环前后还做了什么**。

### 下一步

重新刷 pl109（或 pl110），刷完后**先纯读 60 秒**再发 `ps`。
这样能同时拿到：
- 启动序列（判断 dyld 阶段）
- 探针数据（判断循环细节）

两次都要存盘，不能只存一次。

### 附带发现：cat 无法读偏移

`tb710fu_shell_cat` 固定从 offset 0 读 4096 字节
（`TB710FU_SHELL_CAT_BYTES`，`vn_rdwr(..., 0, ...)`），
所以无法从设备上提取 `/usr/lib/dyld` 的完整 4 MB 二进制做离线反汇编。

要分析 dyld 需要另一条通路（比如让内核把文件分块 dump 到 console，
或者改 cat 支持 offset 参数）。

## 第 54 轮：pl110 完整启动序列抓到了，与探针数据对上

### 正确的抓取顺序奏效了

刷 pl110 后**先纯读 30 秒、不发任何命令**，拿到：

第一次（16:03:52，13020 字符）—— 从内核入口开始：

```
Darwin Kernel Version 25.5.0: Sat Nov 28 07:54:14 UTC 2026
pmap: excluded memory 0xa00000000..0xa00004000 (16 KB)
pmap_startup() 1240769 page entries
vm_page_bootstrap: 1240765 free pages, 69951 wired pages
...
TB710FU CDC: COM5 drained 12629 live bytes, queueing GopDisplay
TB710FU CDC: GopDisplay holding 5533 ms for the post-root settle window
TB710FU GopDisplay: disabled (boot with tb710fu-gop=1 to register the display)
```

第二次（16:05:21，17843 字符）—— 关键序列：

```
load_init_program: attempting to load /usr/appleinternal/sbin/launchd.development
load_init_program: attempting to load /sbin/launchd
exit: pid 1 init status 0
exec: pid 1 launchd
exit: pid 1 init status 0
shared_region: 0xab7802bdc49d1e23 [1(launchd)] check_np(0x16ec608e8) vm_shared_region_st...
shared_region: 0xab7802bdc49d1e23 [1(launchd)] check_np(0x16ec5ff48) vm_shared_region_st...
dyld[1]: result from check_np(): -1, errno 12
ASP: AMFI pointer initialized
```

### 标记对照

| 标记 | pl110 |
|---|---|
| `VM Swap Subsystem is ON` | ✅ |
| `TB710FU GPU: bringup` | ✅ |
| `load_init_program` | ✅ |
| `exec: pid 1 launchd` | ✅ |
| `check_np` x2 | ✅ |
| `dyld[1]: check_np(): -1, errno 12` | ✅ |
| `ASP: AMFI pointer initialized` | ✅ |
| **`dyld cache mapped`** | **❌** |
| **`unnest of range`** | **❌** |

### 与探针数据的对应关系

现在两批数据能拼起来了：

**启动序列显示**：launchd 起来 → check_np 返回 errno 12（正常，"shared region empty"）
→ AMFI 初始化 → **然后没有 `dyld cache mapped`**

**探针数据显示**：同一时期 dyld 在
`openat → fcntl(0x61) → mmap → munmap` 循环，参数恒定，fd 递增

**合起来**：dyld 在 AMFI 之后开始尝试映射共享缓存，进入这个循环，
但始终没有走到 `shared_region_map_and_slide_2_np`（补丁 0052 的
`TB710FU SRMAP` 从未打印），所以 `dyld cache mapped` 永远不出现。

### 仍然未知

`check_np` 返回 `errno 12` 是**正常**的（成功启动的案例也有）。
问题是之后 dyld 为什么要循环。

探针证明了内核侧全部成功、参数恒定。**dyld 在用户态拒绝了什么。**

### 下一步可做的事

1. 等 pl110 的探针数据（64 位修正后），确认 mmap 返回地址的完整值
2. 检查是否有 `TB710FU SRMAP` 打印（应该没有，验证 0052 与 0053 一致）
3. 若确认 mmap 地址完全正确而 dyld 仍拒绝，则问题在 dyld 对
   cache header 的校验，需要另找观测点

## 第 54 轮（续）：pl110 数据完整，内核侧完全正确

### 64 位修正确认生效

```
DYLDPROBE: MMAP a=0x500040002 b=0x2 c=0x101240000
DYLDPROBE: MUNM a=0x101240000 b=0x4000 c=0x0
```

`mmap` 返回 `0x101240000`，`munmap` 解除 `0x101240000` —— **完全一致**。

第 50 轮那个"差 4 GiB"的假象消失了，证实那纯粹是读低 32 位造成的
（`0x9a0000` 之于 `0x1009a0000`）。这次是 `0x101240000`，低 32 位是
`0x01240000`，与 pl109 的 `0x9a0000` 不同，但同样是高位丢失造成的错觉。

### 逐项核验（全部正确）

| 检查项 | 实测值 | 判定 |
|---|---|---|
| `mmap` 返回地址 | `0x101240000` | 16 KiB 对齐（`& 0x3fff == 0`）✓ |
| `munmap` 地址 | `0x101240000` | 与 mmap 返回值一致 ✓ |
| `munmap` 长度 | `0x4000` | 正是 16 KiB ✓ |
| `prot` | `0x5` | `PROT_READ \| PROT_EXEC` ✓ |
| `flags` | `0x40002` | `MAP_PRIVATE \| MAP_UNIX03` ✓ |
| `fcntl cmd` | `0x61` | `F_ADDFILESIGS_RETURN` ✓ |
| `fcntl` 返回 | `0x0` | 成功 ✓ |
| `openat` 返回 | `0x2, 0x3, 0x4, 0x5, 0x6` | 全部成功，fd 递增 |
| `path` | `0x16ec504b8` | 恒定，同一个文件 |
| `TB710FU SRMAP` | 不存在 | `map_and_slide` 从未被调用 |

### 这次数据的决定性意义

从第 27 轮开始我怀疑过的所有内核侧原因，现在**全部有实测数据排除**：

- ~~mmap 返回地址不对齐~~ → 实测 `0x101240000`，16K 对齐
- ~~munmap 地址与 mmap 不符~~ → 实测完全一致
- ~~munmap 长度不对~~ → 实测正是 16 KiB
- ~~prot/flags 异常~~ → 实测都是标准值
- ~~签名挂载失败~~ → 实测返回 0
- ~~文件打开失败~~ → 实测 fd 正常递增

**内核在这条路径上做得完全正确。dyld 在用户态拒绝了它。**

### 问题的位置现在很明确

dyld 的流程应该是：

1. `openat` 缓存文件 → 成功
2. `fcntl(F_ADDFILESIGS_RETURN)` 挂签名 → 成功
3. `mmap` 映射 header → 成功，得到 16K 对齐的地址
4. **读 header，做校验** ← 这里失败
5. 校验通过 → 解析 mappings → 调 `shared_region_map_and_slide_2_np`
6. 校验失败 → `munmap` → 回到 1

**卡在第 4 步。** 第 5 步从未发生（`SRMAP` 零输出证实）。

### 第 4 步可能校验什么

dyld 读进 header 后会检查一批字段。若其中某项与它的预期不符，就会放弃：

- `magic`（`dyld_v1  arm64e`）— 已用 cat 验证过，正确
- `mappingOffset` / `mappingCount` — 已验证 0x238 / 1，看起来正常
- `platform` — 已验证是 2
- `codeSignatureOffset` / `codeSignatureSize` — 已验证 0xb4000 / 0x4000
- **`cacheSubType`** — 未验证
- **`dyldCacheHeaderSize`** — 未验证
- **mmap 出来的内容是否与 header 声称的一致** — 未验证

### 一个此前没考虑过的可能

dyld 用 4 KiB 页内核映射了文件，但 header 里描述的段布局是按 **16 KiB** 页
组织的。如果 dyld 在用户态用 `mappingOffset` 算出的地址去读段内容，
而实际映射的偏移语义不同，**读到的内容就是错的**，校验自然失败。

**这是 4K 页移植的核心难点**，而不是某个参数的调整。

### 下一步需要的信息

要验证这个猜测，需要知道 dyld 在 mmap 之后**读到了什么**。
现有探针只记录系统调用参数，不记录映射内容。

可以考虑的观测手段：
1. 让探针在 mmap 成功后，把映射到的前 64 字节 dump 到环形缓冲
2. 与 cat 读到的文件 header 对比——若不一致，就证明映射内容错位

这是可行的（探针已经能拿到返回地址，只需再 copyin 一次）。
