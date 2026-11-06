# vkgl CopyImageToBuffer development readback result

## Scope and dev-only artifacts

Only `/home/houge/infra/vkgl/vkgl.cpp` was modified, in the previously empty `Cmd::CopyImageToBuffer` execution case. The original source is saved as `vkgl-readback-original.cpp`; the one-hunk patch is `vkgl-readback-dev.patch` (51 diff lines, 39 added implementation lines).

The built candidate is `/home/houge/infra/research/lg110/libvkgl-readback-dev.so`, selected only through `vkgl-readback-dev-icd.json`. The system `/usr/lib/vkgl/libvkgl.so` was not replaced, nothing was installed, and no public repository was changed or pushed.

## Implementation

The implementation follows the existing swapchain-present read path rather than copying the image's potentially stale host shadow:

- `ensure_fbo(im)` obtains the FBO used by actual draw rendering.
- `glBindFramebuffer(GL_FRAMEBUFFER, im->fbo)` and `glReadPixels(..., GL_UNSIGNED_BYTE, ...)` read the GPU-rendered pixels.
- B8G8R8A8_UNORM uses `GL_BGRA_EXT`; R8G8B8A8_UNORM uses `GL_RGBA`, matching existing format/present paths.
- Supports the tested color mip 0, one array layer, 2D regions, nonzero image offset/extent, `bufferOffset`, and `bufferRowLength=0` tightly packed output. Image and buffer/memory bounds are checked before writing.
- Vulkan's top-left image Y is converted to GL read Y as `image_height - imageOffset.y - region_height`. The temporary GL bottom-up rows are reversed when copied into Vulkan top-down buffer order.
- The destination is the bound buffer memory shadow at `mem_offset + bufferOffset`; memory revision is incremented after the write, consistent with existing copy paths.

The present zero-copy dmabuf branch remains unchanged: it still performs only FBO bind/`glFinish`/unbind, with no CPU pixel copy. No performance claim is made. Other formats, depth, multiple mips/layers, and padded rows are not claimed supported.

## Actual A/B center-pixel result

The unchanged `vktest-gradient-sample-readback` test binary and shader were run with the dev ICD. Each ran 120 frames sequentially on active Wayland with no concurrent GPU test. Device was `LoongGPU LG110 (vkgl GLES2)`; no software renderer was selected.

A texture is 64×64 all zero; B changes only red to 255. The fragment uses the same fixed-UV sample and 0.25 weight. The surface is B8G8R8A8_UNORM and the tolerance remains ±2 per byte/channel.

| Run | Ideal top-left-center B,G,R,A | Actual B,G,R,A | Sentinel overwritten | Validated pixels | Result |
| --- | --- | --- | --- | ---: | --- |
| A | `00 80 00 ff` | `00 7f 00 ff` | yes | 1 | PASS, exit 0 |
| B | `00 80 40 ff` | `00 7f 40 ff` | yes | 1 | PASS, exit 0 |

The 1-LSB green difference is explained by the coordinate conversion rather than tolerated arbitrarily: Vulkan region Y=240, height=1 maps to GL row `480-240-1=239`; `round((239.5/480)*255)=127`. All other bytes exactly match. A to B changes only red byte offset 2 from 0 to 64, exactly the expected `0.25*255≈64` contribution.

Raw four-byte results are saved as `vktest-gradient-sample-readback-dev-A.rgba` and `vktest-gradient-sample-readback-dev-B.rgba`.

## Complete steady-state GFX IB comparison

Both traces used `LG110_TRACE_MAX=512` and `LG110_TRACE_IB_DWORDS=512` and contain 121 CS calls, all ret=0.

| Sequence class | Per run | Captured size | Notes |
| --- | ---: | --- | --- |
| 1 XDMA | 1 | 16/16 dwords | Complete |
| Sequence 2, frame containing readback | 1 | 512 of actual 672 dwords | Truncated by fixed cap; excluded from steady-state comparison |
| Sequences 3–121 | 119 | 400/400 dwords each | Complete, 0 truncated, 0 unresolved |

The requested 119 frame-matched steady-state GFX pairs were compared dword for dword:

- Pairs: 119; dwords per pair: 400.
- Equal: **47,600**; different: **0**.
- Complete pairs: 119; truncated pairs: **0**.
- SHA-256 over all 47,600 dwords in sequence order is identical for A and B: `04ed5b028b951ea6da35e8945c8963005c712f59e4116bc3e1412f210803dc1d`.

Thus, within this test, the observed center-pixel red difference comes from texture data while steady-state GFX command contents remain identical. The truncated first readback IB is explicitly not included in this statement.

## Existing-workload smoke

Using the same dev ICD:

- Existing `vktest-clear`: `PASS vktest-clear`, exit 0.
- Existing `vktest-copy-texture` source was built unchanged as a research binary because no existing binary was present; build log is empty, then `PASS vktest-copy-texture`, exit 0.

These are functional smoke results only, not broad regression or performance certification.

## Logs and report artifacts

Under `/home/houge/infra/research/lg110`:

- Patch/candidate: `vkgl-readback-original.cpp`, `vkgl-readback-dev.patch`, `vkgl-readback-dev-build.log`, `libvkgl-readback-dev.so`, `vkgl-readback-dev-icd.json`.
- A: `vktest-gradient-sample-readback-dev-A.stdout.log`, `vktest-gradient-sample-readback-dev-A-full.log`, `vktest-gradient-sample-readback-dev-A.exit`, `vktest-gradient-sample-readback-dev-A.rgba`.
- B: corresponding `...-dev-B...` stdout, full trace, exit, and raw pixel files.
- IB comparison: `vktest-gradient-sample-readback-dev-ib-diff.log`.
- Smoke: `vktest-clear-readback-dev-smoke.log/.exit`, `vktest-copy-texture-dev-smoke-build.log`, `vktest-copy-texture-readback-dev-smoke.log/.exit`, and the research `vktest-copy-texture` binary.
- This report: `vkgl-copy-image-to-buffer-dev.md`.
