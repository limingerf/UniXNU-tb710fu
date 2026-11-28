# launchd 卡死的系统调用环（决定性证据）

采集时间：2026-11-28 13:48
镜像：pl105（含完整 GPU 组，GPU bring-up stage 9/9 成功）
采集方式：CDC `ps` 动词，`TB710FU PID1 RING` 行

## 原始数据

```
TB710FU PS: begin watchdog_policy=0 aes_kat=1 aes_reg=1 aes_done=3 aes_nonce=0
TB710FU PID1: sc=54 in=1 cur=92 arg0=0x4 arg1=0x61
TB710FU PID1 RING: 153:0 463:0 339:0 153:0 92:0 197:0 73:0 463:0 339:0 153:0 92:0 197:0 73:0 463:0 339:0 153:0
TB710FU PS: pid=1 ppid=0 name=launchd thr=1 fp=182656
TB710FU PS: end count=1
```

## 解读

环里出现的系统调用编号（`bsd/kern/syscalls.master` 核对）：

| 编号 | 名称 | 签名 |
|---|---|---|
| 73 | `munmap` | `munmap(caddr_ut addr, size_ut len)` |
| 92 | `fcntl` | `sys_fcntl(int fd, int cmd, long arg)` |
| 153 | `pread` | `pread(int fd, user_addr_t buf, user_size_t nbyte, off_t offset)` |
| 197 | `mmap` | `mmap(caddr_ut addr, size_ut len, int prot, int flags, int fd, off_t)` |
| 339 | `fstat64` | `sys_fstat64(int fd, user_addr_t ub)` |
| 463 | `openat` | `openat(int fd, user_addr_t path, int flags, int mode)` |

环中呈现的模式（从 463 开始读）：

```
openat → fstat64 → pread → fcntl → mmap → munmap
```

然后**立刻回到 `openat`**，如此往复。16 个环槽里这个模式出现了两轮半，
说明它不是偶发，而是一个高频重试。

## 关键推论

### 1. 不是死锁，是活锁（重试循环）

死锁会停在一个系统调用上不动。这里是持续发起新的系统调用，
只是每次都回到同一序列。所以 `launchd` 一直在"尝试并失败"。

### 2. `shared_region_map_and_slide_2_np`（编号 536）**从未出现**

整个环里没有任何 536。这独立印证了补丁 0052 在
`shared_region_map_and_slide_2_np` 入口的 `TB710FU SRMAP` 打印
一次都没有出现的观察。

**两个互相独立的证据都指向同一结论：dyld 根本没走到那次调用。**

### 3. 失败发生在 mmap / munmap 这一段

序列里 `mmap`(197) 后紧跟 `munmap`(73)，说明映射建立后立即被撤销，
或者映射失败后清理。之后没有继续，而是跳回 `openat` 重来。

这条路径符合 dyld 加载共享缓存时的"读取-映射-校验-失败-重试"结构。

### 4. 文件句柄是 fd=4

`arg0=0x4` 表明当前操作的文件描述符是 4。
`cur=92`（fcntl）时 `arg1=0x61`，具体 cmd 语义待查证。

## 与已有结论的关系

- 与第 31 轮 `ps` 快照（`thr=1 fp=182656 count=1`）一致，是同一现象的另一面
- 与"`map_and_slide` 未被调用"一致，现在有两个独立证据
- 与 GPU 无关：pl103（无 GPU）、pl104、pl105（GPU 全通）三者
  `thr`/`fp`/`procs` 完全相同

## 下一步该查什么

1. `fcntl` 的 `arg1=0x61`（97）到底是哪个命令
2. fd=4 打开的是哪个路径
3. `mmap` 的返回值（环里的 `:0` 字段含义需先确认）

如果 `mmap` 确实失败，需要知道 errno。这决定了是"地址空间不足"
还是"文件内容不被接受"。

## 根因定位：launchd 在 F_ADDFILESIGS_RETURN 之后重试

### 环字段的准确语义

`package/BootWatchdogPolicy.c:252`：

```c
printf(" %u:%u", v >> 16, v & 0xffffu);
```

内核侧 `patches/xnu/0043-tb710fu-pid1-syscall-ring.patch`：

```c
tb710fu_pid1_sc_ring[slot] = ((uint32_t)code << 16) | ((uint32_t)error & 0xffffu);
```

所以 `153:0` 的意思是 **系统调用 153、error = 0**。低 16 位是**错误码**，
不是返回值。

### 环的解读（全部成功）

