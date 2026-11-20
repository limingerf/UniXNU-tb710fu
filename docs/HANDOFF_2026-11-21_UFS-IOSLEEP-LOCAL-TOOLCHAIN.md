# 交接文档 · TB710FU macOS 27 UFS 停点定位与本地工具链

交接时间：2026-11-21 04:06（Asia/Shanghai）
工作目录：`D:\unixnu-tb710fu-clean`
分支：`codex/ufs-transactional-handoff`
最近提交：`8eb1a7d`（Trace the SCSI response header after the wake command）

---

## 1. 一句话现状

UFS 驱动已证明**传输层完全正常**，卡点精确定位为 `hostBringUp()` 里
`IOSleep (50)`——该路径运行在屏蔽中断上下文，调度器无法唤醒睡眠中的线程。
下一步是把这个 `IOSleep` 换成忙等（补丁 `0006` 已写好，待验证并派发）。

---

## 2. 已完成的实机结论（这是最有价值的部分，不要重新推导）

### 2.1 已证明正常的环节

实机照片（03:52）逐行确认：

```
host gcc-state gdsc=f022f800 axi=00000001 ahb=00000003
host gcc-state-accepted          ← GCC 时钟/电源检查通过
host controller-map-done
host dma-ready phys=8ca6e000 contig=139264   ← DMA 连续映射正常
host smmu-map-done
host queue-snapshot-done / queue-stop-done / queue-start-done
host nop-done                     ← NOP OUT 成功：UTR L/UTRD、doorbell、SMMU、DMA 全通
host power-query-done power=00000033
```

**NOP OUT 成功是决定性的**：UFS 协议栈已通，问题不在硬件、不在控制器、不在 DMA。

### 2.2 唤醒命令完整成功

```
host wake-begin
host cmd-enter      lun=208 op=1b len=6 dir=0 bytes=0 timeout=500000
host cmd-ucd-cleared
host cmd-submit
host cmd-doorbell   db=00000000 is=00000000
host cmd-deadline   now=437709030 deadline=443309036 timeout_us=500000
host cmd-ring-done
host cmd-return     end=0 ocs=0 polls=1        ← 一次轮询完成
host rsp-head       b0=21 b1=00 b2=d0 b3=00 ... want=21/208/0   ← 完全匹配
host rsp-ok len=0
host scsi-tail      scsi=0000 resid=0 len=0
host scsi-done status=0
host wake-done      ← hostScsi 已返回调用方
host wake-settle-begin
（无 wake-settle-done）        ← 停在这里
页脚：ufssetl
```

### 2.3 最终结论

**卡死点在 `NativeUfs.cpp` 的 `hostBringUp()` 内，`IOSleep (50)` 处。**

依据：
- `wake-done` 已打印 → `hostScsi` 正常返回
- `wake-settle-begin` 已打印 → 执行进入 `IOSleep(50)` 前一行
- `wake-settle-done` 从未打印 → 从未走出 `IOSleep(50)`
- 页脚停在 `ufssetl`（`IOSleep` 之前的 checkpoint），`ufssetd` 未出现

机制：`RootCommands.h:293` 注释写明 UFS 命令链 "Commands only poll ... so they
also run with interrupts masked"。`IOSleep` 需要调度器唤醒线程，在屏蔽中断的
上下文里线程再也不会被唤醒，于是永久挂起，且不产生 panic。

`lun-ready` 重试循环里的 `IOSleep (10)` 同理。

---

## 3. 补丁体系（职责必须分离，别再混用）

| 补丁 | 职责 | 说明 |
|---|---|---|
| `0001-sm8650-compatible.patch` | SoC 兼容 | |
| `0002-transactional-handoff.patch` | 事务式队列/SMMU 接管 | |
| `0004-bringup-stage-trace.patch` | **纯追踪**，不改行为 | 57 条阶段追踪 + 8 条 cmd 探针 + poll 探针 |
| `0005-synchronous-bringup-checkpoints.patch` | 同步刷屏 checkpoint | 20 处 `ufsCheckpoint`，由另一轮协作产出 |
| `0006-quiescent-sleep-replacement.patch` | **行为修复** | 刚写好，待验证 |

应用顺序：`0001 → 0002 → 0004 → 0005 → 0006`（`0003` 由 source-storage 构建器追加）。

### 3.1 关键教训（务必遵守）

**`0004` 是纯追踪补丁，验证器 `tools/check_ufs_bringup_trace.py` 的全部等价性
证明都依赖这个前提**（剥离追踪后必须与 post-0002 源逐字符相同）。

我曾把 `IOSleep` 替换直接塞进 `0004`，结果：
- `IOSleep constants unchanged` 校验失败
- 新标签不在 `FOOTER_STAGES` 白名单
- `0005` 上下文冲突
- 连锁改了 3 轮验证器才放弃

