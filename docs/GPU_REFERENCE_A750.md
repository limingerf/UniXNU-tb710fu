# Adreno 750 参考移植对照（SM8650）

本文记录 2026-11-25 对 UniXNU 设备目录里全部 SM8650 设备的排查结果，以及与上游
`UniXNU/Adreno740` 的逐层对照。目的只有一个：给 TB710FU 选一个可复用、可验证的
GPU／显示参考，而不是凭空写驱动。

## 1. 同 SoC 设备排序（GPU 维度）

| 设备 | 仓库 | GPU 记录状态 | 实际内容 |
|---|---|---|---|
| **zorn / Redmi K80** | `kerrinz/UniXNU-zorn` | `implemented` | Adreno750v2 / Gen7.9.1 direct-ring，含 firmware/GMU/HFI、SMMU、submit、fence、IOGPU/Metal、presentation；真机最远（v572，552 个连续扫描窗口） |
| houji / 小米 14 | `lonzzi/UniXNU-houji` | 诊断级 | GMU/HFI 启动、内存权限、HWSCHED 诊断；GPU 命令提交未完成 |
| giulia / 一加 Ace 5 | `tycykp/UniXNU-OnePlus-Ace5` | 准备服务 | 只有 GPU 准备服务与音频运行时代码，GPU 执行未实机验证 |
| nx769j / 红魔 9 Pro | `Asultop/UniXNU-NX769J` | `planned` | 只有地址表（GPU 0x3d00000、GMU 0x3d6a000、SMMU 0x3da0000） |
| meizu21 / ruyi / nx721j / caihong / phy120 / shennong | — | `planned`／未开始 | 无 GPU 实现 |

结论：**zorn 是唯一把 A750 走通的设备**，其余都停在准备或诊断。

## 2. zorn 的 GPU 栈到底由什么组成

- 驱动主体：`UniXNU/Adreno740`，pin **`4e0314df8ad29d52f6fa3f9383a540adcd618b87`**
  （见 `source-pins.json` 的 `drivers/Adreno740`）。
- 叠加层：`patches/gpu/` 下 **301 个** 补丁目录，覆盖 GMU/HFI、SMMU、ring、
  fence、IOGPU client、Metal、presentation、scanout 与大量错误路径。
- 板级运行时：`native/gpu-owned/`（GMU runtime、reservations、visibility、
  runtime backings、firmware plan），用 Gen7.9.1 自带运行时**替换**了上游的
  `ShengGmu`（SM8550 专用）。
- 板级资源表：`native/gpu-owned/K80GpuResourceProfile.inc`。
- 固件 pin：`native/gpu-owned/firmware-pins.json` —
  `gmu_gen70900.bin`（78,588 B）、`gen70900_sqe.fw`（85,300 B）、
  `gen70900_aqe.fw`（33,176 B）、`gen70900_zap.mbn`（12,088 B）。
- 用户态：`NativeAdrenoMetal.bundle` 安装到
  `/System/Library/Extensions/`，策略文件 `NativeAdrenoMetal.clients` 内容为
  `backboardd`（即只允许渲染进程加载插件）。

zorn 的早期桌面是 **CPU 呈现**（GPU 硬件激活默认关闭），靠 Metal 插件 +
offline accelerator 让 `backboardd` 拿到 Metal 设备；后续才把 direct-ring 真机跑起来。
这一点对 TB710FU 很关键：**桌面不等于 GMU 必须先跑通**。

## 3. 与上游 Adreno740 的对照

上游 `UniXNU/Adreno740` 当前 HEAD `2962e289c9690aeb668072c170679e6fb28bf8de`
（2026-11-25）：

- 没有 `A750` / `gen7-9-1` 任何字符串；`NativeAdrenoA740HwInit.h` 是
  A740 专用的寄存器偏移表（由 `scripts/derive-a740-hwinit.py` 从
  `a6xx.xml` 系列生成）。
- GMU 冷启动依赖 `ShengGmu*`（来自 `kits/Sm8550Platform`），也就是 SM8550 的
  GMU 命令库/votes。SM8650 没有对应的 kit。