```
463:0  openat   error 0  ← 成功
339:0  fstat64  error 0  ← 成功
153:0  pread    error 0  ← 成功
 92:0  fcntl    error 0  ← 成功
197:0  mmap     error 0  ← 成功
 73:0  munmap   error 0  ← 成功
→ 回到 openat，同样六步再来一遍
```

**每一个系统调用都成功。** 这不是失败重试。

### 最后停在哪个调用

```
TB710FU PID1: sc=54 in=1 cur=92 arg0=0x4 arg1=0x61
```

- `cur = 92` → `fcntl`
- `arg0 = 0x4` → **fd = 4**
- `arg1 = 0x61` → **97**

`bsd/sys/fcntl.h`：

```c
#define F_ADDFILESIGS_RETURN  97  /* Add signature from same file,
                                     return end offset in structure on success */
```

### 这不是死锁，是"成功了但结果不被接受"

`launchd`（经 dyld）反复执行：

1. `openat` 打开一个文件（fd=4，几乎确定是 dyld shared cache）
2. `fstat64` 取元数据
3. `pread` 读内容（读 header）
4. **`fcntl(F_ADDFILESIGS_RETURN)` 把该文件的代码签名注册进 vnode**
5. `mmap` 映射
6. `munmap` 立刻解除
7. 回第 1 步

`F_ADDFILESIGS_RETURN` 成功说明签名确实被接受了。但随后 dyld 的某个校验
不通过，于是主动 `munmap` 并重新打开——形成活锁。

### 与 `map_and_slide` 未调用的关系

三个独立证据现在完全一致：

| 证据 | 指向 |
|---|---|
| 补丁 0052 的 `TB710FU SRMAP` 零输出 | `shared_region_map_and_slide_2_np` 未被调用 |
| syscall 环中无 536 | 同上 |
| 环停在 `openat→...→mmap→munmap` 循环 | 卡在**调用它之前的准备阶段** |

所以失败点在：**dyld 读完缓存、挂好签名、映射成功之后，
决定"这批数据不合格"并重来。**

### 下一步该查的（按优先级）

1. `mmap` 的 **flags**（`arg1`）。如果 dyld 用 `MAP_FILE` 但文件按 4K
   页对齐而缓存内部按 16K 描述，映射后校验会不通过。
2. `fcntl(F_ADDFILESIGS_RETURN)` 返回的 **end offset**。它给出签名覆盖到
   文件哪个位置；如果这个值与 cache 的 `codeSignatureOffset/Size` 对不上，
   dyld 会判定签名无效。
3. 该文件是否就是 `dyld_shared_cache_arm64e.03`（131 MB 那个），
   还是主文件（736 KB）。这决定 4K/16K 页对齐问题是否相关。

### 会话内已排除的

- 缓存文件缺失/损坏：已用 `ls` / `stat` / header 逐字节核验
- GPU 驱动：pl103（无）、pl104、pl105（GPU stage 9/9 全通）三者
  `thr=1 fp=182656 procs=1` 完全相同
- `text-fixup0`：pl103/pl104 都到 launchd，此前的"退化"结论已撤回

## 第 27 轮：排除 4K 页对齐，并确认进度条来自 UEFI

### 4K 页对齐假设 —— 已排除

怀疑点原本是：`fcntl(F_ADDFILESIGS_RETURN)` 在内核里对签名 blob 做页舍入
（`kern_descrip.c:4184-4187` 注释 "kernel_blob_size rounded to a page size"），
若 4K 内核与 16K 排布的缓存签名不一致，就会让 dyld 判定签名无效。

逐个查证结果：

| 环节 | 实现 | 是否引入 4K/16K 差异 |
|---|---|---|
| `ubc_cs_blob_pagewise_allocate` | `ubc_subr.c:931` | **否** — 未定义 `CODE_SIGNING_MONITOR` 时恒返回 `false` |
| `ubc_cs_blob_allocate` | `ubc_subr.c:3143` | **否** — 走 `kalloc_data_tag`，不调用 `round_page` |
| `resid` 处理 | `kern_descrip.c:4184` | **否** — 仅当 `resid != 0` 才 memset，而无页对齐时 `resid` 为 0 |

所以 `F_ADDFILESIGS_RETURN` 在本机内核里**不做任何页舍入**，
这条路径与 4K 页适配无关。该假设证伪。

### 进度条来自 UEFI，不是内核

`disp` 动词在卡住状态下返回：

```
TB710FU GopDisplay: disp clients=0 frames=0 transfers=0 retire=0 vsync_send=0 ...
TB710FU GopDisplay: swap validated=0 kicks=0 inflight=0 ...
TB710FU GopDisplay: formats bgra=0 abgr=0 half=0 refused=0 lastfmt=00000000 ...
TB710FU GopDisplay: scanout flips=0 mapped=0 mapfail=0 released=0 copies=0 bootflips=0 ...
```

