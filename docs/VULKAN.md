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
| Format capabilities | RGBA8 UNORM/SRGB 声明实际 sampled linear-filter/color-blend/transfer；R32 一至四分量 SFLOAT 声明 vertex-buffer；core FormatProperties2 一致，不扩大未验证格式 |
| `ClearAttachments` | 活跃 render pass 内 color attachment0 的受限矩形/多层拒绝；用 `glScissor + glClear` 直接 GPU 清除，保存恢复 FBO/scissor/clear color/color mask |
| `CopyImage` | 不同 2D color、同 B8/R8 UNORM、mip0/单层、未映射 dst 的受限区域，从 src FBO 用 `glCopyTexSubImage2D` 直接复制到 dst texture；无 vkgl CPU 像素中转 |
| `CopyImageToBuffer` | 当前 2D color mip0/单层、B8/R8 RGBA8 与 tight/`bufferRowLength` padded 区域从实际 FBO `glReadPixels`，转换 Y 方向并检查溢出/边界；不宣称 depth/多层支持 |
| `CopyBufferToImage` | tight 行（rowLength=0/width）在完整校验后直接引用源 shadow，避免额外 CPU packed copy；padded 与 linear 同 allocation 保留 packed snapshot |
| Queue submission fence | 运行时探测 `EGL_KHR_fence_sync`，用真实 EGL sync 实现 status/waitAll/waitAny/reset；不可用时保守 `glFinish`，acquire fence 仍为主机事件 |
| Render pass / Framebuffer | FBO（颜色纹理 + 深度纹理） |
| Pipeline | SPIR-V → GLSL 1.20（SPIRV-Cross），UBO 扁平化成 `uniform vec4 name[N]` |
| Draw topology | point/line/line-strip/triangle/triangle-strip/triangle-fan 映射真实 GL mode，indexed/non-indexed 共用；adjacency/PATCH 返回 `VK_ERROR_FEATURE_NOT_PRESENT`，不再硬编码 triangles |
| Vertex input | `glBindAttribLocation` + `glVertexAttribPointer` |
| Sampler | 单级2D NEAREST/LINEAR 与 repeat/mirrored/clamp 真实应用；按 Image 当前 GL 状态值去重；compare、unnormalized、非零 LOD bias、border/mirror-clamp、cubic 显式拒绝 |
| `gl_VertexID` | 绑定 0..N-1 的 float 属性，索引取值即 index |
| Swapchain | GBM linear BO → EGLImage → GL texture；优先 `zwp_linux_dmabuf_v1` 零拷贝呈现，逐图像保留 `wl_shm` 回退 |
| 呈现同步 | dmabuf 与 wl_shm 共用独立 `wl_event_queue`，只分发 buffer release；两种路径都等待 compositor release，避免 acquire 内重入默认队列的 configure/swapchain 重建 |
| 呈现方向 | GL 行序直接呈现（Y 翻转约定与背面剔除方向必须一致） |
| 背面剔除 | **翻转 `glFrontFace`**（Vulkan Y-down vs GL Y-up） |

## 已知边界

- 116 个尚未实现的核心入口点注册为 **no-op 桩**（返回 `VK_SUCCESS`），保证 loader 不返回
  NULL、应用不崩，但语义未实现。影响较大的包括：
  `vkCmdBlitImage`、`vkCmdResolveImage`、`vkCmdBeginQuery`/`EndQuery`、
  `vkCmdWriteTimestamp`、`vkCreateBufferView`、`vkQueueBindSparse`。


- 二级命令缓冲区（`vkCmdExecuteCommands`）不会继承主缓冲区的 pipeline/描述符绑定，
  需在二级缓冲区里自行绑定；空绑定执行会画出错误结果（不会崩溃）。


- 不支持 compute、多渲染目标、实例化、时间戳查询；
- 纹理仅支持常见 8bit 格式与线性 tiling 上传；
- anisotropy 仍为 feature=0/max=1：硬件虽暴露扩展和16上限，但16×原型在放大采样中产生错误像素，未通过启用门槛；多级 LOD 亦不宣称；
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

## 0.1.25 实测（2026-11-06）

