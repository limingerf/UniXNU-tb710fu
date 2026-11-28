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

## 第 32 轮：静态分析到极限，等待构建决策

### 本轮验证的数据点（意义有限，记录备查）

三个镜像的 launchd footprint：

| 镜像 | boot-args 差异 | fp |
|---|---|---|
| pl103 | 无 GPU、无 text-fixup0 | 182,656 |
| pl104 | 有 text-fixup0 | 186,816（+4,160） |
| pl105 | 完整 GPU 组 | 182,656 |

pl104 比 pl103 多 4,160 字节（= 4096 + 64，即一个 4K 页加 64 字节），
可能来自 text-fixup0 让 dyld 多保留了一个页的映射。
但这不能解释卡死——pl105 的 fp 与 pl103 完全相同，而它 GPU 全通。

### 已彻底穷尽的手段

| 手段 | 结论 |
|---|---|
| CDC `ls` / `stat` / `cat` | 缓存目录、文件大小、header 全部核验通过 |
| CDC `ps` | 得到 launchd `thr=1 fp=182656 procs=1`，flat |
| CDC `disp` | 得到全部帧计数器为 0（屏幕由 UEFI 画） |
| CDC `ioreg` | 得到 109 个 IOService 节点，可用 |
| PID1 syscall ring | 得到 `openat→fstat64→pread→fcntl→mmap→munmap` 活锁 |
| 预链接镜像符号读取 | 确认 `arm_hardware_page_size = 0xFFF`（4K 页） |
| dyld FAT header 解析 | 确认三个切片全部要求 2^14 对齐 |
| 内核源码走查 | slide 掩码、offset 校验、cs blob 分配三处均无 4K 障碍 |
| 补丁集审计 | 4K shared-region 补丁从未进 series；`no-lc` 作用在 exec 阶段 |

### 卡点

所有证据指向同一个未解问题：

> `mmap` 返回 0（成功），dyld 却立刻 `munmap` 并重新 `openat`。

要回答这个问题，必须知道 `mmap` 的 `addr/len/prot/flags/fd/offset` 与返回值。
现有观测层拿不到这些。

### 唯一的下一步

补丁 0053（生成器已写好 `scratch/gen-0053.sh`，未执行）：

- 在 `bsd/dev/arm/systemcalls.c` 的 `unix_syscall()` 里
- 对 pid 1 记录 `openat` / `mmap` / `fcntl` / `munmap` 的完整参数
- 写入 `tb710fu_dyld_probe[64]`
- 再扩一个 CDC 动词读出

需要一次 CI 构建。**用户尚未批准，不要擅自消耗额度。**

## 第 34 轮：写 XNU 补丁的两个必踩坑（0053 实战记录）

补丁 0053 连续两次构建失败，原因都不是探针逻辑，而是**补丁上下文**。

### 坑一：不能对着裸 feed 生成

第一次在 `ac9718f` 裸 feed 上 `git diff`：

```
error: patch failed: bsd/dev/arm/systemcalls.c:179
error: bsd/dev/arm/systemcalls.c: patch does not apply
```

因为 0053 要改的 dispatch 行，**0043 已经在同一处插入了自己的
enter/exit hook**。对着裸 feed 生成的上下文在实际构建时不存在。

反例说明：0052 能成功，是因为它改 `bsd/vm/vm_unix.c`，
那个文件前面的补丁都没动过。

### 坑二：锚点不能用 `unix_syscall(`

第二次改成先应用 0043 再注入，仍然失败，但行号变了：

```
error: patch failed: bsd/dev/arm/systemcalls.c:79
```

因为 `src.index('unix_syscall(')` 匹配到的是**前向声明**：

```c
extern void
unix_syscall(
```

插入点落在 `extern void` 和函数名之间，既破坏了声明，又让 hunk 79 的
上下文对不上。

### 正确做法

0043 的插入锚点是：

```c
extern int mach_trap_count;
#endif
<空行>
```

在自己的声明块**紧跟这个空行**插入。这样两个补丁的 hunk 上下文一致，
hunk 头变成 `@@ -79,6 +79,88 @@`，与 0043 完全对齐。

### 生成 0053 的可靠流程

```python
# 1. 干净 worktree
git worktree add --detach <wt> ac9718f

# 2. 只应用会碰同一个文件的补丁（这里是 0043）
git -C <wt> apply patches/xnu/0043-tb710fu-pid1-syscall-ring.patch

# 3. 注入，锚点与 0043 相同
src.replace('extern int mach_trap_count;\n#endif\n',
            'extern int mach_trap_count;\n#endif\n' + decls, 1)

# 4. git diff 输出，用 pathlib 写字节（PowerShell 重定向会写出 UTF-16 BOM）

# 5. 验证
git -C <wt> checkout -- <file>
git -C <wt> apply --check <patch>   # 必须 PASS
```

### 附带发现

- **PowerShell 的 `>` 重定向写出 UTF-16 BOM**，`git apply` 会报
  "No valid patches in input"。必须用 Python/`io` 写字节。
