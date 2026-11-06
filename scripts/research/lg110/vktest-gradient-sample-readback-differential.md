# LG110 gradient sampling readback A/B: copy execution unsupported

## Scope

- Independent source/binary: `vktest-gradient-sample-readback.c` / `vktest-gradient-sample-readback`.
- Independent fragment source/SPIR-V: `vktest-gradient-sample-readback.frag` / `vktest-gradient-sample-readback.frag.spv`.
- Retains original `grad.vert.spv`, one `texture()` sample at fixed UV `(0.25,0.25)`, geometry, render pass, 640×480 viewport/scissor, clear sequence, binding-0 combined-image-sampler descriptor, `vkCmdDraw(3,1,0,0)`, and the 120-frame loop.
- The sampled RGB weight is changed from the prior small value to fixed **0.25**. SPIR-V disassembly confirms one `ImageSampleImplicitLod` and a live `VectorTimesScalar -> FAdd -> Store outColor` dependency.
- Both runs use the same binary, shader, and recorded command sequence. Environment variable `LG110_TEXTURE_VARIANT` changes only texture bytes: A is 64×64 RGBA all zero; B is red=255 with green/blue/alpha zero. No texture-upload CS was added; the linear texture memory is directly initialized.

## Readback construction

The swapchain adds `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`. On frame 0 only, after the unchanged draw/render pass:

1. An image barrier requests present-to-transfer-source layout.
2. `vkCmdCopyImageToBuffer` requests one 1×1 pixel at image offset `(320,240)` into a 4-byte `VK_BUFFER_USAGE_TRANSFER_DST_BIT` buffer.
3. A second barrier requests transfer-source-to-present layout.
4. The submission uses a fence; `vkWaitForFences` completes before `vkMapMemory` reads exactly four bytes.

The readback allocation is initialized to sentinel bytes `a5 a5 a5 a5`. The four observed bytes are saved separately as `vktest-gradient-sample-readback-A.rgba` and `vktest-gradient-sample-readback-B.rgba`, including on failure. These files are raw readback-buffer bytes, not claimed valid pixels.

C used `-std=c11 -O2 -Wall -Wextra -Werror`; its build log is empty. Runs used installed vkgl 0.1.15, active Wayland, the existing 512-dword tracer, sequential execution with no concurrent GPU test, and reported `LoongGPU LG110 (vkgl GLES2)` rather than a software renderer.

## Expected center pixel and tolerance

The surface format is 44, `VK_FORMAT_B8G8R8A8_UNORM`, so bytes are ordered B,G,R,A. For pixel center `(320,240)`, the shader's ideal green is `round((240.5/480)*255)=128`. The red sample contributes `0.25*255=63.75`, ideally rounded to 64. Blue is zero and shader alpha is one.

Validation allows **±2 per byte/channel**.

| Run | Texture | Expected B,G,R,A bytes (hex) | Actual bytes (hex) | Validated pixels | Exit |
| --- | --- | --- | --- | ---: | ---: |
| A | All zero | `00 80 00 ff` | `a5 a5 a5 a5` | 0 | 1 |
| B | Red=255 only | `00 80 40 ff` | `a5 a5 a5 a5` | 0 | 1 |

The intended B-minus-A red difference is 64 at BGRA byte offset 2. The actual saved buffers have no differing bytes because both remain the sentinel; this is not a valid pixel comparison. Neither run prints PASS.

## Failure layer in installed vkgl

Read-only source inspection locates the failure precisely:

- `/home/houge/infra/vkgl/vkgl.cpp` lines 1374–1384: `vkgl_CmdCopyImageToBuffer` records `Cmd::CopyImageToBuffer` commands.
- `/home/houge/infra/vkgl/vkgl.cpp` lines 1677–1678: the command executor's `case Cmd::CopyImageToBuffer` body is empty.
- Lines 1697–1706: queue submission executes recorded commands and signals the fence, but cannot copy data when that command body is empty.
- Lines 1419–1422: fence wait marks fences signaled; lines 851–858: memory mapping returns the allocation's host shadow.

Accordingly, API submission/fence/map success does not establish readback success. The unchanged sentinel identifies the failure at the installed driver's **CopyImageToBuffer execution** layer, after pipeline creation and draw submission. No valid center pixel was obtained.

## Trace completeness, without pixel inference

Although pixel validation failed, both applications completed their 120-frame loops and returned their intentional validation failure (exit 1):

| Run | CS, all ret=0 | XDMA | GFX | IB dump status | Truncated GFX |
| --- | ---: | --- | --- | --- | ---: |
| A | 121 | 1 × 16/16 dwords | 120 × 400/400 dwords | 121 ok | 0 |
| B | 121 | 1 × 16/16 dwords | 120 × 400/400 dwords | 121 ok | 0 |

The 512-dword cap was sufficient. There were no unresolved dump records.

The conditional 119-group A/B steady-state GFX content comparison was **not performed**, because its purpose was to attribute an observed valid pixel difference to texture data and no valid pixels exist. These complete IB logs must not be used to claim that hardware sampling did or did not affect pixels.

## Bounded result and artifacts

The requested native pixel A/B validation is **not achieved** on installed vkgl 0.1.15 because CopyImageToBuffer execution is unimplemented. Pixel evidence count is zero for both A and B. No conclusion about sampled-data-dependent pixel output is drawn.

Artifacts under `/home/houge/infra/research/lg110`:

- Source/build: `vktest-gradient-sample-readback.c`, `vktest-gradient-sample-readback`, `vktest-gradient-sample-readback.frag`, `vktest-gradient-sample-readback.frag.spv`, `vktest-gradient-sample-readback-build.log`, `vktest-gradient-sample-readback-shader-build.log`.
- A: `vktest-gradient-sample-readback-A-full.log`, `vktest-gradient-sample-readback-A.stdout.log`, `vktest-gradient-sample-readback-A.exit`, `vktest-gradient-sample-readback-A.rgba`.
- B: corresponding `...-B-full.log`, `...-B.stdout.log`, `...-B.exit`, `...-B.rgba` files.
- This report: `vktest-gradient-sample-readback-differential.md`.

No installed driver or public repository was modified. No unknown LG110 opcode semantics or performance claim is made.
