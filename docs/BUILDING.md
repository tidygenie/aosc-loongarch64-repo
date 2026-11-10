# 构建环境与流程

## 依赖

```bash
sudo apt install gcc g++ make cmake meson ninja pkg-config git curl \
                 pkg-config libdrm wayland mesa vulkan-headers \
                 go nodejs yarn python-3
```

- `vkgl-loonggpu` 需要 **SPIRV-Cross**（默认从 `/usr` 找静态库；也可设置 `SPIRV_CROSS_DIR`）：

```bash
git clone --depth 1 https://github.com/KhronosGroup/SPIRV-Cross.git
cmake -S SPIRV-Cross -B SPIRV-Cross/build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF
cmake --build SPIRV-Cross/build -j4
```

## 单个包

```bash
./scripts/build-package.sh packages/graphics-drivers/vkgl-loonggpu
```

脚本会：

1. 读取 `spec` 的 `VER`；
2. 在临时目录设置 `SRCDIR`（源码）与 `PKGDIR`（staging 根）；
3. 执行 `autobuild/build`；
4. 用 `autobuild/defines` 的元数据生成 `control` 并 `dpkg-deb --build`；
5. 产物放在 `build/<pkgname>_<ver>-<rel>_loongarch64.deb`。

## 手工构建（abbs 兼容）

在 AOSC abbs 环境（`acbs-build` 或 `abbs`）中，把本仓库的 `packages/` 目录作为配方树即可。
配方中的 `ABTYPE=self` 表示完全由 `autobuild/build` 负责构建与暂存。

## 注意事项

- 架构字段必须是 `loongarch64`（不是 `loong64`），否则 apt 不认。
- 依赖写 AOSC 包名（`glibc` 而非 `libc6`），在 `PKGDEP` 中以空格分隔；轻量构建器生成 Debian `Depends` 时统一转换成逗号分隔，不能直接写空格。
- `plasma-loonggpu-compat` 的 shim 用 `LD_PRELOAD` 注入，编译时链接 `libdrm`/`libEGL`。
- 构建内核见 [KERNEL.md](KERNEL.md)，它不走 `scripts/build-package.sh` 的通用流程。

## 已知坑：命令行 `CFLAGS=` 会覆盖 Makefile 自带的 define

djb 风格和老式 Makefile 常把 `-DLIBDIR=... -DDEFAULT_DUPLEX=...` 这类宏写在自己的
`CFLAGS` 里。如果 formula 用

```ruby
system "make", "CFLAGS=-Wno-error=implicit-int ..."
```

显式传入 CFLAGS，make 会**忽略 Makefile 里的 CFLAGS**，这些 define 全部丢失，表现为
`'LIBDIR' undeclared` / `You must compile with -D_REENTRANT` / `bcd.h: No such file`。

正确做法（见 `packages/` 里 `fq` 的处理）：

1. 优先把标志追加到 Makefile 自己的 `CFLAGS` 行（保留原有 define）；
2. 如果必须传命令行 CFLAGS，就把 Makefile 需要的所有 `-D` 和 `-I` 一起写进同一条命令行；
3. include 路径用 `Formula["<dep>"].opt_include`（keg-only 依赖不会自动进 CPATH）。

受影响的包：`fq`、`mpage`、`libowfat`、`tundra` 等。