- `git apply --check` **只能验证上下文匹配**，不能代替真实构建。
  前两次都是 check 通过、构建失败——因为 check 时用的树和构建时的树不同。

### 实现文件

- `scratch/gen-0053-v3.py` — 最终可用的生成器
- `patches/xnu/0053-tb710fu-dyld-cache-probe.patch` — 5013 字节

## 第 39-40 轮：补丁 0053 构建成功，以及八次失败的方法论教训

### 结果

`a7224d4` 的 prelink 构建 **conclusion=success**（run 46118781364）：

```
kernel/kernel.macho  143147008 bytes
sha256 = 5dbd127aeaf40a7f082e05e4c1c29a7040fdbab828d25247a07df0328582ff33
probe symbol present in kernel: YES
```

补丁应用、编译、链接、prelink 全部通过。这是本会话第一次让 0053 走完构建。

### 八次失败的完整谱系（全部是补丁上下文，不是探针代码）

| # | 做法 | 失败点 |
|---|---|---|
| 1 | 对着裸 feed `ac9718f` 生成 | `hunk 179`：0043 已改同一行，上下文不存在 |
| 2 | 改锚点为 `unix_syscall(` | `hunk 79`：匹配到前向声明，插在 `extern void` 和函数名之间 |
| 3 | 换成 0043 的锚点 `extern int mach_trap_count;` | `hunk 79`：该锚点后紧跟的是 0043 的整块，不是函数注释 |
| 4 | 按行分割/重写文件 | 变成整文件重写（713+/584-），会覆盖 0043 |
| 5 | 字节级插入，只增不删 | `hunk 79`：仍是猜的上下文 |
| 6 | 假设文件是 CRLF，做特例 | `hunk 79`：**真实文件是 LF**，特例毫无意义 |
| 7 | 从 artifact 取真实文件作为基线 | **编译错误**：`function definition is not allowed here` —— 声明块插进了 `unix_syscall` 函数体内 |
| 8 | 用花括号深度验证插入点 | **成功** |

### 第 7 次失败的根因（最有价值的一条）

脚本想找"0043 声明块的 `#endif`"，但：

```c
#if CONFIG_DEBUG_SYSCALL_REJECTION
extern int mach_trap_count;
#endif        <- 这个 #endif 关闭的是上面的 guard
```

所以"`extern int mach_trap_count;` 之后的第一个 `#endif`"指向的是**条件编译守卫**，
而不是 0043 声明块的结尾。插入点整体落到了 `unix_syscall` 函数体内，
于是编译器报 `function definition is not allowed here`，后续
`tb710fu_dyld_probe_put` 也就成了未声明函数。

### 有效的做法

1. **从失败构建的 artifact 取真实源码**。`Export the patched sources` 步骤带
   `if: always()`，所以即使构建失败也会产出 `patched-sources/systemcalls.c`。
   这是唯一可靠的基线。

2. **用花括号深度验证插入点**，而不是依赖锚点字符串的直觉位置：

```python
depth = 0
for line in lines[:insert_at]:
    depth += line.count('{') - line.count('}')
assert depth == 0   # 文件作用域
assert depth == 1   # unix_syscall 函数体内
```

3. **拒绝删除任何行的补丁**：`if minus != 0: sys.exit('REFUSING')`。
   整文件重写会静默抹掉前面补丁的改动。

4. **PowerShell 的 `>` 重定向写出 UTF-16 BOM**，`git apply` 会报
   "No valid patches in input"。必须用 Python 写字节。

5. **`git apply --check` 通过不等于构建会通过**。前六次本地 check 全 PASS，
   构建全失败——因为 check 用的树和构建时的树不同。要验证上下文，
   只能用构建机产出的真实文件。

### 仍未验证

探针本身一次都没在实机跑过。`pl107` 镜像已打包：

```
out/tb710fu-pl16-46118781364-pl107-probe-signed.zip
```

boot-args 与 pl105（GPU stage 9/9 全通那版）完全一致，唯一新增是探针。

### 第 40 轮：Windows 串口驱动卡死

刷入前 COM5 打不开：

```
USB 串口设备 (COM5)  Status=OK
Open() -> "A device attached to the system is not functioning"
```

依次尝试全部失败：

- `Disable-PnpDevice` → "常规故障"
- `Enable-PnpDevice` → 成功，设备重新枚举，但串口仍打不开
- `pnputil /remove-device` → "Failed to remove device"
- `pnputil /scan-devices` → "Failed to scan for devices"

设备枚举状态始终 OK，但句柄不可用。**只能物理拔插 USB 或重启系统。**
这不是设备问题，是 Windows 串口驱动层的状态卡死。

## 第 41 轮：pl107 内核离线验证

串口仍未恢复（`Open()` 报 "A device attached to the system is not functioning"），
无法刷机。改用纯离线方式验证 pl107 内核确实带了探针。

### 二进制证据