**行为改动必须单独成补丁（现在就是 `0006`）。**

### 3.2 白名单机制

`FOOTER_STAGES` 是**精确集合相等**比对（`set(stages) == FOOTER_STAGES`）。
新增 footer 标签必须同时加进白名单，且 `CHECKPOINT` 正则要同时作用于比对两侧。
当前白名单 22 项（含 `ufssetl`、`ufssetd`）。

---

## 4. 立即可做的下一步

### 4.1 验证 0006 并派发构建

`0006` 已生成（2105 字节，2 hunk），已注册到
`scripts/build-tb710fu-ufs-adapter.py`（在 `0005` 之后）。

**尚未做的**：
1. 跑序列验证：`bash .work/test-ufs-series.sh`（需先把 0006 加进该脚本的列表）
2. 跑验证器：`python3 tools/check_ufs_bringup_trace.py --source .work/UniXNU/out/feeds/ShengUfs-38c19c2b`
   - 注意：验证器会因 `IOSleep` 常量变化而 FAIL，需要像之前那样把
     `RECLASSIFIED_IOSLEEP = {"50", "10"}` 加入守卫逻辑
   - 之前这个改动被回退了（因为当时 0004 被污染）；现在 0006 是独立补丁，
     逻辑上应该重新考虑怎么让验证器区分 0004 与 0006
3. host 测试编译：`bash .work/host-test-real2.sh`
4. 提交 → 推送 → 派发 `ufs-integrated`

### 4.2 已就绪、可直接刷机的镜像

**`out/live-checkpoints-44802380583-settle-signed.zip`**
- 35,559,035 字节，SHA-256 `e9a3503834d5d190124a2196bd037d2d91d25f9de790a8e57cbcaf481a3fc749`
- `vendor_boot.img` `a3fc67ab9749a4b06ba829d96f86910e831e276699afaab0814bb31d2877589f`
- `vbmeta.img` `ae79b5dcb02a3b95d7b00be0a16d1f407b8bb3a94314b5c0203831555b01afa2`
- `boot.img` `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`（基线）
- 构建标识 `src-ae2a2128-ramoops`，内核 `7425619681c9d6a4`

这版就是产出上述结论的镜像，已经刷过。

---

## 5. 本地工具链（WSL/Linux 编译内核）

### 5.1 结论：arm64e **可以**编译

实测：
```
$ .work/UniXNU/.linux-toolchain/llvm/bin/clang -target arm64e-apple-macos26.0 -c t.c -o t.o
exit=0
Mach header
      magic  cputype cpusubtype  caps    filetype
MH_MAGIC_64    ARM64          E USR00      OBJECT
```
`cpusubtype` 列的 `E` 就是 `CPU_SUBTYPE_ARM64E`。

之前"不支持 arm64e"的说法**不准确**，实际含义见下一节。

### 5.2 已装好的工具链

位置：`.work/UniXNU/.linux-toolchain`（179MB）
- `llvm/` 交叉 clang（带 `llvm-xnu-builtin-type.patch`）
- `CommandLineTools/`：ld64、nm、lipo、otool、libtool（来自 cctools-port 1030.6.3-ld64-956.6）
- `XcodeDefault.xctoolchain/`：iig
- `bin/`、`lib/`、`include/`：lzfse、libdispatch
- `scripts/linux/`：migcom、codesign/plutil/gm4/install 等 shim

安装命令（需要 root）：
```bash
sudo python3 .work/UniXNU/scripts/linux/setup-toolchain.py \
  .work/UniXNU/.linux-toolchain --package-manager apt -j 6
```
7GB 内存机器用 `-j6`；LLVM 3136 个 target，约 40-60 分钟。

### 5.3 本地编译内核：当前卡在哪

`python3 tools/ci_kernel.py native` 已经能跑起来并推进到真实编译阶段。

**已解决的坑**：
- `env: 'python3\r': No such file or directory` → `AvailabilityVersions` 源的
  shebang 是 CRLF。已修（19 个文件，`sed -i 's/\r$//'`）。若重新 fetch 需重修。

**当前卡点**（`firehose_buffer.c` 编译）：
```
error: '__ptrauth' qualifier must take between 1 and 3 arguments
  OS_PTRAUTH_SIGNED_PTR_AUTH_NULL(type)
  __ptrauth(ptrauth_key_process_independent_data, 1,
            ptrauth_string_discriminator(type), "authenticates-null-values")
```

**根因**：macOS 27 的 `os/base.h:355` 用了 Apple clang 才支持的**四参数**
`__ptrauth`（第 4 个是 `"authenticates-null-values"` 布尔标志）。
上游 LLVM 只接受 1-3 参数，且把 `AuthenticatesNullValues` 写死为 `false`。

