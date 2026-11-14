# TB710FU / UniXNU clean bring-up

为 Lenovo TB710FU / SM8650Q-AB 六核设备重新建立的干净适配工程。

**最新结果见 [CURRENT_STATUS.md](CURRENT_STATUS.md)：内核编译、严格预链接、生产加载器解析与UEFI云构建均已通过。**

当前重点是在 GitHub macOS runner 产出可核验的 arm64e 内核。
硬件启动、UFS、根卷、macOS 用户态与桌面分别验收。

## 云编译

- 框架固定：`UniXNU/UniXNU@0c1bac3d51a3009dca1708b15a5198f4b43d057a`。
- 系统固定：`macos-27.3-26C5035r`；每条官方命令显式选择此 profile。
- 主机：GitHub `macos-26`、Xcode 26.6、Python 3.13。
- Actions → **TB710FU macOS 27 kernel** → Run workflow。
- `native`：真实编译/链接，使用官方 `--skip-prelink`；不加入原版 kernelcache kext。
- `prelink`：先编译 native，再运行官方 macOS kernelcache 预链接；闭包另行验证。
- 日志、源码锁、内核和 SHA-256 保存在本私有仓的 Actions artifact，默认保留三天。

UEFI 使用独立的 **TB710FU reviewed UEFI** workflow。它应用 `firmware/overlay` 与
独立 SimpleFb 修复，再调用官方 `build firmware`；已通过云编译及本地复验，产物为3MiB FD，
最新按键修复候选run43636909923包含10个必需模块，两个真实FV及14个overlay文件哈希复验通过；
证据在 `docs/evidence/uefi-input-build.json`。实体按键响应仍需该FD的实机验证，历史构建记录保留。
首轮 native 已成功，独立校验记录在 `docs/evidence/native-build.json`；完整预链接单独验收。

最小 kernel 构建无需 PAT、平台种子、Data seed 或 trust cache 的 Actions secret。
默认 `github.token` 只用于源码 API；本机保存的个人令牌不会送入 workflow。

## 本地配置检查

按官方文档准备主机工具后：

```sh
git clone https://github.com/UniXNU/UniXNU .work/UniXNU
git -C .work/UniXNU checkout --detach 0c1bac3d51a3009dca1708b15a5198f4b43d057a
python3 tools/ci_kernel.py configure
python3 tools/ci_kernel.py native
```

Windows 可做配置检查；内核编译在 macOS 云端进行。`.work` 中的源码、缓存和日志不入 Git。

## 文件归属

| 文件 | 用途 |
| --- | --- |
| `board.toml`、`configs/` | 最小 TB710FU/macOS 配置，不带他机驱动或内存占位值 |
| `soc/`、`hardware.toml` | SoC 身份与硬件事实清单；六核裁剪不污染通用 SoC |
| `patches/pre-os/` | 官方 boot-progress 的上下文依赖，不启用 SM8550 电源实现 |
| `patches/xnu/` | 两个可选功能 gate；无空桩或伪造安全结果 |
| `upstream.lock.json` | 顶层版本锁；官方依赖锁是另一个 TOML `unixnu.lock` |
| `porting.toml` | 按官方等级维护的适配记录 |
| `docs/CLEAN_REBUILD.md` | 错误根因、重建决定与后续顺序 |
| `docs/HARDWARE.md` | 可复用与待复核的板级资料 |

旧目录 `D:/unixnu-tb710fu-plan` 保留供取证。当前任务不运行设备命令、签名、刷写或分区操作。

官方文档：[macOS](https://unixnu.com/zh/porting/macos)、[构建](https://unixnu.com/zh/building)、[启动验收](https://unixnu.com/zh/porting/bring-up)。
