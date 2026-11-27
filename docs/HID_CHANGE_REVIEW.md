# HID 触摸/按键改动审查 · 2026-11-28

**结论：改动是 fail-safe 的，不会崩溃，也不会写坏内存。**
**未验证的只有功能本身（触摸/按键是否真的工作）。**

---

## 一、被审查的改动

`2d30066` + `0b7abfe` + `199af3f`，在 `package/Tb710fuInput/Tb710fuInputProbe.cpp`：

1. 不再用 `OSMetaClass::allocClassWithName("IOHIDDevice")`（抽象类，必返回 NULL）
2. 改为：注册运行时 `OSMetaClass` 子类 → `OSObject::operator new` 分配
   `sizeof(Tb710fuHidObj)` → 用 `0xfffffff00ab353f0`（`__ZN11IOHIDDeviceC2EPK11OSMetaClass`）
   调真实 C2 → `adopt_vtable` → `init()`
3. HID 构造函数全部移到 worker 线程（`input_worker`），不再阻塞启动路径

## 二、审查发现

### 2.1 `0xD0` 硬编码有运行时保护（不是风险）

`TB710FU_HID_INSTANCE_BYTES = 0xD0` 是硬编码，生成器没有从内核推导。
如果它是错的，C2 会写越界——**但代码有两道独立闸门**：

```
Tb710fuInputProbe.cpp:359  if (super->getClassSize() != kIohidDeviceInstanceBytes)
Tb710fuInputProbe.cpp:504  if (meta->getClassSize()  != TB710FU_HID_INSTANCE_BYTES)
```

两处都打印 `hid: IOHIDDevice extent 0x%x, expected 0x%x` 后 `return nullptr`，
**在任何对象分配之前**。所以 0xD0 错了只会导致触摸不工作，不会 panic、
不会内存损坏。

### 2.2 为什么无法静态验证 0xD0

读 `.local/prelink-kernel.macho` 里 `0xfffffff00cc24958`（`gMetaClass`）
的头 0x80 字节，**全是 0**：`__DATA` 段的 C++ 静态对象由 `__mod_init_func`
在启动时构造，预链接文件里没有填充。

工具：`scratch/read-hid-metaclass.py`

### 2.3 剩余的真实风险（低）

- `adopt_vtable` 逐条验证每个 vtable slot 都能 `ptrauth_strip` 到内核文本，
  不满足就拒绝。这层保护是好的。
- `hid_new_report_descriptor` 通过 `self == gTouchDevice / gKeysDevice`
  区分两个设备；若两者都为 NULL 会返回 `kIOReturnNotAttached`，安全。

## 三、功能上仍未验证的

硬件一恢复，唯一要看的日志是：

```
TB710FU input: hid: runtime metaclass=0x... super=0x... size=0xd0 postModLoad=0
TB710FU input: TB710FU Keys started        <- 或 TB710FU Touchscreen started
```

- 有 `runtime metaclass=` 且 `postModLoad=0` → 子类注册成功
- 紧接着 `... started` → 设备真的起来了
- 只有 `extent 0x...` → 0xD0 猜错了，改这个常量即可
- 连 `runtime metaclass=` 都没有 → 卡在 `ensure_hid_family()` 或更早

## 四、不要做的事

- 不要在设备恢复前再改 HID 代码（当前已是 fail-safe 形态）
- 不要为了"验证 0xD0"去反汇编 C2 推算对象大小——运行时会自己告诉我们