在 `out/pl107/kernel.macho` 中搜索：

| 目标 | 出现次数 | 说明 |
|---|---|---|
| `tb710fu_dyld_probe` | **2** | 探针环形缓冲的符号名，已进镜像 |
| `tb710fu_dyld_probe_idx` | **1** | 写索引符号名 |
| `MMAP` 立即数 | 11 | recorder 里的 mmap 分支 |
| `MUNM` 立即数 | 3 | recorder 里的 munmap 分支 |
| `OATN` / `FNCT` | 0 | 被编译器优化掉或在别处编码 |
| `TB710FU SRMAP`（对照，0052） | 2 | 控制组，证明搜索方法有效 |

`tb710fu_dyld_probe_put` 本身搜不到是**正常**的：它是 `static inline`，
被内联进调用点后不留符号名。

### 补丁结构复核

```
@@ -92,6 +92,32 @@ int tb710fu_pid1_last_mib[4] = { 0 };
  int tb710fu_pid1_last_sysctl_err = 0;
  char tb710fu_pid1_last_open[96] = { 0 };
  
 +/* ... 探针声明 ... */
```

第一处插入紧跟 0043 的最后一条声明，在**文件作用域**（生成器断言花括号深度为 0）。
第二处在 `unix_syscall` 内 0043 的 sc_exit 之后（深度 1）。

### 结论与边界

- 探针的**数据结构**确实进了内核镜像
- recorder 的**分支**存在（MMAP/MUNM 立即数命中）
- 但 **recorder 是否真的被调用、参数是否正确**，只有实机能证明

`OATN`/`FNCT` 缺失是个待观察点：如果实机跑起来后 `openat`/`fcntl`
的记录缺失，说明这两条分支被优化掉了，需要改用 `volatile` 或
`__attribute__((noinline))` 强制保留。

### 镜像

```
out/tb710fu-pl16-46118781364-pl107-probe-signed.zip
kernel.macho sha256 = 5dbd127aeaf40a7f082e05e4c1c29a7040fdbab828d25247a07df0328582ff33
```

## 第 42 轮：纠正第 41 轮的验证，并发现两个分支被优化掉

### 第 41 轮的结论有一半是错的

第 41 轮我用"MMAP 立即数命中 11 次"当作 recorder 存在的证据。**这是错的。**
查上下文后可见，那些命中全是无关字符串：

```
000d57be  4d 4d 41 50  -> "SKR_MODE_MMAPOK)"       里的 MMAP
0020c649  4d 4d 41 50  -> "eSMART_MMAP_KERNEL_FLAGS_TRIALS"  里的 MMAP
```

它们属于既有代码的常量名/错误信息，和探针无关。

### 真实有效的证据

符号名称表里确实有探针的全局变量：

```
@0x7e8392e  .._tb710fu_dwc3_start._tb710fu_dwc3_stop.
              _tb710fu_dyld_probe._tb710fu_dyld_probe_idx.
              _tb710fu_ethern..
```

对照组 `_tb710fu_pid1_sc_count`（0043 的，已知有效）也在同一张表里，
证明这个搜索方法可靠。

**所以：探针的数据结构确实进了镜像。**

### 但两个分支被优化掉了

四个 tag 立即数在镜像中的实际命中：

| tag | 值 | 命中 |
|---|---|---|
| `OATN`（openat） | 0x4F41544E | **0** |
| `MMAP`（mmap） | 0x4D4D4150 | 16（全是无关字符串） |
| `FNCT`（fcntl） | 0x464E4354 | **0** |
| `MUNM`（munmap） | 0x4D554E4D | 6（同上，无关） |

`OATN` 与 `FNCT` 是**真正的零命中**。也就是说 recorder 里的
`case 463`（openat）和 `case 92`（fcntl）**没有被编译进代码**。

这很致命：`fcntl(0x61 = F_ADDFILESIGS_RETURN)` 正是我们最想看的那个调用，
它决定 dyld 给哪个 fd 挂签名。没有它，探针的价值去了一大半。

### 为什么会被优化掉

`tb710fu_dyld_probe_put` 被声明为 `static inline`，且参数里带了
`(uint64_t)(long)uthread->uu_rval[0]`。当编译器在
`#if DEVELOPMENT || DEBUG` 下判断某些 case 不可达，或者把
`static inline` 函数内联后认为部分写入无效（`tb710fu_dyld_probe` 是普通
volatile 全局，但如果编译器把整个 helper 当成无副作用而省略
——注意 `volatile` 修饰的是指针目标，不是对它的写入算子），
就可能整段删掉。

### 修法

1. 给 recorder 加 `__attribute__((noinline))` 并移到独立的非 inline 函数
2. 每个 case 后用 `__asm__ __volatile__("" ::: "memory")` 建立内存屏障
3. 保证四个 case 的 tag 都以 `volatile` 存储方式写入

这些要等串口恢复后再做一次构建。眼下串口仍然打不开：

```
Open() -> "A device attached to the system is not functioning"
```