**每一个计数器都是 0。** 内核从未向面板提交过任何一帧。

这有两层含义：

1. 用户看到的"苹果进度条走到 1-2%"是 **UEFI 画的**，不是内核画的。
   所以它不能用来判断内核进度。
2. `GopDisplay` 虽已初始化（能应答 `disp`），但 `clients=0` 说明
   没有任何用户态进程连上来——与 `launchd` 从未 fork 任何子进程一致。

### 当前根因表述（收敛后）

`launchd` 在 dyld 引导阶段陷入活锁：

```
openat → fstat64 → pread → fcntl(F_ADDFILESIGS_RETURN) → mmap → munmap → 重来
```

六步全部返回 0（成功），没有一次失败。`shared_region_map_and_slide_2_np`
（syscall 536）在整个环中从未出现。

已排除的解释：

- 缓存文件缺失/损坏（ls / stat / header 逐字节核验通过）
- 4K 页对齐（本轮证伪，见上）
- GPU 驱动（pl103/104/105 三者 launchd 状态完全一致）
- `text-fixup0`（pl103/104 都到 launchd，此前结论已撤回）

仍在候选中的解释：

- `mmap` 的 flags 与缓存内部段对齐（16K）不匹配，映射后校验失败
- `F_ADDFILESIGS_RETURN` 返回的 end offset 与 cache header 的
  `codeSignatureOffset/Size` 不符
- dyld 自身在 4K 页环境下的其它地址计算（例如 slide 计算依赖 16K 粒度）

## 第 28 轮：子缓存 header 实测（并纠正一个错误联想）

### 实测数据

用 CDC `cat` 读取三个缓存的 header，转义还原后按同一布局解析：

| 文件 | mappingOffset | mappingCount | uuid@0x58 |
|---|---|---|---|
| `dyld_shared_cache_arm64e`（主） | 0x238 | 1 | `8b1e17c672d6301791a4916022e3612e` |
| `dyld_shared_cache_arm64e.03` | 0x238 | 1 | `12c09024bd03321f989a28512b5971dd` |
| `dyld_shared_cache_arm64e.25.dylddata` | 0x238 | **7** | `c191e072a69937968f02d65c5cf6be4e` |

三个文件的 header 结构一致（mappingOffset 都是 0x238），
`.dylddata` 的 mappingCount=7 符合"数据段分片"的新格式。

### 一个必须记录的纠错

我一度想用"主缓存与子缓存 UUID 不同"来解释 dyld 拒绝缓存。
**这是错的，已放弃。** 每个子缓存本来就有自己的 cacheUUID，
这是正常设计，不是缺陷。拿它当根因会把排查引向死路。

同理，`.25.dylddata` 解出的 `platform=538` 是**解码偏移错误**：
不同子类型的 header 在 0x68 之后的字段布局不同，不能直接套用主缓存的偏移。

### 现有观测能力的边界（已穷尽）

CDC shell 动词只有：
`edl` `reboot` `bootloader` `recovery` `ps` `help` `ls` `cat` `stat` `ioreg` `disp`

`cat` 固定从 offset 0 读 4096 字节
（`vn_rdwr(..., TB710FU_SHELL_CAT_BYTES /*4096*/, 0, ...)`），
不支持偏移读。因此**无法**：

- 读取 `.03` 在 `0x5de1134` 处的实际字节
- 读取 `launchd` 的 fd 表或 VM 映射
- 读取任何 `mmap` 参数

### 结论：必须加内核侧观测

要区分下面三种可能，只能在内核里记录完整参数：

| 假设 | 需要的证据 |
|---|---|
| A. `mmap` 成功但 flags/anchor 让 dyld 事后拒绝 | `mmap` 的 prot/flags/fd/offset 与返回值 |
| B. `F_ADDFILESIGS_RETURN` 返回的 end offset 与 cache header 不一致 | fcntl 的输入结构与内核回写字段 |
| C. fd=4 指向的子缓存 header 与主缓存不匹配 | `openat` 的路径 + 该 fd 后续调用序列 |

已写好生成器 `scratch/gen-0053.sh`（补丁 0053），在
`bsd/dev/arm/systemcalls.c` 的 `unix_syscall()` 里对 pid 1 记录
`openat`/`mmap`/`fcntl`/`munmap` 的完整参数与返回值，
输出到 `tb710fu_dyld_probe[64]`，供扩展 `ps` 动词读出。

尚未生成补丁文件、尚未编译、尚未验证 —— 需要一次 CI 构建。
