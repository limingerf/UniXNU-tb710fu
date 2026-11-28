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
