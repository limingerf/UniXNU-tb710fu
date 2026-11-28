# TB710FU macOS 27 移植 · 当前真实状态与交接

**更新**：2026-11-28 16:40
**目的**：记录实际验证到的事实与已被证伪的假设，避免后来者重走。
**详尽的原始记录**：`docs/LAUNCHD_STALL_ROOTCAUSE.md`、
`docs/LAUNCHD_SYSCALL_LOOP.md`、`docs/DYLDPROBE_READING.md`、
`docs/HID_CHANGE_REVIEW.md`、`docs/GPU_RENDER_DIAGNOSTICS.md`。

---

## 一、当前阻塞（最重要）

启动停在 `launchd` 的 dyld 阶段：**dyld 反复重试同一个共享缓存请求，
内核侧全部成功，但用户态拒绝，永不调用 `shared_region_map_and_slide_2_np`。**

判定判据（实机反复验证）：

```
exec: pid 1 launchd
shared_region: [1(launchd)] check_np(...) x2
dyld[1]: result from check_np(): -1, errno 12     ← 正常，成功案例也有
ASP: AMFI pointer initialized
[之后没有 dyld cache mapped，没有 unnest of range]
```

`TB710FU SRMAP`（补丁 0052 在 syscall 536 入口的打印）**始终零输出**，
这是"map_and_slide 从未被调用"的独立证据。

**当前设备**：`pl112` 已刷入。16:31 与 16:33 两次抓取都停在
`SystemCryptex` 挂载之后、**没有** `boot-policy` / `VM Swap` / `launchd`，
比 pl110 更早。尚不能区分是偶发（pl109/pl110 也出现过停在更早位置、
重刷即恢复）还是 pl112 新增的 head 捕获引入的回归。**需要再观察几次确认。**

电量约 36%，`-1893 mA` 持续放电；若条件允许先插充电器，否则实验不可靠。

---

## 二、已确认为事实的结论

### 2.1 内核侧完全正确（探针实测，pl110）

pl110 的 64 位探针给出的完整记录，逐项核验全部正确：

```
OATN  a=0x16ec504b8 b=0x0 c=0x2/0x3/0x4/0x5/0x6   同一路径，每次 open 成功
FNCT  a=<fd> b=0x61 c=0x0                          F_ADDFILESIGS_RETURN，成功
MMAP  a=0x500040002 b=<fd> c=0x101240000           READ|EXEC, 16 KiB 对齐
MUNM  a=0x101240000 b=0x4000 c=0x0                 与 mmap 一致，长度 16 KiB
```

| 检查项 | 实测 | 判定 |
|---|---|---|
| mmap 返回地址 | `0x101240000` | 16 KiB 对齐 ✓ |
| munmap 地址 | `0x101240000` | 与 mmap 一致 ✓ |
| munmap 长度 | `0x4000` | 正是 16 KiB ✓ |
| prot / flags | `0x5` / `0x40002` | READ\|EXEC / MAP_PRIVATE\|MAP_UNIX03，标准 ✓ |
| fcntl cmd / 返回 | `0x61` / `0x0` | 签名挂载成功 ✓ |
| 循环模式 | 所有字段恒定，仅 fd 递增 | 完美重复同一失败 |

**结论：dyld 在用户态第 4 步（读 header 做校验）失败，第 5 步
（调 map_and_slide）从未发生。这不是内核 bug。**

### 2.2 GPU 驱动是好的

pl105（GPU 组开启）实测 `TB710FU GPU: bringup stage=9/9`：GMU 固件 4/4 加载、
HFI 序列完成、GX 上电、CP ring 就绪、SMMU 零故障。且 pl103（无 GPU）、
pl105（GPU 全通）的 launchd 停滞状态完全一致（`thr=1 fp=182656`）。
**GPU 与卡启动无关；GPU 渲染这条目标项已部分实证。**

### 2.3 探针链完整可用

- 补丁 0053：pid 1 的 openat/mmap/fcntl/munmap 完整参数记录（`noinline` + 字符串 tag）
- `BootWatchdogPolicy.c` 的 `tb710fu_dump_procs()`：`ps` 输出 `TB710FU DYLDPROBE:` 行
- pl112 额外把 mmap 映射的前 32 字节 dump 出来（HEAD/HEADASCII），
  用于验证"映射内容是否与文件一致"——**尚未拿到实机数据**

镜像验证工具：`scratch/verify-pl109.py`（pl109 通过 12/12）。
内核确认工具：`scratch/compare-probe-images.py`、`scratch/check-tag-encoding.py`。

### 2.4 抓取顺序铁律（血泪教训）

内核 console 环形缓冲容量有限，`ps` 的输出（探针 16 组 + 进程表）会把启动序列挤掉。
**刷机后必须先纯读 30-60 秒抓启动序列，再发 `ps` 抓探针**，两次分别存盘。
先发 `ps` 会导致启动序列永久丢失（第 49/53 轮踩过）。

