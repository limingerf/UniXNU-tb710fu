# 干净重建记录

最终验收与绿色验证run见仓库根目录 `CURRENT_STATUS.md`。本页保留错误根因与重建顺序；
内核严格预链接及DarwinBoot生产解析器、UEFI云编译和本地FD复验现已通过。

日期：2026-11-15。按用户授权创建新目录、私有 GitHub 仓库与 macOS 云编译。旧工程和设备不改动。

## 旧链接失败的实际原因

旧工程混合了 Linux 工具链补丁、手改 XNU feed、过期导出头文件和多轮固件试验。
最终几组未定义符号不能简单归因为归档列表没有传入链接器；全新官方版本也存在以下依赖关系：

| 症状 | 原因 | 当前处理 |
| --- | --- | --- |
| `_source_corecrypto14_original/start_once` 未定义 | 默认 prelinked 模式删除 prng_random 的 source provider 定义，OSKext 路径仍有引用 | 使用 source crypto，与当前官方 sheng macOS 一致，仍构建真实 corecrypto 归档 |
| `_native_owner_root_accepted/mount_device` 未定义 | macOS fallback 无条件依赖 audited storage 的状态 | 存储未实现时显式关闭整个 fallback，保留正常 EAUTH，不伪造授权 |
| `_native_exit_trace` 未定义 | 最小板级没有 BootUiTraceProbe 的可选诊断 hook | 对整个可选调用加 gate，保留普通 exec/exit 日志，无空函数 |
| boot-progress 补丁不能应用 | OS 序列预期存在共享平台电源块 | 先应用固定官方上下文补丁，保持 XNU_SHENG_PMGR=0 |
| arm64/arm64e 与 ptrauth 冲突 | 旧 Linux 路径、手工归档和 stale headers 混用 | 从干净 macOS26/Xcode26.6 开始，不迁移旧编译缓存 |
| 预链接工具来源不一致 | 官方主仓 gitlink 为 440589b0，catalog fallback 仍是 c645d76b；后者缺 macOS fileset 名称匹配路径 | feeds 显式锁定官方 gitlink 的 440589b0，不依赖是否初始化子模块来偶然选版本 |

两个本地 gate 的语义见 `patches/xnu/README.md`。后续接入真实 owner-verified storage 或
exit trace 时必须同时启用 provider 和对应编译定义，再做运行验证。当前最小内核不具备这些板级功能。

## 云构建证据

- 工具链预检通过：https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43614621144
- 首轮 native：https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43615291360

以 run 的提交、结论、`build-result.json`、官方 kernel record 与日志为准。native-only 仅证明
ARM64e MH_EXECUTE 已编译/链接；完整预链接、payload、固件交接、根卷与用户态是独立关卡。

每条 CI 命令显式选择完整 macOS profile，避免直接复制官方 workflow 默认选中 iPadOS。
私有 artifact 交付内核与日志；个人 PAT 不入库，也不作为 Actions secret。

## 后续顺序

1. 以首轮真实日志修正最早失败点，得到 native kernel 与 hash。
2. 完成 macOS kernelcache 预链接闭包，不加入安全/硬件空桩。
3. 从旧备份和参考中提取本机原始 DT/UEFI 输入；不复制零长度 RAM 的手写 DTS。
4. 按官方 DSC/FDF 接入 package、DarwinBoot 与最小固件；先不重编程厂商显示时钟。
5. 验证单核 EL1/4KiB/PAC 与 XNU 入口，扩核时逐核核对。
6. 实现真实 UFS、独立 unixnu 存储、root policy、Data 与日志读取。
7. 先固定 macOS Data seed UUID，再以同一组 kernel/payload 输入构建；首次 seed 制作需要 macOS root 工具。
8. 证明 launchd 与后续进程执行后，再做显示/HID/TimeSync、桌面和 GPU。

旧 70GiB Android / 约144.71GiB UnixNU 布局仅为预览；本工程不自动执行分区或刷写。
