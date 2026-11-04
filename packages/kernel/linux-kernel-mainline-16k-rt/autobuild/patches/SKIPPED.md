# 未应用 / 需替换的 AOSC 补丁

在 7.3.1 上应用 AOSC 的 450 个补丁时，以下补丁**不能**直接使用：

## 0356-RUYI-drm-amdgpu-disable-dynamic-PCIe-speed-switch-on.patch

- 该补丁针对旧版树编写，7.3.1 里 `amdgpu_device_pcie_dynamic_switching_supported()`
  在非 x86 是 `#else` 的 stub；`patch --fuzz` 会把新增声明插到 stub 的 `{` **之前**，
  产生 “标量初始化带花括号 / 旧式参数声明” 等一连串编译错误。
- 该补丁只为 **SpacemiT K3（RISC-V）** 关闭 PCIe 动态速率切换，与 LoongArch 无关。
- 处理：**跳过该补丁**（或应用后把 `#else` 分支恢复成上游的 `{ return true; }`）。
- 复现命令：`./scripts/fetch-aosc-patches.sh <tree>` 已内置跳过列表。

## 40 个冲突补丁

RISC-V/SpacemiT、x86、Surface、Rust 工具链等非 LoongArch 补丁，`patch` 直接 reject，
不影响本平台（详见 `docs/KERNEL.md` 的应用统计：applied=355 / already=55 / conflict=40）。

## 0427-AOSCOS-mei-me-use-PCI_DEVICE_DATA-macro-for-linux-su.patch（部分应用）

该补丁要把 linux-surface 添加的 MEI 设备项从 `MEI_PCI_DEVICE(...)` 改成
`PCI_DEVICE_DATA(INTEL, ...)`，并同步改 `hw-me-regs.h` 的宏名。
在 7.3.1 上**删除成功、新增与改名未完全应用**，结果 `pci-me.c` 里残留三行
`MEI_PCI_DEVICE(MEI_DEV_ID_ICP_LP...)`，而 `MEI_PCI_DEVICE`/`MEI_DEV_ID_ICP_*`
都已不存在 → `implicit declaration of function 'MEI_PCI_DEVICE'`。

处理：删除这三行残留（Intel MEI 与 LoongArch 无关，iTouch 设备 ID 也不需要）。
如需保留，可同时把 `PCI_DEVICE_ID_INTEL_MEI_ICP_LP_3` 定义补回 `hw-me-regs.h`。

## 0344-SURFACE-hid-multitouch-Prevent-mode-set-on-Surface-L.patch（错位应用）

补丁要在 `mt_on_hid_hw_open()` 里加 `td` 声明和 quirk 判断，但 hunk 的上下文与 7.3.1 不同，
`patch --fuzz` 把整段插进了 `mt_probe()` 中一个 `if (...)` 条件的中间，导致：

```
hid-multitouch.c:2310: error: expected ')' before 'struct'
hid-multitouch.c:2459: error: 'td' undeclared
```

处理：把插入块从 `if` 表达式里摘出、恢复条件为上游写法；并给 `mt_on_hid_hw_open()`
补上 `struct mt_device *td = hid_get_drvdata(hdev);` 与同样的 quirk 判断。

> 这是第三个“hunk 错位”案例（前两个：0356 amdgpu、0427 mei）。
> 规律：**带长上下文的 Surface/RISC-V 补丁在 7.3.1 上容易错位**，出错时优先检查
> 这些补丁对应文件的结构是否被插坏。
