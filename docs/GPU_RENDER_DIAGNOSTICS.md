# GPU 渲染异常诊断方案（设备恢复后执行）

**来源**：`drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm`（官方源码，只读）
**状态**：用户最近描述的画面大部分为黑色，仅灰色密码框和白色状态栏图标清楚，时间区域间歇出现绿色损坏像素。是否为登录窗口还需要结合进程状态核实。pl93 的 583 次接受/送帧只证明 swap 路径工作，不证明画面正确。plane base 和 post-fence dcache 修复尚未在后续镜像实机验证。

固定 `NativeAdrenoMetalPlugin.mm` 会设置 `CA_DISABLE_FRAMEBUFFER_COMPRESSION=1`，并将 `CAEnableFramebufferCompression` 与 `CAEnableIOSurfaceCompression` 设为 false。当前排查重点是核实设备实际生成的 surface 内容、plane base 和 stride，再用 GMEM/SYSMEM 对照判断渲染差异。

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
- `gmem` 高、`sys other` ≈ 0 → tiling 路径正常，继续核对显示端 IOSurface plane 布局、行步长与像素内容。
- `sys other` 显著 > 0 → **tiling 路径有问题**，pass 被迫走 SYSMEM，
  这会直接表现为画面错位/条纹/残缺

这一条能一次性把「GPU tiling 问题」和「显示表面布局问题」分开。

## 四、一次启动区分渲染路径

无需环境变量注入。固定的 `NativeAdrenoMetalPlugin.mm` 从启动参数读取
`tu-debug=`，插件注释说明 `sysmem,nocb` 绕过 GMEM tile load/store，
`gmem,nocb` 强制 GMEM。`diag=1` 或 `gpu-trace=1` 会启用上游 GPU 诊断。

确认进程已进入待测显示阶段后，一轮使用 `tu-debug=sysmem,nocb` 做对照。伪影消失指向 GMEM/TU
路径，伪影仍在则检查 IOSurface plane 布局和 scanout。当前工作树从
26C5035r plane 表 `+0x10` 读取 base offset、`+0x20` 读取 stride，并将偏移用于 CPU 复制和可选直扫。实际 base 值仍须从成功设备日志确认。

`SHENG_DIAG` 的 RM 报告可进一步区分 gmem 与 sysmem passes。插件会从
`diag=1` 设置这个环境变量，无需写 LaunchDaemon plist。

## 五、下一轮采集

设备到达待测显示阶段后，记录 `disp` 统计、surface 的格式/plane 数/stride/base offset、
时间和菜单栏区域的画面，以及 `loginwindow`、`Dock`、`Finder`、`SystemUIServer` 的进程状态。
随后用 `tu-debug=sysmem,nocb diag=1` 做一轮对照，保存 RM 行和完整渲染日志。插件从启动参数读取这些开关。

## 六、验证边界

- 不要为了加一行 printf 去改 `.work/UniXNU/drivers/AdrenoMetal/`（违反项目规则）
- swap 计数不能代替画面像素检查，RM 行也只描述 Turnip 的渲染模式
- 不要把 `gpu-metal=1` / `gpu-power=9` 与渲染质量混为一谈：前者只保证
  Metal 设备发布与 GPU 上电，不保证合成正确
