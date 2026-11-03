# 主线 16K RT 内核

## 目标

| 变体 | 编译器 | 优化 | 说明 |
|---|---|---|---|
| `gcc-o3` | gcc 16.2 | `KCFLAGS=-O3` | 完整 ftrace/调试能力 |
| `clang-lto` | clang 22.1 | `CONFIG_LTO_CLANG_FULL` | 关闭 function tracer（见下） |

两者共同点：

- 上游 7.3.1 源码；
- AOSC OS `runtime-kernel/linux-kernel` 的全部补丁（450 个）；
- AOSC 的 `autobuild/loongarch64/config.16k` 作为配置基线；
- `CONFIG_PAGE_SIZE_16KB=y`、`CONFIG_PREEMPT_RT=y`；
- 内核 release 名分别为 `7.3.1-mainline-16k-rt-gcc-o3` / `7.3.1-mainline-16k-rt-clang-lto`，
  可以共存，DKMS 会为各自构建 `loonggpu`。

## 补丁应用结果（7.3.1）

```
applied=355  already-upstream=55  conflict=40
```

40 个冲突全部是非 LoongArch 补丁（RISC-V/SpacemiT、x86、Surface、Rust 工具链等），
LoongArch 相关的 `AOSCOS-loongarch-*` 系列（0369–0374）全部应用成功；
`i2c-ls2x-v2`（2K0300）在上游已存在。

复现：

```bash
git clone --depth 1 --filter=blob:none --sparse -b stable \
  https://github.com/AOSC-Dev/aosc-os-abbs.git
cd aosc-os-abbs && git sparse-checkout set runtime-kernel/linux-kernel
# 对每个 patch 依次 patch -p1 -N --fuzz=5
```

## 配置

```bash
cp aosc-os-abbs/runtime-kernel/linux-kernel/autobuild/loongarch64/config.16k .config
scripts/config --enable PREEMPT_RT --disable PREEMPT_NONE --disable PREEMPT_VOLUNTARY
scripts/config --set-str LOCALVERSION "-mainline-16k-rt-gcc-o3"
make olddefconfig
```

clang 变体还需要（必须先用 `LLVM=1` 跑一次 olddefconfig，让 `CC_IS_CLANG` 生效）：

```bash
make LLVM=1 olddefconfig
scripts/config --enable LTO --enable LTO_CLANG --enable LTO_CLANG_FULL
scripts/config --set-str LOCALVERSION "-mainline-16k-rt-clang-lto"
make LLVM=1 olddefconfig
```

### 为什么关掉 function tracer

内核 Kconfig 中：

```
config HAS_LTO_CLANG
	depends on !FTRACE_MCOUNT_USE_RECORDMCOUNT
```

LoongArch 上 clang **不支持** `-mrecord-mcount`（实测报错），所以只能走 recordmcount，
而 recordmcount 与 LTO 对象不兼容。要拿到 full LTO，只能关闭 `FUNCTION_TRACER`
（`DYNAMIC_FTRACE` 随之消失）。这是有意的取舍：`clang-lto` 变体没有 ftrace，
需要 ftrace 时请用 `gcc-o3` 变体。

## 构建

```bash
# gcc
make -j1 KCFLAGS="-O3" all
# clang
make -j1 LLVM=1 all
```

`-j1` 是刻意的（机器资源有限，且要保证日志可复现）。

## 打包与安装

`files/package-kernel-v2.sh <gcc-o3|clang-lto>` 会：

1. `make modules_install` 到 staging；
2. 拷贝 `vmlinuz`、`System.map`、`.config`；
3. 生成 `postinst`（`depmod` → `dkms autoinstall` → `dracut`）、`prerm`、`postrm`；
4. 输出 kernel deb 与 headers deb。

```bash
sudo dpkg -i linux-kernel-*_loongarch64.deb linux-headers-*_loongarch64.deb
```

`postinst` 会自动为当前内核构建 DKMS 的 `loonggpu` 显卡驱动；首次安装后建议
`dracut -f` 再重启。

## 启动验证

- 一次性启动项：`grub-reboot <entry>`，失败后自动回退 `saved_entry`；
- 看 `/proc/version` 区分编译器（`gcc version` vs `clang version`）；
- 看 `uname -r` 区分变体；
- 图形验证：`qdbus org.kde.KWin /KWin supportInformation | grep renderer` 应显示 LG110。
