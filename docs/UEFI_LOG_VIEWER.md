# UEFI 查看上次内核日志 · 2026-11-15

本轮交付是 **UEFI 重启后查看保留的 console 快照**，不是实时 USB、ADB、
fastbootd 或一次性 Android 引导器。用户确认当前 A 槽能启动 Android 和 TWRP；
旧备份中两个 boot 均为 UEFI 的记录不能代表当前 A 槽。

## 使用与边界

固件启动时，`Tb710fuRamoopsDxe` 在 BDS 之前复制 `0xAC340000+0x40000`
到独立 BootServices 池。菜单自动补出 `Tools`（已有则不重复），进入后选择：

- `Last XNU log (ramoops ECC 0)`：当前 ecc=0 内核参数对应布局。
- `Last XNU log (ramoops ECC 16)`：早期 ecc=16 产物的备用布局，不做纠错。

从最后一页开始，音量加/减翻页，电源返回。状态栏显示原始 `sig/start/size`
和所选布局容量；无效头显示诊断信息，原始内存保持不变。

不会写磁盘、GPT、槽位变量或 ramoops。未接入现有未完成的 fastboot 平台协议壳。
此模块本身不保证 DDR 跨复位保存，也无法挽救 XBL/ABL 在进入本固件之前覆盖的数据。
若 XNU 未运行到 ramoops 初始化，早期 4096 字节暂存不会持久化。快照可能来自
之前的 Linux/Android；仅有效头不能证明是本次 XNU 日志，需核对正文横幅。

当前 payload FAT 是只读 RAM 副本，不能用来持久保存日志。UEFI 服务在
ExitBootServices 后结束；XNU 运行期间的 USB 日志需要内核自身驱动。

## 来源与格式

- 板级地址来自已保存的 live FDT，布局见 [RAMOOPS.md](RAMOOPS.md)。
  `MemoryMapLib.c` 的 `Kernel` Reserv 区已覆盖此范围，40 项内存表未改。
- Linux `persistent_ram` 参考固定为
  [SpendyYT/tb710fu-linux@4d044107](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/fs/pstore/ram_core.c)。
  实际写端为仓库 `0004-tb710fu-ramoops-console.patch`。
- 12 字节小端头：签名 `0x43474244`、`start`、`size`。验证
  `start <= size <= capacity` 后按 `data[start:size] + data[0:start]` 读取。
  ecc=0 容量 262132 字节，ecc=16/128 字节块容量 232980 字节。
  格式没有 ECC 模式标记，用户必须选择匹配布局。
- Tools 协议和 8×19 ASCII 字体来自
  `UniXNU/UniXNU@0c1bac3d51a3009dca1708b15a5198f4b43d057a`：
  `package/UniXNUPkg/Include/Protocol/UniXNUMenuPlugin.h`、
  `package/UniXNUPkg/Library/UniXNULib/Font.c`。字体保留 Intel 原始版权。
- `0003-unixnu-menu-tools.patch` 是对此固定框架的显式补丁：仅在发现有效
  Tools 插件且配置缺少 Tools 动作时追加入口，保留原有默认项、次序和倒计时。
  CI 在独立 checkout 应用补丁，不修改本机 fetched source cache。

## 验证

主机测试直接编译生产 `Ramoops.h`，检查无效头、两种容量、部分填充/多次环绕、
逐字节时序、CRLF/换行/折行、控制字符过滤、20000 个破损头及只读性。
Debian GCC 严格 C11 + ASan/UBSan 通过；云 CI 执行同一测试后构建固件，
并在实际嵌套 FFS 中检查新驱动 GUID `69454faf-8c51-4e01-b354-e3fb21e5c745`。
图形绘制、UEFI 事件和实机 DDR 保留需要设备验证，主机模型不覆盖这些路径。

## 一次性 A 槽引导：后续阶段

目标是保持 B 为默认，在内存中加载 A 槽 Linux/Android 镜像并传递匹配的
slot suffix、DTB/DTBO、vendor/recovery/init ramdisk；正常重启继续由 B 启动 UEFI。
`set-active=a` 不是一次性操作，不作为此功能的替代；也不写未经核实的 GPT 位。

当前已构建的 UFS 变体仍缺实机 BlockIo 读取证据；本次未读取当前 A 槽启动镜像，
因此 EFI stub、压缩格式、ramdisk 组合及 AVB/ABL 交接尚未验证。
不能只把 `boot_a.img` 交给普通 EFI `LoadImage`，也不能只添加菜单便声称可启动。
本轮不做设备重启、刷写、签名或槽位变更。