**重要**：clang 二进制里**已有** `ptrAuthAuthenticatesNullValues` 和
`DW_AT_LLVM_ptrauth_authenticates_null_values`——底层支持，只差语法解析层。

### 5.4 已写好的 clang 补丁

`.work/llvm-ptrauth-null-values.patch`（2896 字节，2 hunk）
已复制到 `.work/UniXNU/scripts/linux/llvm-ptrauth-null-values.patch`，
并已接入 `setup-toolchain.py:220`：

```python
run(['git', '-C', src, 'apply', HERE / 'llvm-xnu-builtin-type.patch'])
run(['git', '-C', src, 'apply', HERE / 'llvm-ptrauth-null-values.patch'])
```

**补丁改三处**（`clang/lib/Sema/SemaType.cpp` 的 `HandlePtrAuthQualifier`）：
1. 断言从 `<= 3` 放宽到 `<= 4`
2. 解析第 4 个参数（只认 `"authenticates-null-values"`，其他值报错）
3. 把 `AuthenticatesNullValues` 传给已有的 `PointerAuthQualifier::Create`

**已验证**：`patch -p1 --dry-run` 对 `LLVM_COMMIT 58c46bae21181fb4240cf964461bea738d96d50e` 通过。

**注意**：补丁必须基于该固定 commit 生成，**不能用 `llvm-project/main`**——两者行号
不同（main 的 `HandlePtrAuthQualifier` 在 8727 行，固定 commit 在 8659 行），
用 main 版会应用失败。

### 5.5 clang 重建：当前脚本与三个坑

**当前脚本**：`.work/rebuild-clang5.sh`（tarball 获取 + 打补丁 + cmake + ninja + 自动
验证 `__ptrauth`）。日志写 `.work/toolchain-rebuild.log`（**持久**，不要写 `/tmp`）。

**为什么不用 `setup-toolchain.py` 直接跑**：它在 ninja 结束后执行
`shutil.rmtree(src)`，构建一旦被中断，检出就没了，下次只能从零开始（每次约
40-60 分钟）。现在的做法是源码树固定在 `.work/llvm-project`，直接调 ninja，
中断只损失未编译的 .o，检出永远保留。

**三个坑**（都实际踩过）：

1. **`/tmp` 会被 WSL 重启清空**。日志必须写 `.work/`。同理 `/tmp/pt.c` 这类
   验证文件也要放 `.work/`。

2. **不要用 `--filter=blob:none` 的 partial clone**。`git clone --filter=blob:none
   --no-checkout` + `checkout` 会在 checkout 阶段按需逐个下载 blob，每个 blob
   一次网络往返，10 分钟才拉了 900M，而且极易触发
   `GnuTLS recv error (-9)` / `early EOF`。
   **正确做法是 tarball**：
   ```bash
   curl -L --http1.1 --retry 5 --connect-timeout 30 --max-time 1800 \
        -o llvm.tar.gz \
        "https://github.com/llvm/llvm-project/archive/58c46bae21181fb4240cf964461bea738d96d50e.tar.gz"
   tar xzf llvm.tar.gz -C .work/llvm-project --strip-components=1
   ```
   一次请求拿回整棵树。tarball 没有 `.git`，所以补丁要用 `patch -p1 -d <dir>`，
   不能用 `git apply`。

3. **`bash script.sh | tail -N` 的退出码取自 `tail`，不是脚本**。用它判断构建
   成功会得到假的 exit 0。正确做法：脚本自己把退出码写进日志，或
   `set -o pipefail`。

**重建完成后要做的**：
1. 脚本会自动验证四参数 `__ptrauth`：
   ```bash
   .work/UniXNU/.linux-toolchain/llvm/bin/clang \
     -target arm64e-apple-macos26.0 -c .work/llvm-project/ptrauth-check.c \
     -o .work/llvm-project/ptrauth-check.o
   ```
   期望 `ptrauth compile exit=0`。
2. 重跑 `python3 tools/ci_kernel.py native`，看是否越过 `firehose_buffer.c`
3. 若成功，本地编译链路打通，此后改补丁只需本地几秒

**回滚**：`.work/UniXNU/.linux-toolchain/llvm.prev` 是旧 clang（无 `__ptrauth`
补丁）。新 clang 若有问题，`mv llvm.prev llvm` 即可换回。

---

## 6. 环境与工具备忘

### 6.1 云端构建

- Workflow：`.github/workflows/macos-kernel.yml`，`stage=ufs-integrated`
- 凭据：`D:\unixnu-tb710fu-plan\.env`（`GITHUB_TOKEN`/`GH_TOKEN`），只经环境变量或 HTTP 头传递，**不得提交**
- 推送：`python3 tools/github_admin.py push --branch codex/ufs-transactional-handoff`
- 派发：`POST /repos/limingerf/UniXNU-tb710fu/actions/workflows/macos-kernel.yml/dispatches`，body `{"ref":"codex/ufs-transactional-handoff","inputs":{"stage":"ufs-integrated"}}`
- 轮询：`GET /actions/runs/{id}`
- 典型耗时 15-25 分钟

