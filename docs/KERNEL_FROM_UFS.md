# 内核从 UFS 分区加载：源码级确认

## 结论

把 `\EFI\UniXNU\unixnu.toml` 和 `Payloads/kernel.macho` 一起放进 `unixnu`
分区（FAT），内核即可从 UFS 读取，**不需要**塞进 96 MiB 的 `vendor_boot`
ramdisk。137 MiB 的 macOS 27 内核因此不受 payload 容量限制。

## 证据

`package/UniXNUPkg/Library/UniXNULib/Config.c`

### 1. 配置卷的发现顺序

```c
// Config.c:1547-1563
// The volume UniXNU was started from wins; then every other volume.
if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, ...))) {
  Loaded = TryVolume (Config, Self->DeviceHandle);
}
if (!Loaded && (Config->Error[0] == '\0')) {
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, ...);
  for (Index = 0; Index < Count && !Loaded; Index++) {
    Loaded = TryVolume (Config, Handles[Index]);
  }
}
```

UniXNU 遍历**所有已发布的 SimpleFileSystem 卷**找 `unixnu.toml`，不限于启动卷。

### 2. 菜单项绑定到配置所在卷

```c
// Config.c:1132
Entry->Volume = Config->Volume;
```

每个菜单项（含 `xnu`）的文件路径都在 `Config->Volume` 上解析。
`Config->Volume` 由 `TryVolume` 设置为找到 `unixnu.toml` 的那个卷（`Config.c:1514`）。

### 3. 结论

`unixnu.toml` 与 `kernel.macho` 同卷即可。只要 `unixnu` 分区被发布为
FAT 卷，内核就从那里读。`vendor_boot` 的 payload ramdisk 可以只放配置，
甚至完全不放（若配置在 UFS 卷上）。

## 依赖

1. UEFI 必须发布 UFS 的 `SimpleFileSystem` —— 需要 `UFSDxe` + `PartitionDxe`
   + `FatPkg`（后两者已在基线卷里）。
2. `unixnu` 分区必须是 FAT 格式（EnhancedFatDxe 可挂载）。
3. GPT 里要有 `unixnu` 分区项——按官方 `scripts/dual_sys/partition.py`
   从 `userdata` 尾部切出。

## 容量对比

| 方案 | 内核位置 | 容量上限 | 状态 |
| --- | --- | --- | --- |
| vendor_boot ramdisk | payload FAT | 81 MiB | ✗ 装不下 137 MiB |
| **unixnu 分区** | UFS FAT 卷 | 分区大小（数 GiB） | 待 UFS 可见后实施 |
| fastboot boot | RAM | 无限制 | ✗ ABL 不支持 |

## 下一步

1. 实机确认 UFSFD（run 43692304879，`dfca596a…`）能否发布 UFS 块设备。
2. 若能：切 `unixnu` 分区（需 TWRP/recovery 或 adb root），格式化为 FAT，
   放入 `EFI/UniXNU/unixnu.toml` + `Payloads/kernel.macho`。
3. 若黑屏：按 `docs/UFS_BUILD.md` 的失败模式逐个排查 ClockDxe/RpmhDxe。