---

## 三、已被证伪的假设（不要再试）

| 假设 | 证伪依据 |
|---|---|
| `errno 12` 是错误 | 文档化的 "shared region is empty"，成功案例也有 |
| GPU 修坏了 | pl105 GPU stage 9/9 全通，launchd 状态与无 GPU 一致 |
| `text-fixup0` 导致退化 | pl103/pl104 都过 launchd，boot-args 逐字相同 |
| mmap 返回地址不对齐 | 实测 `0x101240000`，低 14 位为 0 |
| munmap 地址与 mmap 不符 | 64 位修正后实测完全一致（此前 4 GiB 差值是只读低 32 位的假象） |
| munmap 长度不是 16K | 实测 `0x4000` |
| prot/flags 异常 | `READ\|EXEC` + `MAP_PRIVATE\|MAP_UNIX03`，标准值 |
| `F_ADDFILESIGS_RETURN` 失败 | 实测返回 0 |
| fd 泄漏是病根 | fd 递增是重试的结果，不是原因 |
| 循环已停止 | 只是慢（约 1 分钟 1 轮），idx 从 24 一路涨到 424 |
| pl49 进过完整桌面 | 只有 WindowServer+loginwindow，无 Dock/Finder/SystemUIServer，停在登录窗口 |
| 4K shared-region 补丁回归 | 那些补丁从未进 series |
| 固定延时 settle | `d90e9a4` 卡死内核丢 COM5，已 revert |
| 调度参数 / 数据卷参数 / GopDisplay 铺路 | pl93 带着它们照样成功 |

---

## 四、可用镜像与关键脚本

**镜像**（`out/` 下）：

| 镜像 | 用途 |
|---|---|
| `tb710fu-pl16-46125126722-pl112-head-signed.zip` | 当前刷入，含 head 捕获 |
| `tb710fu-pl16-46122815206-pl110-probe64-signed.zip` | 64 位探针，数据最完整可信 |
| `tb710fu-pl16-46121210251-pl109-probe-signed.zip` | 首个实机探针（返回值只有低 32 位） |
| `tb710fu-pl16-46058104010-pl100-srmap-signed.zip` | 0052 原始追踪 |
| `pl16-46021085431-pl93-5g-srgb-signed/` | 最后一次确认进桌面（583 帧 0 拒帧） |

**脚本**（`scratch/` 下）：

| 脚本 | 用途 |
|---|---|
| `gen-0053-v7.py` | 0053 补丁生成器（**改补丁必须先改它**） |
| `get-kernel.py` / `poll-0053.py` / `dispatch-0053.py` | 构建下载/轮询/触发 |
| `verify-pl109.py` / `compare-probe-images.py` | 镜像完整性验证 |
| `analyse-probe-loop.py` | 探针数据结构化分析 |
| `package-pl112.sh` | 打包脚本（照抄改 run/tag/kernel 即可） |
| `classify-boot.ps1` | 启动判据（unnest + mapped） |
| `boot-monitor.ps1` | 无人值守监视器（结束强制 edl） |

**git 分支**：`main`（原 `cdc-smmu-claim` 支线已于 2026-11-28 16:53 提升为主线并推送；
所有其它本地与远端分支已剪除）。最新提交 `d5fb8f9`（交接文档；探针最新代码提交 `18082c2`，
pl112 head 捕获）。完整历史备份在 `D:\unixnu-tb710fu-clean-backup-20261128.bundle`。
CI 额度无限，仓库公开，放心构建。

---

## 五、无人值守铁律

1. **不抓日志时平板必须在 EDL**（`9008`）。卡启动持续发热伤电池。
2. **绝不让桌面久留**：该板渲染异常下久留会背光黑屏、COM5 枚举但不响应，串口永久丢失。
3. 监视器 `scratch/boot-monitor.ps1` 每代启动结束**强制发 `edl`**。

---

## 六、下一步（按序）

1. **先确认 pl112 是否引入回归**：再启动 2-3 次，看是否总停在
   `SystemCryptex` 后。若恢复过 launchd，则是偶发（电量/运气），
   直接抓 HEAD 数据；若三次都停，回退到 pl110（head 捕获惹的祸）。
2. **拿到 `TB710FU DYLDPROBE HEAD:` 数据后**，与第 32 轮 `cat` 读到的
   `dyld_v1  arm64e` / `mappingOffset=0x238` / `mappingCount=1` 对比。
   - 不一致 → 4K 页映射没有按 16K 布局暴露文件字节，这是根因
   - 一致 → dyld 拒绝的是 header 的其他字段，需要再扩展探针
3. **永恒警告**：本会话推翻过 6 个自己预设的假设（对齐、长度、flags、
   签名、fd 耗尽、循环停止）。**拿到数据前不要预设结论。**
