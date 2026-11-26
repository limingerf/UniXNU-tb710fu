# TB710FU / SM8650 Adreno 750：GPU + Metal 里程碑 1（接线与发现）

本文件记录 `package/Tb710fuGpu`（内核）、`scripts/build-tb710fu-gpu*.py`（构建）、
`.github/workflows/macos-gpu-addon.yml`（用户态封装）和 `Macos27Services.cpp` 的接线，
以及一条可复现的硬件验证流程。**默认启动不经过任何新代码。**

## 1. 参考项目做到了什么

同 SoC（SM8650 / Adreno 750，gen70900）的 `UniXNU-zorn` 把 GPU 分三层：

| 层 | 组件 | 现状（参考项目自述） |
| --- | --- | --- |
| 内核 | `drivers/Adreno740`（`AGXAccelerator`、`NativeAdrenoUserClient`、SMMU gen7-9-1） | 已建模、已发布过 accelerator，但 GMU 固件启动与真实提交尚未闭环 |
| 电源 | `package/GpuPowerProbe.cpp`（`gpu-power=1..3` 分级、RPMh/RSCC 票、CX/GX GDSC） | 分级探测，写寄存器只在高等级 |
| 用户态 | `drivers/AdrenoMetal`（`NativeAdrenoMetal.bundle` + Turnip/Metal-on-Vulkan 桥） | macOS 侧走 `IOGPUMetalDevice` + `NativeAdrenoMetalDevice` |

关键结论（决定了本板的顺序）：**Metal 发现只需要一个能被 `IOServiceMatching("IOAccelerator")` 匹配、
带 `MetalPluginName` 的 IOService**；真正出图还要 GMU 固件、SMMU、Turnip 的 Vulkan 设备与内核提交路径。
所以里程碑 1 只做“发现 + 接线 + 证据”，不做任何 MMIO 写。

## 2. 里程碑 1 的实现（本仓库）

- `package/Tb710fuGpu/Tb710fuGpuABI.h`
  - 以 `IOAccelerator : public IOService`、`OSDeclareAbstractStructors` 声明 macOS 26C5035r 的这条基类；
  - `static_assert(sizeof(IOService) == 136)`、`static_assert(sizeof(IOAccelerator) == 136)`；
    基类自身不声明任何 virtual，子类只覆盖 `IOService` 已有槽位（`newUserClient`、`copyProperty`）。
- `package/Tb710fuGpu/Tb710fuGpuAccelerator.cpp`
  - `Tb710fuGpuAccelerator : IOAccelerator`，`OSDefineMetaClassAndStructors`；
  - 发布 `MetalPluginName=NativeAdrenoMetal`、`MetalPluginClassName=NativeAdrenoMetalDevice`、
    `NativeSandboxGrantProcess=*`，并 `attach(IOService::getPlatform())` + `registerService()`；
  - `newUserClient` 返回 `kIOReturnUnsupported`（本里程碑不提供 GPU 用户端），只计数不打印
    （该路径持锁且关抢占，pl49 已因在这里写控制台翻过车）；
  - `copyProperty` 只计数；首次读到 `MetalPluginName` 打一行，作为“发现确实到达”的证据。
- `package/Tb710fuGpu/Tb710fuGpuPower.cpp`
  - `gpu-power=<n>`：1 = 只读普查（ADT `kgsl-3d0`/`kgsl-smmu@3da0000`、payload addon 条目），
    2 = 额外发布 `Tb710fuGpuPower`（普通 IOService）作为后续 GMU/SMMU 阶段的注册表锚点；
  - **不建映射、不写寄存器**，`scripts/check-tb710fu-gpu-host.py` 用标记词静态检查这一点。
- `scripts/build-tb710fu-gpu.py`：用真实 IOKit C++ 记录编译两个 TU，把 `__GLOBAL__sub_I_*`
  移出 `__mod_init_func` 到 `__mod_deferred`，跑 `kernel_target.check_object`（PAC/vtable），
  归档 `libTb710fuGpu.a`，并失效 XNU 链接目标。
- `scripts/build-tb710fu-gpu-addon.py`：在 macOS runner 上按官方顺序
  `build-turnip-darwin.py --target macos27` → `build-adreno-metal-plugin.py --target macos27 --no-replay`
  → `make-gpu-addon.py --prefix GPUBundles --no-daemon [--publish]`，可选 `addon-add-dir.py` 加固件、
  `trust-append-module.py` 追加 TrustCache；随后解析 NGPUADD2 自检条目与路径上限（160 字节）。
