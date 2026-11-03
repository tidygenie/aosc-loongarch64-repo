# LG110 的 Wayland 兼容层

厂商栈（`loonggl` / `gsgpu`）能跑 OpenGL 2.1，但**不能**直接支撑 KWin：

1. 没有 `wl_drm` 全局（KWin 6.x 已移除）；
2. EGL 只暴露 pbuffer 配置，Qt 要 window 配置；
3. 第二个 `open("/dev/dri/card0")` 会拿到 master fd，DRM lease 失败，Xwayland 崩溃；
4. 扫描输出（scanout）在 16K 页下有 stride/tiling 问题。

`plasma-loonggpu-compat` 用三个 preload shim 解决：

| shim | 注入进程 | 作用 |
|---|---|---|
| `kwin-drm-fd-reuse.so` | kwin_wayland | 复用 logind 授权的 card0 fd；对 lease 场景改用真实 `openat()` 取非 master fd；发布 legacy `wl_drm` 全局 |
| `kwin-dmabuf-bridge.so` | kwin_wayland | 拦截 `Display::bufferForResource`，把 `wl_drm` prime buffer 包装成 KWin `GraphicsBuffer`，走正常 EGL 导入 |
| `gsgpu-egl-window-compat.so` | plasmashell | 把 `EGL_SURFACE_TYPE` 的 WINDOW 请求改写成 PBUFFER，让 Qt 拿到可用配置 |

配套 systemd user drop-in 设置 `LD_PRELOAD`、`GBM_BACKEND=gsgpu`、
`__EGL_VENDOR_LIBRARY_FILENAMES`，并让 Plasmashell 在 KWin 就绪后启动。

内核侧由 `loonggpu` DKMS 提供扫描输出修复（见 `packages/kernel/.../autobuild/patches`）。

## 验证

```bash
qdbus org.kde.KWin /KWin supportInformation | grep -E "Compositing Type|OpenGL renderer"
# -> Compositing Type: OpenGL
# -> OpenGL renderer string: LoongGPU(TM) LG110 (...)
glxgears            # Xwayland 正常，~55 FPS
```