- 修复 dispatchable 对象生命周期 ABI：loader 会用 dispatch 表覆盖 Instance/Device/Queue/CommandBuffer 首字，普通 C++ 虚析构会把该表误当 vtable。现用限定非虚析构+匹配的全局 operator delete，不改变对象布局；非 dispatchable 不变。
- 实际渲染下旧式虚析构曾在 FreeCommandBuffers/DestroyDevice 跳入 libvulkan/strcmp 崩溃；gdb 首字和 ASan 证据公开。修复后 ASan 通过，候选及安装版各连续5次完整 vkmark 退出通过，新增64周期公开生命周期测试。最小测试在旧版也通过，不冒充独立负对照。
- 本轮是**正确性而非性能发布**：最终配对 vertex -3.21%、texture -7.26%、shading +0.67%，不利数据不隐藏；getenv缓存、函数对齐、cold分离组合均未采用。完整像素/fence/acquire及双路径28/4不变。安装库 SHA-256=`00fb9eb672cd9c42a05d94ab4eae6a2637c3cb1fb3555cd746f9dba579747a50`，deb SHA-256=`a630b3b3e6bcefb90c9cd59703d9a0df49327cc824cd1c74b02b9315e322e57e`；24包发布全过。见 [0.1.25 报告](../scripts/tests/vkgl/vkgl-0.1.25.md)。

## 0.1.24 实测（2026-11-06）

- Vulkan/GL 源码与0.1.23完全相同，仅将 C/C++ 构建改为 GCC16.2 `-O3 -march=loongarch64 -mtune=la664 -flto`。ISA 仍是 loongarch64 基线，LA664 只做调度调优，不是 native/LASX-only 包。
- 五轮正反顺序、640×480 FIFO、当前约8负载下，LTO 相对0.1.23 O2 的 vertex/texture/shading 中位数分别为 **184→197（+7.07%，5/5）**、**120→127（+5.83%，4/5）**、**150→153（+2.00%，4/5）**。纯O3虽shading +10.42%，但vertex -6.15%，故未选择。提升仅限这些小场景与已记录环境，不宣称榨尽硬件或全应用通用。
- 实测候选、包内、安装库字节一致；完整像素/tight/fence/acquire及安装双路径全过，validation仍28/4。安装库 SHA-256=`f3c17974dc01ac5438a94273f8f5523563b8172f9c69efc3e2eaab67fb061e3a`，deb SHA-256=`7b39464c262fdb3c46bd5bb74202de15706e2fb154321fba67476952c307a00a`；23包发布验证全过。原始O3/LTO JSON与环境记录见 [0.1.24 报告](../scripts/tests/vkgl/vkgl-0.1.24.md)。

## 0.1.23 实测（2026-11-06）

- 不再丢弃 `VkSamplerCreateInfo`：单级2D combined sampler 真实应用 NEAREST/LINEAR 与三种 U/V wrap，Image 按实际 GL 状态值去重；六类未支持状态返回 `VK_ERROR_FEATURE_NOT_PRESENT`。公开 split-texture 对照中0.1.22请求LINEAR仍为64，0.1.23 NEAREST=64/LINEAR=34；三draw三次绑定仅一次状态提交。
- anisotropy **没有启用**：硬件扩展/16上限虽存在，但原型使放大LINEAR像素34错误变为64，故保持feature=0/max=1并公开负证据。validation 两路径仍为28 events/4类，与0.1.22逐项一致，不伪装减量。
- 完整像素/tight/fence/acquire与两路径烟雾全过。安装库 SHA-256=`1a2ee24ee1add7a39c78b1d52ea8615bd76d1c49b336f58eadc9e4d11e11e0be`，deb SHA-256=`444d76daf4b4fd7229d1410285b18ad192f175828fda8623eeace73a163b76f7`；22包索引/签名/HTTPS/隔离apt/远端字节全过。高负载配对FPS与同字节噪声数据一并公开，不作性能提升或稳定回归归因。见 [0.1.23 报告](../scripts/tests/vkgl/vkgl-0.1.23.md)和[anisotropy负证据](../scripts/research/lg110/lg110-sampler-anisotropy.md)。

## 0.1.22 实测（2026-11-06）