### 6.2 下载产物（用这个脚本，别用 Python 那个）

`.work/fetch-<run>.sh`：`curl -L -C -` 断点续传。之前 Python 下载器在 27MB
处断掉且重试会卡死；curl 脚本一次通过。

### 6.3 签名与打包

- AVB 工具：`/mnt/d/avbtool/tools/avbtool.py`，密钥 `pem/testkey_rsa4096.pem`（RSA-4096）
- 基线三件套：`out/tb710fu-dual-live-wsl-44124706860-signed/`
- 打包器脚本命名规律：`.work/package-<name>-<run>.py`（从上一个派生）
- **manifest 文件名必须与上一轮一致**（如 `-rsp-inputs.json`），否则
  `prior_manifest` 读不到而失败
- 独立复核脚本：`.work/recheck-<run>.py`

### 6.4 WSL 环境的坑（血泪）

1. **PowerShell 会破坏引号**：`grep -E 'a|b'`、heredoc、`sed` 里的引号都可能被
   PowerShell 解析掉。**所有多行 shell 一律写成 `.work/*.sh` 文件再执行。**
2. **`grep` 管道用 `|` 在 PowerShell 里会被当管道符**：改用脚本文件 + `grep -F`
3. **`/tmp` 会被清空**：重要文件放 `.work/` 下
4. **`head` 会掩盖退出码**：判断编译成功要用 `$?` 或 `PIPESTATUS`，不能
   `... | head` 之后取 `$?`（本轮曾因此误报 PASS）
5. `Set-Content -Encoding` 在某些 PowerShell 版本不存在参数

### 6.5 目录速查

```
patches/ufs/         0001..0006 补丁
scripts/build-tb710fu-ufs-adapter.py   PATCHES 元组（注册新补丁的地方）
tools/check_ufs_bringup_trace.py     等价性验证器
tools/ci_kernel.py                   构建入口（configure/native/prelink/collect）
.work/UniXNU/                         UniXNU 源码树（含 unixnu 驱动）
.work/UniXNU/.linux-toolchain/        本地 Linux 交叉工具链
.work/ufs-bringup-staging2/           0004 后的 NativeUfs.cpp 暂存副本
.work/ufs-0002-baseline/              post-0002 基线
out/                                  签名包
docs/evidence/                        证据 JSON
CURRENT_STATUS.md                     权威状态（每次里程碑都要更新）
```

---

## 7. 硬性纪律

1. **补丁职责分离**：追踪进 `0004`，行为修复另起补丁
2. **验证器是独立证据**：不要为了让验证通过而放宽检查，除非改动本身是有意的
   行为变更——那要在 commit message 里写明实机证据
3. **不要伪造状态**：`hardware_boot_verified=false`、`runtime_qualified=false`、
   `device_operations=false`，直到有实机证据
4. **每次里程碑更新 `CURRENT_STATUS.md`**，并记录 run id / commit / 全部哈希
5. **不确定就先查证**，不要猜。我曾因猜测浪费多轮：怪槽位、读错 APFS 版本、
   猜虚表、误判 `IOLog` 与 `printf`、误判 "不支持 arm64e"
6. **探针要能真正显示**：卡死时 worker 停摆，纯 `printf` 写进环形缓冲区也不会
   上屏，**必须用 checkpoint 强制刷屏**（页脚文字才是可靠信号）
7. `LOG` 与 `QR` 的差值反映滞留在 ring 里未上屏的字节数，可用来判断"是否只是
   没刷新"

---

## 8. 当前未完成清单

| 项 | 状态 |
|---|---|
| 验证 `0006` 并派发构建 | **待做（最高优先级）** |
| 验证器适配 `0006` 的 `IOSleep` 变化 | 待做 |
| clang 重建 | **运行中** |
| 用新 clang 验证四参数 `__ptrauth` | 待做 |
| 本地 `ci_kernel.py native` 越过 `firehose_buffer.c` | 待做 |
| `0006` 的实机验证 | 待做（预期：能走过 `lun-ready`，进入根布局） |
| CDC 枚举（Windows 侧） | 长期目标，需内核能继续启动后才有意义 |

---

## 9. 已归档记忆节点

- `mem_1795198372497_c3dbdd` — 双编译单元下的编译期开关方案
- `mem_1795203152775_cf1700` — WSL/Linux 编译 arm64e 内核（含实测证据）
- `mem_1795189037970_518e30` — 用同步 checkpoint 定位内核停点
