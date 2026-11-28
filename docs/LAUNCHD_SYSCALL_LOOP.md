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
