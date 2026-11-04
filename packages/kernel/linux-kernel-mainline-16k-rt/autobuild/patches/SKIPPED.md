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
