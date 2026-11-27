# 整夜无人值守策略 · 2026-11-28 夜

## 铁律（用户明确要求）

1. **不抓日志时平板必须在 EDL。** 卡在启动会持续发热，伤电池。
2. **绝不让桌面久留。** 该板在渲染异常下久留会背光黑屏，COM5 仍枚举但不再响应，
   串口从此永久丢失，整夜就无法再无人值守。
3. 因此监视器每代启动都**必须**以 `edl` 收尾，无论成功失败。

## 当前状态（03:55）

- 已刷入 `pl100-srmap`（含补丁 0052，打印 `TB710FU SRMAP[...]`）。
- 设备**完全没有 USB 枚举**：无 COM5、无 9008、无任何 `VID_05C6/1209/18D1`。
- 与「卡初始化后自行重启」不符：重启循环中至少应短暂出现 COM5 或 9008。
  最可能是停在 UEFI 菜单等待选择，或 boot_b 启动失败后停住（不枚举 USB）。

## 监视器

`scratch/boot-monitor.ps1`，参数 `-WatchMinutes 480 -ObserveSeconds 40 -MaxBoots 40`。

行为：
- 每代启动抓**完整原始串口文本**到 `boot_monitor_<ts>.-raw/genNN-<VERDICT>.log`
- 判读：`DESKTOP`（ps 见 WindowServer 或 disp transfers>0）/ `BSD` / `STUCK` / `NOSHELL`
- 每代结束**强制发 `edl`**，并等待 9008 出现确认
- 若端口中途消失（重启），记录 `restart #N` 后等它回来

## 醒来后的第一件事

看 `scratch/boot_monitor_*.log` 的末段和 `-raw/` 里的 gen 文件：

- 有 `TB710FU SRMAP[...]` → 说明 dyld 确实调用了 map_and_slide，
  看 `exit kr=0x..` 就知道失败码，直接定位。
- **没有** SRMAP 行但启动卡住 → dyld 根本没走到 syscall 536，
  问题在 dyld 解析共享缓存之前的某一步（读文件/验证），换方向查。
- 若监视器未发现任何启动（USB 一直不枚举）→ 平板停在 UEFI 菜单，
  需要人工确认后重新上电引导。

## 不要再做的事

- 不要再写基于固定延时的"settle 窗口"补丁（已证明无因果：pl93 里 GopDisplay
  在 launchd 之后启动仍然成功）。
- 不要把 `check_np` 的 `errno 12` 当错误（它是文档化的 "shared region is empty"，
  每个成功启动都有）。
- 不要在 `.work/xnu-patch-lf-clean` 上直接 `git diff` 造补丁（该工作树有历史残留，
  必须先 `git worktree add --detach HEAD` 得到纯净树）。