- 修正格式能力查询：RGBA8 UNORM/SRGB 增加实际已支持的 linear-filter 与 color-attachment-blend 位，R32 一至四分量 SFLOAT 增加 vertex-buffer 位，core FormatProperties2 返回一致结果。未借机声明未验证格式或功能。
- 同一公开 validation 烟雾脚本在 dmabuf/wl_shm 均从 **59→28 个 events、8→4 类 VUID**，精确消除 vertex-format、renderPass/blend、magFilter 四类共31项；剩余计数不变且无新类。仍未 clean：optimal+PREINITIALIZED、anisotropy 16>当前上限1、queue/acquire binary semaphore 留作后续，不通过过滤或虚报能力掩盖。
- 公开同一 format 测试在0.1.21失败、安装0.1.22通过；完整像素、tight/fence/acquire、六topology及不支持模式回归全过。安装库 SHA-256=`759dadbb7de2b44ed7cd01fefb9ed2c092e26e84b779bc247afa5d8f393195d2`，deb SHA-256=`74316cc6d4574cef6e62584cee567ee4bdbf8aef0078c27f5e4d57eb3ef5d72f`；21包索引/签名/HTTPS/隔离apt/远端字节全过，无性能宣称。见 [0.1.22 报告](../scripts/tests/vkgl/vkgl-0.1.22.md)。

## 0.1.21 实测（2026-11-06）

- 修复 pipeline 已记录 topology、执行却将 indexed/non-indexed 都硬编码为 `GL_TRIANGLES` 的兼容性 bug。六种基础 topology 现在映射真实 GL mode；adjacency/PATCH 在创建 pipeline 时返回 `VK_ERROR_FEATURE_NOT_PRESENT`，不再静默误画。
- 安装库 SHA-256=`f7452278fcaa6d7bfde600527c523181d87cdd599ee0a7487e135b8f612bb063`；deb SHA-256=`ec99119b9bb4aaa4ce44dfcc357d3fe14fda0de32049c1ef1c624bf2ad979bed`。候选、包内和安装库字节一致；20 包索引、签名、HTTPS、隔离 apt、远端 deb 均通过，0.1.21-1 为源中最高版本。
- 六种 topology 的真实像素和完整 native IB 均验证，主动 triangle→point 仅修改 word268 并复现 passive point；count 回归和旧值拒绝通过。A/B gradient/padded/CopyImage/ClearAttachments、tight/fence/acquire 全部通过。
- **不宣称 validation clean**：同一公开脚本下，0.1.20 与 0.1.21 在 dmabuf/wl_shm 各报告 59 个 diagnostics、8 类 VUID，逐类计数完全一致，属于既有兼容债而非本版新增。渲染均使用 LG110，无 llvmpipe；本版无性能提升宣称。详见 [0.1.21 报告](../scripts/tests/vkgl/vkgl-0.1.21.md)。

## 0.1.20 实测（2026-11-06）

- tight `CopyBufferToImage` 在 rowLength=0/width 时不再分配并填充额外 CPU packed snapshot；完整边界校验、padded 路径和 linear 同 allocation 防覆盖 snapshot 均保留。精确 0.1.19 deb 与已安装 0.1.20 使用同一公开测试二进制，五组正反顺序配对、每次七批 64 个 1024² RGBA optimal-tiling 上传：tight 中位时间 `0.682755353→0.640047151 s`，对应吞吐 **+6.67%，5/5 对更快**；padded 对照 **-0.53%，0/5**。仅限该 microbenchmark，不外推 vkmark FPS。
- 队列提交 fence 从“flush 后直接置 bool”改为运行时探测的真实 `EGL_KHR_fence_sync`；支持 status、waitAll/waitAny、reset、共享 deadline 和未提交零超时，缺少能力时保守 `glFinish`。acquire fence 仍是 host signal，dmabuf present `glFinish` 未改。
- 安装版实测 vendor `EGL_KHR_fence_sync=1`；0.1.19 未提交零等待错误返回成功可复现，0.1.20 返回 `VK_TIMEOUT`，真实 waitAll/waitAny、Reset 后重提交和 `663300ff` 像素读回全通过。11 个 deterministic host 用例也通过。
- 安装库 SHA-256=`d9aad6dea8cb6b39e538722cb8b1fccb8f3bc9aa44380dc5944725d0eea0067d`；deb SHA-256=`1665530c2b600172cbd2ee00ed882f92bb10458078a36265d2357e9d404184a7`。安装 dmabuf/wl_shm 均 **5/5、0 validation、GPU-only**，六组 tight/padded 对照、公开四类像素 A/B、acquire 与两路径各 153600红+153600蓝截屏全部通过。19 包索引、签名、HTTPS、隔离 apt 与远端 deb 字节全部验证。
- 完整数据见 [0.1.20 发布报告](../scripts/tests/vkgl/vkgl-0.1.20.md)。本版仍是 Vulkan-over-GL，不是 native Vulkan。

