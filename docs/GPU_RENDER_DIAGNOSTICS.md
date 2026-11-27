# GPU 渲染异常诊断方案（设备恢复后执行）

**来源**：`drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm`（官方源码，只读）
**状态**：未在实机验证过任何一条，仅从源码确认变量存在与语义

---

## 一、为什么用环境变量而不是改代码

`.work/UniXNU/drivers/AdrenoMetal/` 是 fetch 下来的官方源码缓存，项目规则禁止修改。
但该前端已经把渲染诊断做成了运行时开关，**全部通过 getenv 读取**，所以无需改一行代码
就能拿到关键指标。

## 二、可用的开关（源码逐条确认）

| 变量 | 源码位置 | 作用 |
|---|---|---|
| `SHENG_DIAG` | L225 `diagOn()` | 打开诊断报表，**含 `RM gmem/sys` 渲染模式统计** |
| `SHENG_GPU_TRACE` | L66 `gpuTraceOn()` | GPU 追踪环形缓冲 |
| `SHENG_LOG_GPU` | L570 `logGpuOn()` | GPU 日志 |
| `NMB_LOG_STDERR` | L566 `logToStderr()` | 日志同时写 stderr（**关键**：daemon 的 NSLog 只进统一日志，bring-up 板读不到） |
| `NATIVE_METAL_TIMING` | L151 | 计时 |
| `NATIVE_METAL_ABORT_PROBE` | L156 | abort 探针 |
| `NATIVE_METAL_DUMP_TESS` | L862 | 细分着色器转储 |
| `NATIVE_METAL_NO_SHADER_CACHE` | L701/L736 | 禁用着色器缓存（排查缓存导致的错误着色） |

## 三、决定性指标：渲染模式统计

源码 L5419-5430 产生这一行：

```
RM gmem <passes> <Mpx> | sys auto <passes> <Mpx> other <passes> <Mpx> | exact <n>
```

字段含义（L206-207 注释）：
- `gmem` = 在 tile memory 内完成的 pass
- `sys auto` = 被 autotuner 主动选择落到 SYSMEM 的 pass
- `sys other` = **因其他原因**落到 SYSMEM 的 pass
- `exact` = 精确命中

**判读**：
- `gmem` 高、`sys other` ≈ 0 → tiling 路径正常，渲染异常另有原因（更可能是
  `'&BGA'` 压缩表面的 stride/baseOffset，见 GopDisplaySupport）
- `sys other` 显著 > 0 → **tiling 路径有问题**，pass 被迫走 SYSMEM，
  这会直接表现为画面错位/条纹/残缺

这一条能一次性把「GPU tiling 问题」和「显示表面布局问题」分开。

## 四、执行步骤（设备恢复后）

**前提未解决：目前没有已知的 env 注入通路。**

已核实的事实：

- `package_pl16_data.py` 只写 `unixnu.toml` 的 boot-args，**不支持环境变量**
- 全仓库 `docs/` 与 `CURRENT_STATUS.md` 中**没有任何** `WindowServer` 环境变量
  注入的记录（`grep 'EnvironmentVariables|setenv|WindowServer.*env'` 零命中）
- `package/BootWatchdogPolicy.c` 已有完整的「写 launchd plist」机制
  （`mkdir` → `open` → 分块 `copyout`+`write` → `fchown`，L117-159），
  但它写的是 `/private/var/db/com.apple.xpc.launchd/disabled.plist`，
  那是「禁用哪些 job」的清单，**不是** job 的环境变量定义

所以真正要做的第一件事是**找到 `WindowServer` 的 job 定义位置**并确认能否
往里加 `EnvironmentVariables`：

- 候选一：`/System/Library/LaunchDaemons/com.apple.WindowServer.plist`
  （只读系统卷，需要在 payload 里用 overlay 或 shim 覆盖）
- 候选二：`/Library/LaunchDaemons/` 下的同名覆盖（Data 卷可写）
- 候选三：`launchctl setenv`（需要登录会话，bring-up 阶段可能不可用）

在这三条里任一条被实机证实可用之前，**不要花构建额度去试环境变量**。

替代方案（不需要 env）：读 Turnip 自己的统计环形缓冲。源码里
`modeStats(rm)` 是从 `gDevice` 取的，而 `nmb_*` 系列本就暴露给内核侧；
可以改为在 `capture-stall-procs.ps1` 里通过一个新的 CDC 动词读取，
但那是新增内核代码，需要单独评估。

## 五、结论

本方案的价值是**把判据提前定义好**（`RM gmem/sys auto/sys other` 的解读方式），
而不是声称现在就能跑通。设备恢复后先解决 env 注入通路，再看指标。

## 五、不要做的事

- 不要为了加一行 printf 去改 `.work/UniXNU/drivers/AdrenoMetal/`（违反项目规则）
- 不要在还没读到 `RM` 行之前就猜 UBWC/压缩布局是原因（已多次因未验证的假设走弯路）
- 不要把 `gpu-metal=1` / `gpu-power=9` 与渲染质量混为一谈：前者只保证
  Metal 设备发布与 GPU 上电，不保证合成正确
