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
./scripts/fetch-aosc-patches.sh /path/to/linux-7.3.1
# 等价于：
#   git clone --depth 1 --filter=blob:none --sparse -b stable AOSC-Dev/aosc-os-abbs
#   git sparse-checkout set runtime-kernel/linux-kernel
#   对每个 patch 依次 patch -p1 -N --fuzz=5
```

本仓库另外收录了两个让 clang full-LTO 能编过 AOSC 补丁代码的修复
（`packages/kernel/.../autobuild/patches/`）：

| 补丁 | 原因 |
|---|---|
| `0001-loongarch-legacy-boot-strict-prototypes.patch` | AOSC 的 `acpi_arch_init (){` 缺 prototype，clang `-Werror=strict-prototypes` |
| `0002-loongarch-ow-syscall-no-lto-for-feat-test.patch` | `feat_test.o` 要被 `$(OBJDUMP)` 读，full-LTO 下是 bitcode，需 `-fno-lto` |
| `0004-mt7921-txpwr-heap-allocation.patch` | clang full-LTO 链接时 `mt7921_txpwr()` 栈帧 4016 B 超过 `-Wframe-larger-than=2048`，改为 kzalloc |
| `0003-gpio-xilinx-init-state-for-O3.patch` | `-O3` 下 gcc 发现 `xgpio_get()` 用未初始化的 bitmap 做读改写（`-Werror=uninitialized`），补 `bitmap_zero()` |

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

## 实际落地的 LoongArch 相关补丁（48 个）

在 7.3.1 上最终成功应用、且与 LoongArch/龙芯平台直接相关的补丁：

  - `0004-UPSTREAM-MIPS-Loongson64-dts-Add-node-for-LS7A-PCH-L`
  - `0021-FROMLIST-LoongArch-Update-the-flush-cache-policy`
  - `0026-FROMLIST-PCI-Prevent-LS7A-Bus-Master-clearing-on-kex`
  - `0033-FROMLIST-RFC-drm-amdkfd-disable-HSA_AMD_SVM-on-Loong`
  - `0065-BACKPORT-FROMLIST-LoongArch-detect-and-disable-sc.q-`
  - `0182-FROMLIST-pci-loongson-Avoid-L0s-on-LS7A1000-PCIe-x8-`
  - `0184-FROMLIST-can-Add-Loongson-CAN-FD-controller-driver`
  - `0185-FROMLIST-can-loongson_canfd-Add-RXDMA-support`
  - `0186-FROMLIST-gpio-loongson-64bit-Add-back-the-support-fo`
  - `0189-FROMLIST-cpufreq-loongson3-Make-this-driver-depend-o`
  - `0190-FROMLIST-cpufreq-loongson3-Adjust-the-width-of-id-an`
  - `0191-FROMLIST-cpufreq-loongson3-Replace-per-package-mutex`
  - `0192-FROMLIST-cpufreq-loongson3-Use-global-physical-CPU-I`
  - `0193-FROMLIST-cpufreq-loongson3-Replace-IOCSR-read-write-`
  - `0202-FROMLIST-MIPS-Loongson-Add-board_ebase_setup-support`
  - `0203-FROMLIST-MIPS-Loongson64-Enlarge-cross-package-node-`
  - `0206-LOONGSON-LoongArch-Add-CPU-HWMon-platform-driver`
  - `0207-LOONGSON-drivers-firmware-Move-sysfb_init-from-devic`
  - `0208-LOONGSON-drm-radeon-Workaround-radeon-driver-bug-for`
  - `0264-DEBIAN-Use-RELAXED-ieee754-mode-for-Loongson-3-as-3A`
  - `0297-BACKPORT-DEEPIN-pci-quirks-LS7A2000-Fix-pm-transitio`
  - `0367-AOSCOS-loongarch-re-introduce-add_numamem_region-ini`
  - `0368-AOSCOS-loongarch-basic-boot-support-for-legacy-firmw`
  - `0369-AOSCOS-loongarch-parse-BPI-data-and-add-memory-mappi`
  - `0370-AOSCOS-loongarch-add-MADT-ACPI-table-conversion`
  - `0371-AOSCOS-loongarch-correct-missing-offset-of-PCI-root-`
  - `0372-AOSCOS-loongarch-fix-missing-dependency-info-in-DSDT`
  - `0373-AOSCOS-loongarch-fix-DMA-address-offset`
  - `0374-AOSCOS-loongarch-fix-HT_RX_INT_TRANS-register`
  - `0375-AOSCOS-arch-loongarch-add-la_ow_syscall-as-in-tree-m`
  - `0378-AOSCOS-MIPS-Temporarily-disable-Loongson3-LL-SC-erra`
  - `0384-AOSCOS-MIPS-loongson64-deselect-ARCH_SUPPORTS_HUGETL`
  - `0395-AOSCOS-MIPS-Loongson-Add-constant-timer-support`
  - `0396-AOSCOS-MIPS-loongson64-fix-constant-timer-build-on-k`
  - `0397-AOSCOS-MIPS-loongson64-use-generic-vDSO-clock-mode-s`
  - `0398-AOSCOS-MIPS-Loongson-Add-PMON-read-write-in-OS-suppo`
  - `0400-AOSCOS-MIPS-math-emu-replace-CPU_LOONGSON3-condition`
  - `0402-AOSCOS-memset-optimization-for-loongson-3`
  - `0403-AOSCOS-MIPS-loongson3-memset-replace-asm-export.h-wi`
  - `0404-AOSCOS-MIPS-loongson3-memset-use-PTR_WD-to-fix-build`
  - `0406-AOSCOS-memcpy-optimization-for-loongson-3`
  - `0407-AOSCOS-MIPS-loongson3-memcpy-use-PTR_WD-to-fix-build`
  - `0408-AOSCOS-MIPS-loongson3-memcpy-replace-asm-export.h-wi`
  - `0409-AOSCOS-MIPS-mark-CPU_LOONGSON64-as-HAVE_PLAT_MEMCPY`
  - `0410-AOSCOS-MIPS-loongson3-memcpy-adapt-to-RAW_COPY_USER`
  - `0411-AOSCOS-spi-spi-loongson-allow-building-on-MACH_LOONG`
  - `0419-AOSCOS-MIPS-Loongson-Salvage-the-full-count-from-the`
  - `0441-AOSCOS-mmc-loongson2-add-back-the-so-called-ABI-1.0-`

其余 307 个成功应用的补丁为通用修复（网络 PHY、PCI、USB、drm 等），
55 个已被上游合并，40 个冲突补丁均与 LoongArch 无关（RISC-V/SpacemiT、x86、
Surface、Rust 工具链）。

## 外部模块（DKMS）前置条件

内核 headers 包必须包含 **`tools/objtool/objtool`**，否则任何 DKMS 模块构建都会失败：

```
/bin/sh: /usr/src/linux-headers-<rel>/tools/objtool/objtool: No such file or directory
```

`package-kernel-v2.sh` / `package-kernel-v3.sh` 的 headers 段已经改为
`include scripts tools/build tools/objtool tools/bpf`，把该二进制一起打包。

`loonggpu` 的 tracepoint 告警（`unsafe pointer field 'ring_name'`）修复见
`packages/graphics-drivers/loonggpu-dkms/`。

## 变体共存

`gcc-o3` 与 `clang-lto` 的包名、`/boot` 文件名、`/lib/modules/<release>` 目录、
`/usr/src/linux-headers-<release>` 路径都带变体后缀，因此**两个内核可以同时安装**，
在 GRUB 里选择启动即可做 A/B 对比；打包脚本不再声明 `Conflicts`。
