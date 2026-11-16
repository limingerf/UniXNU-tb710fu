# ADT 身份修复交付 · 2026-11-16

承接 [绿条审计](GREENBAR_AUDIT.md) 的发现：`out/kernel-pr249-final` 里的
`kernel.macho.adt` 不是 macOS 设备树。本文记录修复件，以及**它能证明什么、
不能证明什么**。

## 交付

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `out/kernel-adt-fix/vendor_boot.img` | 100,663,296 | `eb09e141fcb7065793554f772a0aefe18e76449ecad20d3ba314ac3aa129f901` |
| `out/kernel-adt-fix/vbmeta.img` | 8,000 | `a7472fe5d7995f1694ddca850e7d5eae71945df589c6f44d4bdda334b5b43c1e` |

沿用用户已有的仅本地签名授权。未重新 native/prelink、未云构建、未操作设备。

## 唯一改动：payload 里的 ADT

payload 逐文件复核：

| 文件 | 前 | 后 | 结论 |
| --- | --- | --- | --- |
| `Payloads/kernel.macho` | `861dbf1c…` | `861dbf1c…` | **未变**，解回内核仍为 `9daf5064…` |
| `Payloads/kernel.macho.adt` | `4c56fb2a…`(11,536 B) | `f4161ff9…`(14,212 B) | 替换为目标修复 |
| `unixnu.toml` | `a14b5a8f…` | `a14b5a8f…` | **未变** |

FAT 几何不变（66,512 簇，仍 ≥ 65,525）。vendor_boot 的 header 与
ramdisk 之外字节逐字节不变。

### ADT 前后对比

| 字段 | 修复前 | 修复后 |
| --- | --- | --- |
| `model` | `Qualcomm Technologies, Inc. Pineapple MTP` | `Mac17,11` |
| `compatible[0]` | `qcom,pineapple-mtp` | `K114cAP` |
| `/product` 属性数 | 0 | 35 |
| `/chosen` `osenvironment` | 缺失 | `main` |
| `/chosen` `boot-uuid` | 缺失 | `84581E99-…` |
| `/chosen` `root-matching` | 缺失 | 有 |

生成方式与审计时一致：`board.toml`（已补 `[payload.adt]`）+ macOS 27
profile + `.local/adt/adt-input4.dtb`，走固定生成器
`0c1bac3d…:scripts/unixnu/devicetree.py`，`--unique-chip-id 0x30c18713`。
`tools/check_adt_identity.py` 对修后 ADT 9 项全 PASS，对修前 7 项 FAIL。

## 验证：不依赖 avbtool 的自述

三层，全部来自实际字节：

1. **签名脚本自报**：`signature_checks=PASS`、22 个其它描述符不变、公钥不变、
   header 不变、描述符分布仍为 13 property / 4 hashtree / 3 hash / 3 chain。
2. **独立 payload 复核**（`.work/audit/verify_signed_pair.py`）：解出 LZMA、
   比对内核与 toml 哈希、确认 ADT 已替换、确认 footer 布局与零填充。
3. **独立 RSA 验签**（`.work/audit/verify_rsa.py`）：只用镜像内嵌模数自行
   算 `s^e mod n`，检查 PKCS#1 v1.5 填充与摘要，**不调用 avbtool**。
   关键点是 digest 覆盖 **header + aux 块**（跳过整个 auth 块），这是
   avbtool `sign_vbmeta_blob` 的真实行为；两处（vbmeta 文件与 vendor_boot
   footer 内嵌）均 PASS。

**反向对照**（`.work/audit/test_rsa_negative.py`）：翻转描述符、公钥、
签名、header 各一字节，4 例全部被拒。一个永远说 PASS 的验证器是没用的，
所以这一步必须有。

## 边界：这条修复**没有**证明能过绿条

必须说清楚，否则会重演“上次报告说修好了”的问题：

- `PE_init_platform(FALSE)` 在 `arm_init` **第 380 行**执行，读取 ADT；
  而设备当前停在 `arm_vm_init`（**第 586 行**）内部。也就是说，
  **内核早就读过这份 ADT，并且继续往下跑了**。
- 所以：ADT 缺身份是**客观缺陷**（内核匹配不到 macOS 平台专家，后续
  `/arm-io`、控制台、根卷都无从谈起），但**它不能被单独认定为绿条的成因**。
- 真接要分开验收：ADT 身份修复是一件事，`arm_vm_init` 内的停点是另一件事。
  后者目前仍无证据归因（PR249 已定量排除）。

进一步说，`pe_identify_machine()`（`PE_init_platform` 内调用）要求
`/cpus` 的 `timebase-frequency`，本 ADT 有（19.2 MHz），所以它不会在这里失败。

## 已知未修：`/arm-io` 仍为空

生成器只导出带 `unixnu,adt-name` 的节点，本板 DTS 没有。这是独立的
**板级移植任务**（从本机 live FDT 派生带 ADT 名称的硬件节点），
审计与门禁都把它标为 `INFO` 而非 PASS，不用“通过”掩盖缺口。

## 刷写（未执行）

```powershell
$fb = 'C:\Program Files (x86)\platform-tools\fastboot.exe'
& $fb flash vendor_boot_b 'D:\unixnu-tb710fu-clean\out\kernel-adt-fix\vendor_boot.img'
& $fb flash vbmeta_b      'D:\unixnu-tb710fu-clean\out\kernel-adt-fix\vbmeta.img'
```

保留现有 `boot_b`（不动）。刷后选原 XNU 项。

## 预期与判读

- 若停点**推进**（出现更多内核输出、或进入平台/控制台初始化），说明
  ADT 身份确实卡住了后续，这条修复有效。
- 若**仍停在原处**，说明 ADT 身份是并列缺陷而非当前停点成因；下一步应带
  核内报告器定位 `arm_vm_init` 内部，而不是回到 PR249 类容量改动。
  注意 `0004` 补丁的 `tb710fu_ramoops_init()` 在 `arm_vm_init` **返回之后**，
  本阶段 pstore 一定为空，不能据此判断。

机器记录：[kernel-adt-fix-build.json](evidence/kernel-adt-fix-build.json)。