- `.github/workflows/macos-gpu-addon.yml`：仅 `workflow_dispatch`，产出 `gpuaddon.bin` + `report.json`。

## 3. 启动参数

| 参数 | 作用 | 默认 |
| --- | --- | --- |
| `gpu-metal=1` | 发布 IOAccelerator（发现路径） | 关闭 |
| `gpu-power=1` \| `2` | GPU 普查；2 额外发布电源域节点 | 关闭 |
| `gpu-addon=1` | 官方 XNU 补丁：把 addon 的 `GPUBundles/*` 写入 Data 的 `/Library/GPUBundles` 并加 rootless 保护 | 关闭 |
| `gpu-bundle-system=1` | 可选：再把同一批 bundle 以只读 union 显示到 `/System/Library/Extensions` | 关闭 |

`gpu-metal=1` 与 `gpu-power` 都由内核侧自己解析：两个都不给时，`tb710fu_gpu_probe()` 直接返回，
`tb710fu_gpu_start()` 一行也不打印，行为与零抖动基线完全一致。

## 4. 固件（本里程碑只登记、不加载）

`scripts/data/tb710fu-gpu-firmware.json` 固定了四件 gen70900 镜像（大小 + SHA-256 与参考项目
`native/gpu-owned/firmware-pins.json` 一致）：

| 文件 | 字节 | 本板来源 |
| --- | --- | --- |
| `gmu_gen70900.bin` | 78,588 | `/vendor/firmware/gmu_gen70900.bin` |
| `gen70900_sqe.fw` | 85,300 | `/vendor/firmware/gen70900_sqe.fw` |
| `gen70900_aqe.fw` | 33,176 | `/vendor/firmware/gen70900_aqe.fw` |
| `gen70900_zap.mbn` | 12,088 | `/odm/firmware/gen70900_zap.mbn` |

addon 内前缀 `Gpu/Firmware/`（内核会写到 `/private/var/ngpu/Gpu/Firmware/`）。
`--firmware DIR` 会在打包前逐个校验大小与摘要，不符直接失败。

## 5. 构建与硬件验证

1. 内核：正常跑 `macos-kernel.yml`（`libTb710fuGpu.a` 已进 `board.toml` 与 defconfig），
   `scripts/check-tb710fu-gpu-host.py` 在 CI 里先过。
2. 用户态：手动触发 `macos-gpu-addon`，得 `gpuaddon.bin`；把它作为 `--gpu-addon` 传给
   `unixnu build payload`（替换现有 `boot-shims.bin`，或与 ADSP 固件合并后的那份）。
   addon 预算约 7.4 MiB（payload 78 MiB / 上限 85,024,768 字节），脚本会告警。
3. CDHash：`report.json` 的 `cdhashes` 必须进 owner TrustCache
   （`trust-append-module.py`），否则 IOGPU 载入 bundle 会被 AMFI 拒绝。
4. 首轮只加 `gpu-addon=1 gpu-power=1`：验证 bundle 落位（`/Library/GPUBundles/NativeAdrenoMetal.bundle`）
   与普查行（`TB710FU GPU: ADT kgsl-3d0 model=... smmu=1`、`addon bundle=1 firmware=4/4`）。
5. 次轮加 `gpu-metal=1`：验证
   - `TB710FU GPU: IOAccelerator size=0x88, base resolved`
   - `TB710FU GPU: accelerator registered plugin=NativeAdrenoMetal ...`
   - `TB710FU GPU: discovery read MetalPluginName (accelerator reached)`（说明 IOGPU 真的读了属性）
   - WindowServer 的 `STR: No suitable Metal devices present after 30 seconds` 若被替换成
     插件自己的失败标记（`0x6800`/`0x68f1` 等），说明发现链走通、只差设备。
6. 只有 `--publish`（`NativeAdrenoMetal.publish`）+ Turnip 能开出 Vulkan 设备时，
   `NativeAdrenoMetalDevice` 才会被发布；那一步需要 GMU 固件与 SMMU 真的工作，属于里程碑 2。

## 6. 明确不包含

- 不写 GPUCC/GX/CX GDSC、不投 RPMh 票、不启 GMU、不建 SMMU 域、不加载上述四件固件。
- 不提供 `newUserClient` 实现：`NativeAdrenoUserClient` 的 ABI 与提交环在里程碑 2 才接。
- 因此**本里程碑不会让 WindowServer 画出桌面**；它把“30 秒无设备”变成可定位的发现结果。
