# LG110 上的 Vulkan：为什么是 vkgl

## 结论先行

- **RADV 不可能工作**：LG110 的 CP 不认 AMD PM4。
- **vkgl**（本仓库）把 Vulkan 1.0 子集翻译到厂商 GL 2.1，`vkcube` 已经能跑。

## 证据

1. 用户态直接提交 Loongson `GSPKT_WRITE`（`GSPKT(0x81,3)|WRITE_DST_SEL(1)|WRITE_WAIT`）：
   GPU 真的把 `0xDEADBEEF` 写进了目标显存，fence 正常返回。
2. RADV 的 `vkCmdClearColorImage`/`vkCmdFillBuffer`/`vkCmdCopyBuffer`：
   提交成功、`vkQueueWaitIdle` 成功、**读回全 0**。
3. 内核 tracepoint `loonggpu_cs` 显示两种提交都进了 GFX ring（`ring=0`），
   IB 长度分别为 24/416 dwords；厂商 GL 用同一条 ring 渲染正常。
4. 内核源码 `include/loonggpu.h`：LG100 的 ring 用 `GSPKT`，LG200/210 才用 `LG2SCMD32`。

## vkgl 的实现要点

| Vulkan 概念 | 实现 |
|---|---|
| Instance/Device/Queue | 单设备，EGL/GBM 上下文懒创建 |
| Memory | CPU shadow + GL buffer/texture；Memory 全局代次跟踪写入，未映射且未修改的静态 Buffer/线性 Image 跳过重复上传 |
| Render pass / Framebuffer | FBO（颜色纹理 + 深度纹理） |
| Pipeline | SPIR-V → GLSL 1.20（SPIRV-Cross），UBO 扁平化成 `uniform vec4 name[N]` |
| Vertex input | `glBindAttribLocation` + `glVertexAttribPointer` |
| `gl_VertexID` | 绑定 0..N-1 的 float 属性，索引取值即 index |
| Swapchain | GBM linear BO → EGLImage → GL texture；优先 `zwp_linux_dmabuf_v1` 零拷贝呈现，逐图像保留 `wl_shm` 回退 |
| 呈现同步 | dmabuf 与 wl_shm 共用独立 `wl_event_queue`，只分发 buffer release；两种路径都等待 compositor release，避免 acquire 内重入默认队列的 configure/swapchain 重建 |
| 呈现方向 | GL 行序直接呈现（Y 翻转约定与背面剔除方向必须一致） |
| 背面剔除 | **翻转 `glFrontFace`**（Vulkan Y-down vs GL Y-up） |

## 已知边界

- 118 个尚未实现的核心入口点注册为 **no-op 桩**（返回 `VK_SUCCESS`），保证 loader 不返回
  NULL、应用不崩，但语义未实现。影响较大的包括：`vkCmdClearAttachments`、
  `vkCmdBlitImage`、`vkCmdCopyImage`、`vkCmdResolveImage`、`vkCmdBeginQuery`/`EndQuery`、
  `vkCmdWriteTimestamp`、`vkCreateBufferView`、`vkQueueBindSparse`。


- 二级命令缓冲区（`vkCmdExecuteCommands`）不会继承主缓冲区的 pipeline/描述符绑定，
  需在二级缓冲区里自行绑定；空绑定执行会画出错误结果（不会崩溃）。


- 不支持 compute、多渲染目标、实例化、时间戳查询；
- 纹理仅支持常见 8bit 格式与线性 tiling 上传；
- dmabuf 使用线性 `XR24` 和 `DRM_FORMAT_MOD_INVALID`；KWin 在本机上拒绝 modifier 0 的导入，
  而接受 `0x00ffffffffffffff`。单个图像导入/FBO 失败时只回退该图像到 `wl_shm`；
- `VKGL_NO_DMABUF=1` 可强制旧的 `glReadPixels` + `wl_shm` 路径用于对照；
- 目前只声明实际实现的 FIFO。vkmark 默认硬请求 Mailbox，测试需显式加 `-p fifo`，不能靠虚报
  Mailbox/Immediate 能力绕过。

