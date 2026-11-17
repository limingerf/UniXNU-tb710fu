# macOS 27 根卷（userdata）构建 · 2026-11-15

## 目标

XNU 已实机进入 EL1，但要挂载根卷还需要 `userdata-root.img`：把 Apple IPSW
里的 OS/SystemOS/App disk image 原样放进一个带 `SHNGROOT` 清单的镜像，
内核按清单找到 APFS 卷。

## 官方构建入口

`unixnu build userdata`（`scripts/unixnu/userdata.py`），流程：

1. `board.toml [userdata]` 给出 GPT 几何与容量上限
2. OS profile 给出 `windows`（本工程 macOS 27 是 OS / SystemOS / App 三个）
3. 从 IPSW 按需提取成员（HTTP range，不下载整包）
4. AEA 加密成员在 macOS runner 上解密（`/usr/bin/aea` + `ipsw fw aea --key`）
5. `hdiutil` 只读挂接、按原始块写出，前置 1 MiB `SHNGROOT` 头
6. 产出 `userdata-root.img` + `userdata-root.json`（含窗口偏移/大小/SHA-256、
   分区容量、flash 命令）

## 本机实测输入

### IPSW（远程，25.3 GiB，只读需要的成员）

```
https://updates.cdn-apple.com/2026FallSeed/
  53df7654-4865-4259-8dae-30177b76f104/
  UniversalMac_27.3_26C5035r_Restore.ipsw
```

- 27,206,093,542 B（25.3 GiB），1801 个 zip 条目
- 实测支持 HTTP range（206 Partial Content）
- `BuildManifest.plist` 有 198 个 identity，profile 指定 `k114cap / Erase`

| 成员 | 文件 | 压缩大小 |
| --- | --- | --- |
| OS | `141-22233-021.dmg.aea` | 10.62 GiB（10880 MiB） |
| Cryptex1,SystemOS | `141-22145-021.dmg.aea` | 2.31 GiB（2368 MiB） |
| Cryptex1,AppOS | `141-22106-021.dmg` | 0.05 GiB（52 MiB） |
| **需读取合计** | | **≈ 13 GiB** |

**不需要下载 25.3 GiB 整包**：`ipsw.py` 用 range 请求只读中央目录、
BuildManifest 和这三个成员。

### GPT 几何（TWRP 中 `sgdisk --print /dev/block/sda` 实测）

4096 字节逻辑扇区，磁盘 61390848 扇区（234.2 GiB）：

```
14  5105064  61390842  214.7 GiB  A03A  userdata
Total free space is 0 sectors
```

- `userdata` 是第 14 个、也是最后一个分区，占满到磁盘末尾
- 容量 `(61390842 - 5105064 + 1) × 4096` = **230,546,550,784 B = 214.71 GiB**
- 这个 214 GiB 是**分区容量上限**，不是产出镜像的大小
- `userdata-root.img` ≈ 1 MiB 头 + 三个窗口（各 1 MiB 对齐）≈ 13–20 GiB

> sysfs `/sys/block/sda/sda14/start` 报 40840512 是 512 字节单位，
> 除以 8 正好是 sgdisk 的 5105064，两者一致。

### 已具备的配置

- profile 自带 `[userdata] windows` 与
  `[payload.adt] root_match_volume_uuid = "84581E99-85BE-4957-AC53-2947E887B3E4"`
  ——即之前发现 ADT 缺的那个属性，profile 其实已给值
- 本工程 `board.toml` 只补了 `[userdata]` 几何
- `unique_chip_id` 是设备私有种子，通过 `UNIXNU_UNIQUE_CHIP_ID` 传入，不入库

## 云构建

`.github/workflows/macos-userdata.yml`（macos-26，180 分钟）：

- 安装 `blacktop/tap/ipsw`（取 AEA key），`/usr/bin/aea` 与 `hdiutil` 为系统自带
- `tools/ci_userdata.py dry-run` → 预检 IPSW 身份/成员/容量
- `tools/ci_userdata.py build` → 提取解密并写出镜像，校验 `image_sha256` 与报告一致
- 上传 `userdata-root.img` + 报告 + 日志（私有 artifact，保留 3 天）

### host 侧已验证

在可写的 pinned UniXNU 副本上跑过真实 dry-run（连真实远程 IPSW）：

```
== ipsw: remote ...UniversalMac_27.3_26C5035r_Restore.ipsw
== userdata: OS would be extracted from 141-22233-021.dmg.aea (10880 MiB)
== userdata: SystemOS would be extracted from 141-22145-021.dmg.aea (2368 MiB)
== userdata: App would be extracted from 141-22106-021.dmg (52 MiB)
== userdata: dry run, nothing extracted or written
```

`profiles.merge` 验证过 board 几何与 profile windows / root_match_volume_uuid
能正确共存。

### 云构建结果（已通过）

run **43724852936**（commit `024ac99`），**success**。

两次运行：43724165867 因工具检查里一句多余的 `ipsw --version`（该工具无此
flag）在 step 5 提前失败，已修正；43724852936 全部 9 步通过。

