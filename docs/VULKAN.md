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
| Memory | CPU shadow + GL buffer/texture |
| Render pass / Framebuffer | FBO（颜色纹理 + 深度纹理） |
| Pipeline | SPIR-V → GLSL 1.20（SPIRV-Cross），UBO 扁平化成 `uniform vec4 name[N]` |
| Vertex input | `glBindAttribLocation` + `glVertexAttribPointer` |
| `gl_VertexID` | 绑定 0..N-1 的 float 属性，索引取值即 index |
| Swapchain | `wl_shm` + `glReadPixels`（B8G8R8A8 用 `GL_BGRA`） |
| 呈现方向 | GL 行序直接写 wl_shm（两次 Y 翻转互相抵消） |
| 背面剔除 | **翻转 `glFrontFace`**（Vulkan Y-down vs GL Y-up） |

## 已知边界

- 二级命令缓冲区（`vkCmdExecuteCommands`）不会继承主缓冲区的 pipeline/描述符绑定，
  需在二级缓冲区里自行绑定；空绑定执行会画出错误结果（不会崩溃）。


- 无 compute、多渲染目标、实例化、时间戳查询；
- 纹理仅支持常见 8bit 格式与线性 tiling 上传；
- 交换链是 CPU 拷贝呈现（性能一般，但正确）。

## 测试

```bash
vulkaninfo --summary          # GPU0 为 vkgl
vkcube                       # 立方体 + 旋转
vulkanCapsViewer             # GUI 能力浏览
vulkanCapsViewer -s caps.json  # 无头导出
```