## 0.1.19 实测（2026-11-06）

- 实现原先完全 no-op 的 `vkCmdClearAttachments`：仅限活跃 render pass 的 color attachment0、baseArrayLayer0/layerCount1、非负且完整位于 framebuffer 内的矩形；支持多个 clear attachment 与 rect 的组合，depth/stencil/越界明确拒绝。
- 执行使用 `glScissor + glClear` 直接 GPU 清除，并保存恢复 FBO、scissor box/enable、clear color 与 color mask；命令记录深拷贝 clear/rect，不保留调用者栈指针。
- 真实测试分别 padded readback 已清除的 `(320,239) 2×2` 和邻近未清除的 `(324,239) 2×2`。A/B 的 clear 区四像素均为 `40 20 10 ff`；邻近区保持 gradient+sample，A/B 仅 red 由 0 增至 64。两个 32-byte buffer 的全部 guard/padding 均不变。
- 2048-dword full-line tracer 捕获 120 个逻辑帧对应的 121 个 GFX：seq2=800、seq3=272、seq4..122=400；总计 **48,672 dword 全同、0 截断**，A/B SHA-256 均为 `834addd09674cf704413cde34ca3f68bbec9a263b5903ee2ed54042254954df5`。通用比较器拒绝 122 个成功 CS，专用严格比较器按实际 XDMA+GFX 布局验证，不推断额外物理提交的未知语义。
- 包内/安装/dev SHA-256 均为 `04e51457f62e56b4a3443c3cd99e6a0d7a0cffb74e9b72fff765d7c327da38e3`。安装版 tight/padded/CopyImage/ClearAttachments A/B、dmabuf/wl_shm **5/5、0 validation errors、GPU-only** 全部通过；公开相对路径 ClearAttachments 源码重新构建后的 A/B 及四个 raw buffer 也与 dev 完全一致。无性能或完整 Vulkan 合规宣称。

## 0.1.18 实测（2026-11-06）

- 实现原先完全 no-op 的 `vkCmdCopyImage` 记录与执行：不同 2D color、同 B8/R8 UNORM、mip0/单层、depth1、合法 transfer usage 和完整边界；dst 必须 bound 且未映射。src FBO 通过 `glCopyTexSubImage2D` 直接复制到 dst texture，无 vkgl CPU 像素中转；FBO/texture 绑定恢复，present dmabuf 未改。
- 真实链路为 `src draw → 64×64 offscreen dst clear(a5) → CopyImage (320,239)→(7,9) → dst padded readback`。安装版 A/B 的 32 字节均与期望完全相同，4 像素有效、guard/padding 不变，仅四个 red 字节由 0 增至 64。
- 初次 1024 tracer 发现 seq2 实际 800 dwords，但 8192-byte 日志行只序列化 733 个，未将其算作完整。full-line 变体扩大行缓冲后，120 组 GFX 全完整：seq2=800/800，其余 400/400；共 **48,400 dword 全同、0 截断**，A/B SHA-256 均为 `c1b0cb49f9d2cb2d024bb65204f7436466c4fddcd7bacc8c99d974f70be7a95e`。
- 安装库/dev SHA-256 均为 `adf5baef948c65191180fc93124741101ae7de26af4e4bed86050297e568783d`；安装版 tight/padded/CopyImage A/B 与 dmabuf/wl_shm **5/5、0 validation errors、GPU-only** 全部通过。公开相对路径 CopyImage 源码也重新构建并通过安装版 A/B。无性能或完整 Vulkan 合规宣称。
- 实现、坐标、代次限制、raw buffer 与完整日志索引见 [CopyImage 报告](../scripts/research/lg110/vkgl-copy-image-dev.md)。