| 检查 | 结果 |
| --- | --- |
| AEA 工具 | `/usr/bin/aea`、`hdiutil`、`ipsw 3.1.748`（brew）全部就位 |
| dry-run | PASS（IPSW 成员可解析、镜像不超分区容量） |
| `build userdata` | PASS，三个窗口全部写入 |
| 产物校验 | `image_sha256` 与实算一致（脚本内断言） |
| artifact | `tb710fu-macos-userdata-43724852936`，14,135,017,096 B（压缩） |

**产出镜像**（`userdata-root.img`，在 artifact 内）：

| 项 | 值 |
| --- | --- |
| `image_bytes` | **17,466,130,432 B = 16.27 GiB** |
| `image_sha256` | `221d18c76ccd6bcc0ec9141508ef109a254783d029b04e3401993450440ccc7a` |
| `windows` | `OS`、`SystemOS`、`App`（三个都在） |
| `userdata_bytes` | 230,546,550,784 B = 214.71 GiB（分区容量上限） |
| artifact 上传后 digest | `b661a8ddbe4ad0911bbb141baa4b88a40401543bb73328fcd0d18ec02ccd8fb0` |

**镜像只有 16.27 GiB，不是 214 GiB** —— 214 GiB 是分区容量，镜像是
1 MiB `SHNGROOT` 头 + 三个窗口（各 1 MiB 对齐）的实际内容。压缩后 artifact
13.2 GiB。

本机后来收到 run `43724852936` 的 artifact ZIP，下面的策略和 userdata 重建都以该
镜像及其 `userdata-root.json` 为输入。

### Owner policies 与 `.vroot` 表

2026-11-17 从 OS、SystemOS 和 App 原始窗口生成同一 owner key 的 RSA-3072/SHA-256
签名策略。VerifiedRoot 的 C host verifier 对签名和每个 4 KiB block 做了完整验证：

| 窗口 | 已验证 blocks | `.vroot` 大小 | `.vroot` SHA-256 |
| --- | ---: | ---: | --- |
| OS | 3,538,432 | 113,229,824 B | `f10b77a0b2f0475c296a66c69ec3c1d26598dbca3e6b09751a0b1cc31af533b0` |
| SystemOS | 712,192 | 22,790,144 B | `48402d8eccea26cd5e226830f97a08fe3c1d8d2265bdd38977c39b761a79d608` |
| App | 13,312 | 425,984 B | `8e0bfe4d34f4bda99b8f04fb2dc7af157ebf587631ed093b344023c4a092ec62` |

三份验证报告都记录 `hardware_tested=false`、`boot_root_authenticated=false`。公钥哈希为
`c2d8d462d0a790056bef2dddc30ae60ce210dbf0ee1cd4350a21e864db3a7d7e`；公开签名头和
公钥位于 `scripts/policies/26C5035r/`。owner 私钥只保留在本地忽略的构建目录，不入库。

重建后的 `userdata-root-vroot.img` 保留原三个镜像窗口，并以官方 `SHNGROOT v2`
数据对象格式加入三张原始 32-byte/block 哈希表。对象起点均按 1 MiB 对齐，最后对象后
也按官方 builder 补齐。全镜像读回检查了 manifest checksum、所有窗口和表格哈希、零填充
以及最终 SHA-256：

| 项 | 值 |
| --- | --- |
| `image_bytes` | **17,603,493,888 B** |
| `image_sha256` | `35b1c581c7074c626b4b3750af93901a988425e42671f1aaeafd7fd6e13de199` |
| `source_image_sha256` | `221d18c76ccd6bcc0ec9141508ef109a254783d029b04e3401993450440ccc7a` |
| `hardware_tested` | `false` |

新镜像仅完成主机端格式和哈希验证，未刷入设备，也不代表根卷已挂载。

## 尚未解决 / 需要决策

1. **分区写入方式**。`userdata` 现在装满 Android data、GPT 无空闲扇区。
   官方 `dual-system-partition.md` 的保留数据缩容路径因本机 data 多层加密
   （`aes-256-xts:aes-256-cts:v2+inlinecrypt_optimized+wrappedkey_v0` +
   `metadata_encryption=aes-256-xts:wrappedkey_v0`）会被工具拒绝。
   用户表示「清空就行」——即擦掉 214.7 GiB userdata 重建，但这一步**尚未执行**，
   且不可逆。
2. **Data 卷 seed** 是另一独立输入（`--data-ramdisk` + `seed-data-grow.py`，
   3–1024 GiB），不在本轮范围。
3. **根卷 UUID 匹配**：profile 给的 `84581E99-…` 是所选 OS 镜像的 APFS 卷 UUID，
   官方要求用 `diskutil apfs list` 核对实际镜像后重建 payload/ADT。本轮未验。
4. 云构建产物、AEA 解密是否在本 runner 成功，均待 run 结果。

## 边界

本文只记录构建配置与输入事实。编译/提取成功 ≠ 根卷可挂载 ≠ launchd/用户态。
实机刷写、分区擦除、Data 卷初始化均未经授权，尚未执行。