- 板级接口是通用的：`NativeAdrenoBoard.h` 只要 MMIO window + SMMU stream ids。
- 也就是说：**直接把 Adreno740 作为 component 加进来，会在 A750 上跑 A740 的
  hwinit，并且缺 ShengGmu kit**——这正是 zorn 用 301 个补丁和自带 GMU runtime
  绕开的部分。

## 4. TB710FU 与 K80 的硬件等价性（可直接复用）

本机 live DTS（`.local/adt/adt-input.dts`）实测：

- `qcom,kgsl-3d0@3d00000`：`qcom,gpu-model = "Adreno750v2"`，
  `qcom,chipid = <0x43051401>`，`compatible = "qcom,adreno-gpu-gen7-9-1"`。
- reg：GPU `0x3d00000/0x40000`、cx_dbgc `0x3d61000/0x3000`、
  rscc `0x3d50000/0x10000`、cx_misc `0x3d9e000/0x2000`、
  qdss_gfx `0x10900000/0x80000`、qdss_etr `0x10048000/0x8000`、
  qdss_tmc `0x10b05000/0x1000`。
- GMU：`0x3d68000/0x37000`、gmu_pdc `0xb280000/0x10000`、
  gmu_ao_blk_dec0 `0x3d40000/0x10000`，`compatible = "qcom,gen7-gmu"`。
- kgsl-smmu：`0x3da0000/0x40000` + tcu `0x3de6000/0x40`。

与 zorn 的 `K80GpuResourceProfile.inc` 逐项一致（含 GMU 0x3d68000/0x37000）。
GPU 代数、chipid、固件族（gen70900）都相同，**zorn 的 A750 hwinit 表、固件 pin、
资源表对 TB710FU 直接成立**，不需要重新推导。

差异只在板级供电/时钟/面板：

- 电源域与时钟：AOP/RSC/ICC/QOS/PDC 的板级投票路径（zorn 用
  `native/gpu-owned/runtime` + 一堆 `zorn_ios26_*_trial.py` 一步步试出来）。
- 面板：TB710FU 3200×2000，K80 1440×3200，presentation/DSI 代码不能照抄。
- 系统：TB710FU 是 macOS 27.3（26C5035r），zorn 是 iOS 26.6.2；
  Metal 插件的 client 从 `backboardd` 换成 `WindowServer`，插件目录与签名策略也不同。

## 5. 建议路线（按收益/风险排序）

**阶段 A（先要桌面，不碰 GMU）**

1. 在 kernel 侧发布一个不触碰硬件的 `IOAcceleratorES` 子类
   （参考 `AdrenoMetal/NativeOfflineAccelerator.cpp`），属性带
   `MetalPluginName`，让 WindowServer 能创建 Metal 设备。
2. 按 zorn 的方式构建并安装 Metal 插件 bundle（client 改成 `WindowServer`），
   先走 offline/CPU 渲染，presentation 仍用现有 IOMobileFramebuffer + GOP。
3. 验收：`WindowServer` 线程数/内存开始增长，出现 Dock/Finder/loginwindow 正常栈，
   屏幕出桌面。

**阶段 B（真 GPU）**

4. 引入 Adreno740（上游 HEAD）+ zorn 的 A750 hwinit / gen70900 固件 /
   `native/gpu-owned/runtime`，先只做到“GMU 冷启动 + SMMU attach + ring 建立”。
5. 再接 submit/fence/IOGPU client，最后替换阶段 A 的 offline 后端。

**阶段 C（面板与呈现）**

6. 用 TB710FU 的 DSI/DSI 时序替换 K80 的 presentation/scanout 常量。

## 6. 明确不做的事

- 不把 zorn 的 301 个补丁整体 cherry-pick 到本仓：它们围绕 K80 的
  AOP/RSC/ICC/QOS 与 iOS26 backboardd 生命周期，直接搬会引入不可验证的假设。
- 不在 A750 上跑 A740 的 hwinit 表或 ShengGmu（SM8550）冷启动。
- 不把“CPU 呈现能进桌面”当成“GPU 验收”；两者分开记录。