## 0.1.12 实测（2026-11-05，clang-LTO 内核）

- 修复 wl_shm 未等待 release 的共享内存竞争；timeout=0 的 acquire 也会先分发已到达事件，等待后重新扫描所有图像。
- 安装后的 deb 分别跑 dmabuf 与 `VKGL_NO_DMABUF=1`：两条路径均 **5/5、0 validation errors、GPU-only**。
- 640×480 FIFO vkmark 五场景：dmabuf **120/132/101/61/120 FPS**，wl_shm **77/82/69/57/74 FPS**。
  这是小型场景结果，不外推为大型 3D 性能。
- 保留呈现前 `glFinish`：当前没有可验证的 dma-buf fence 交接，不能用 glFlush 猜测替代。

## 0.1.14 实测（2026-11-05）

- Swapchain 图像增加 acquired 状态：已经 acquire、尚未 present 的图像不会被再次返回；成功 acquire 才给调用方 fence 置位，present 时清除 acquired 并等待 compositor release。
- `CopyBufferToImage` 按 `bufferRowLength` 逐行打包，同时更新线性 Image 的 shadow 和 Memory 代次，避免下一次采样被旧 shadow 覆盖。
- Swapchain 正常析构与 memfd/ftruncate/mmap 部分创建失败共用资源清理，补充 wl_shm proxy 释放。
- ICD manifest 改为 `/usr/lib/vkgl/libvkgl.so`，不再引用开发目录；重新编译源码树不会偷换已安装驱动。
- 新 `vktest-acquire` 在 dmabuf/wl_shm 均通过：图像独占 acquire、fence、耗尽后 timeout、release 后重新 acquire。
- 新 padded 线性纹理测试中，真实 0.1.13 `/usr/lib` 对照红/蓝像素均为 **0**；0.1.14 两条路径均为 **153,600 + 153,600**。
- 安装后的 deb 两条路径仍均为 **5/5、0 validation errors、GPU-only**。本版是正确性与生命周期修复，不宣称性能提升。

## 0.1.13 实测（2026-11-05）

- Memory 写入增加全局代次，Buffer/Image 记录已上传代次；Unmap、UpdateBuffer、FillBuffer、CopyBuffer 均使代次前进。
  持久映射资源仍逐 draw 上传，避免漏掉应用直接写入；UpdateBuffer 同时修正了遗漏的 `mem_offset`。
- 安装后的 deb 在 dmabuf 与 wl_shm 两路径均为 **5/5、0 validation errors、GPU-only**；实际截屏包含彩色内容，
  相邻帧在立方体区域有 52,659 个显著变化像素，确认不是静态假画面。
- 同机 1920×1080 vkmark 总分：0.1.12 为 **25**，缓存版为 **24**。这一差异视为波动，不宣称 1080p 流畅度提升；
  本版确定消除的是未修改静态资源的冗余 CPU→GPU 上传。

## 0.1.11 实测（2026-11-05）

- `scripts/test-vulkan.sh`：**5/5 通过**，包括 vkcube、vkmark 五场景、0 validation errors、无 llvmpipe；
- vkcube 截图有真实彩色内容，连续帧差分确认动画变化；
- 1920x1080 vkmark 五场景（每场景 3 秒）：dmabuf 总分 **21–23**，同库 `VKGL_NO_DMABUF=1` 为 **16**；
  零拷贝有明确收益，但约 20 FPS 仍不等于大型 3D 应用可流畅运行；
- 调试日志：`VKGL_DEBUG=1`；发布时普通运行不打印逐帧日志。

## 测试

```bash
vulkaninfo --summary          # GPU0 为 vkgl
vkcube                       # 立方体 + 旋转
vulkanCapsViewer             # GUI 能力浏览
vulkanCapsViewer -s caps.json  # 无头导出
```