## 0.1.17 实测（2026-11-06）

- `CopyImageToBuffer` 增加 `bufferRowLength`：0 保持 tight；非零按 `rowLength*4` 写目标行，空间需求为 `(height-1)*stride + width*4`，包含整数溢出和 memory/buffer 边界检查。
- 合法的 2×2、rowLength=4、bufferOffset=4、32-byte buffer A/B 在安装版逐字节等于期望：4 个像素正确，8-byte 行间 padding 与首尾 guard 全部保持 `a5`；A→B 仅四个 red 字节由 0 增至 64，未使用或放宽容差。
- 只读 tracer 的独立 1024-dword 变体补齐安装版 0.1.16 的首个 readback IB：120 组 GFX 中 seq2 为 672/672，其余为 400/400；共 **48,272 dword 全同、0 截断**，A/B SHA-256 均为 `7430660bf066aa62c359ea8ed343d54a6ea0234e2e3d60da079d10f76ffff535`。
- 0.1.17 安装库与 padded dev 库 SHA-256 均为 `213e68397bba7d5839a477e77e7e65fb594f83fdc45de5754e7538dbb3712490`；安装版 tight/padded A/B 与 dmabuf/wl_shm **5/5、0 validation errors、GPU-only** 全部通过。无性能宣称。
- 公式、精确 hexdump、patch 与原始 buffer 见 [padded readback 报告](../scripts/research/lg110/vkgl-buffer-row-length-readback-dev.md)。

## 0.1.16 实测（2026-11-06）

- 实现此前空缺的 `Cmd::CopyImageToBuffer`：从实际 draw 的 FBO 读回，而非复制可能过期的 image host shadow；当前限 2D color mip0/单层、B8/R8 RGBA8 UNORM、offset/extent 与 tightly-packed 输出。dmabuf present 零拷贝分支未改。
- 相同 shader/命令的纹理 A/B 在安装版得到真实中心像素：A=`00 7f 00 ff`，B=`00 7f 40 ff`，仅 red 增加 64，符合 `0.25*255`；绿色与理想值差 1 LSB 由 GL 行 239 的坐标转换解释。119 组稳态 GFX IB 共 47,600 dword 完全相同；首个含 readback 的 672-dword IB 因 512 捕获上限被明确排除。
- 安装库与测试 dev 库 SHA-256 均为 `025f5cff6c68a436fc299fcf81caf24364c453128a35d17329d4da9068e4b39a`；安装后的 dmabuf/wl_shm 均为 **5/5、0 validation errors、GPU-only**。这是兼容性修复，不宣称性能提升。
- 源码、patch、原始像素和报告见 [`scripts/research/lg110`](../scripts/research/lg110/)，详细结果见 [CopyImageToBuffer 报告](../scripts/research/lg110/vkgl-copy-image-to-buffer-dev.md)。

## 0.1.15 实测（2026-11-05）

- 临时 wall/thread-CPU instrumentation 发现常见 color+depth render pass 每帧实际调用三次 clear：color+depth、depth loop、post-loop depth。提交末尾 `glFlush` 等待这些 GL/GPU 工作；逐 draw `glGetError` 仅约 1.48 μs，不是热点。
- 对 `fb->color && fb->depth && clears.size()==2` 合并为一次 `glClear(COLOR|DEPTH)`，先设置两个 clear value；所有其他 attachment/clear 排列保留原 fallback。未改 `glFlush`、present `glFinish`、dmabuf 或 fence/release。
- 最终合入库与安装包逐字节一致。640×480 FIFO vkmark，六轮 base/candidate 交错配对、每场景 5 秒，**18/18 对均提升**：

  | 场景 | 0.1.14 中位 FPS | 0.1.15 中位 FPS | 变化 | 中位帧时间变化 |
  |---|---:|---:|---:|---:|
  | vertex | 149 | 222 | +49.0% | 6.713→4.505 ms（-32.9%） |
  | texture | 106.5 | 149 | +39.9% | 9.465→6.723 ms（-29.0%） |
  | shading | 119 | 157 | +31.9% | 8.463→6.421 ms（-24.1%） |

  原始数据与可复现 runner：[`scripts/tests/vkgl`](../scripts/tests/vkgl/)。无 clear 的 effect2d/desktop 不宣称提升，也不外推大型 3D。
- 1920×1080 FIFO 四轮交错配对共 24 次成功运行，12/12 对均提升：vertex **21.5→27.5 FPS（+27.9%）**，texture **20.5→26.0（+26.8%）**，shading **21→25（+19.0%）**。两变体均实测 gsgpu、三张 dmabuf 零拷贝图像、无 llvmpipe，库 hash 在矩阵期间不变；仍不外推其他场景。
- 安装后的 deb 在 dmabuf/wl_shm 均为 **5/5、0 validation errors、GPU-only**；acquire/fence 与 padded copy 回归保持通过。

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

## 原生 GSPKT 追踪与字段因果（2026-11-06）

- 只读 `LD_PRELOAD` tracer 已能关联 GEM/VA/mmap 与 CS，并恢复真实厂商 GL 提交的 IB 内容；源码和样本见
  [`scripts/research/lg110`](../scripts/research/lg110/)。
- 实测厂商 GEM_VA 使用 `0xc0286448`（IOWR），公开 1.0.2 头为 `0x40286448`（IOW）；payload 相同，tracer 兼容匹配但不改原 ioctl。
- `vktest-clear` 样本 4/4 次 CS 成功、4/4 个 576 字节 IB 均完成 32-dword dump，并带 FENCE chunk；首字包含未知 opcode `0x88`，后续出现公开头注释为 DRAW 的 `0x89`。
- 独立 opt-in pre-CS 实验在完整可写映射、流长度/签名和原始值防护下，分别只修改 clear IB 的一个 dword。400-word swapchain 流中 red/green/blue=word87/88/89，raw 分别从 `663300ff` 到 `6633ffff`/`66ff00ff`/`ff3300ff`，每次只有一个对应像素通道变化。
- alpha=word90 不能由 swapchain 直接证明：主动写入与被动 alpha=0.5 都只改该词但读回固定 `ff`。独立 272-word 离屏 optimal RGBA 流可保留 alpha；word90 `1.0→0.5` 主动实验得到 `003366ff→00336680`，完整 IB 和像素均只有对应一项变化。公开源码重建重复 green/offscreen-alpha，离屏 red 请求零写入。见 [clear RGBA 因果报告](../scripts/research/lg110/lg110-clear-color-fields.md)。
- draw3/draw2 被动差分的 XDMA 全同，672-word GFX 仅 word270 `3→2`、word276 `2→1`，中心由 `007f00ff` 变为 clear `663300ff`。主动实验仅改 word270、保持 word276=2，仍精确抑制三角形并复现 draw2 raw；已是 draw2 的输入被旧值防护拒绝且零 IB 差异。公开严格重建复现全部四项。这证明该流 word270 的 vertex-count 因果，word276 不命名。见 [draw-count 报告](../scripts/research/lg110/lg110-draw-count-field.md)。
- 0.1.20 的 point 请求因 vkgl 硬编码 triangles 而与 triangle 完全同 IB；修复后六种 topology 仅在 word268 呈现 `0/1/3/4/5/6 << 8` 对应值，其余 GFX 不变。主动 triangle→point 只改 word268 `0x40000400→0x40000000` 即精确复现 point clear raw，count 回归与 point 旧值拒绝通过；word273 不随 topology 改变，已更正为未命名字段。见 [draw-topology 报告](../scripts/research/lg110/lg110-draw-topology-field.md)。
- 这些仅是特定 clear/draw 流的字段因果证据，仍不是完整 0x88/0x89 包语义、完整 draw/shader ISA、地址/tile、独立 replay 或原生 Vulkan；不能复用 AMD PM4/RADV 代替。

## 测试

```bash
vulkaninfo --summary          # GPU0 为 vkgl
vkcube                       # 立方体 + 旋转
vulkanCapsViewer             # GUI 能力浏览
vulkanCapsViewer -s caps.json  # 无头导出
```
