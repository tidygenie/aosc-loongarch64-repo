# vkgl in-render-pass `Cmd::ClearAttachments` dev result

## Scope and implementation

Only `/home/houge/infra/vkgl/vkgl.cpp` and independent research tests/tools were changed. No system library was installed or replaced, and no public repository was modified or pushed. The dmabuf zero-copy present path is unchanged.

The original `vkCmdClearAttachments` was a no-op varargs stub. `vkgl-clear-attachments-dev.patch` (91 diff lines) adds:

- A real `Cmd::ClearAttachments` kind and a correctly typed recording entry point.
- Deep-owned `std::vector<VkClearAttachment>` and `std::vector<VkClearRect>` copies, plus the active framebuffer/render-pass pointers. No stack clear-value or rect pointers are retained.
- Support for iterating multiple color-attachment-0 clears and multiple rects. Each clear is applied to every rect; the independent test exercises one attachment and one rect.
- Full validation before any GL state change or pixel write: active fb/rp and color attachment0/FBO must exist; only `VK_IMAGE_ASPECT_COLOR_BIT`, colorAttachment0, baseArrayLayer0 and layerCount1 are accepted. Empty, depth/stencil, negative, zero-sized, or out-of-bounds requests are rejected without writing.
- Bounds use signed 64-bit x/y/width/height arithmetic, reject framebuffer dimensions above INT32_MAX, and compute GL scissor Y as `fb_height - rect_y - rect_height`.
- Direct GPU `glScissor` + `glClear(GL_COLOR_BUFFER_BIT)` in the active render pass; no CPU pixel staging is used by ClearAttachments.
- Save/restore of framebuffer binding, GL scissor box and enable, clear color, and all four color-write masks. The executor's logical Vulkan `sc` state is left unchanged. Clear temporarily enables scissor and sets all color masks to GL_TRUE so pipeline colorWriteMask does not suppress the Vulkan clear.

Build log `vkgl-clear-attachments-dev-build.log` is empty. Immediate base source is preserved as `vkgl-clear-attachments-base.cpp`.

## Independent two-region real-pixel A/B test

`vktest-clear-attachments.c` retains the gradient+fixed-UV sample draw. On frame0 it records a local-stack `VkClearAttachment` and `VkClearRect` inside the active render pass, after the draw and before `vkCmdEndRenderPass`; submission occurs after those locals leave scope, exercising the required deep-copy lifetime.

- Clear rect: `(320,239)`, 2×2, fixed RGBA `(16,32,64,255)`.
- Neighboring untouched rect: `(324,239)`, 2×2, separated from the clear rect by two columns.
- The pipeline colorWriteMask enables R/G/A but disables B. Original gradient blue is already zero, so neighbor expectations are unchanged; clear blue=64 verifies that ClearAttachments correctly overrides the non-full mask.
- After the render pass, each region is copied to its own 32-byte padded buffer with rowLength4 and valid bufferOffset4.
- Both buffers start as `a5`; data occupies 4–11/20–27, padding 12–19, guards 0–3/28–31. Pixel tolerance remains ±2 and guards/padding require exact `a5`.
- Frames1–119 retain the unchanged draw. A changes no texture bytes; B changes only sampled texture red to 255.

Both runs completed 120 application frames and exited 0 with `clear_pixels=4 neighbor_pixels=4`. Actual bytes exactly matched expectations:

```text
clear A = clear B
00000000: a5 a5 a5 a5 40 20 10 ff 40 20 10 ff a5 a5 a5 a5
00000010: a5 a5 a5 a5 40 20 10 ff 40 20 10 ff a5 a5 a5 a5

neighbor A
00000000: a5 a5 a5 a5 00 80 00 ff 00 80 00 ff a5 a5 a5 a5
00000010: a5 a5 a5 a5 00 7f 00 ff 00 7f 00 ff a5 a5 a5 a5

neighbor B
00000000: a5 a5 a5 a5 00 80 40 ff 00 80 40 ff a5 a5 a5 a5
00000010: a5 a5 a5 a5 00 7f 40 ff 00 7f 40 ff a5 a5 a5 a5
```

The clear regions are byte-for-byte identical between A/B, including the fixed blue=64 despite the pipeline's disabled blue mask. Neighbor regions differ only at offsets 6/10/22/26, red 0→64. All guards and padding in both buffers remain `a5`.

Raw files: `vktest-clear-attachments-dev-A/B-clear.bin` and `...-neighbor.bin`. Full verified dump: `vktest-clear-attachments-dev-hexdump.log`.

## 2048 full-line capture and actual split layout

Both runs used `lg110-iotrace-2048.so` with `LG110_TRACE_MAX=512` and `LG110_TRACE_IB_DWORDS=2048`.

Recorded facts per run:

- 120 application frames, **122 successful CS** calls, all ret=0.
- One 16-dword XDMA and **121 physical GFX** submissions.
- seq2: **800/800 dwords**; seq3: **272/272**; seq4–122: **119 × 400/400**.
- No GFX dump is unresolved, short, or truncated under the 2048 full-line tracer.

The generic readback comparator invoked with third argument 800 rejected this different physical layout with `A: expected 121 successful CS, got 122`; this is preserved in `vktest-clear-attachments-dev-generic-comparator.log`. It was not bypassed by dropping the extra submission.

Instead, `compare-clear-attachments-full.py` is saved as a standalone strict comparator with usage text. It requires exactly 122 successful CS, the recorded 800/272/400 sizes, matching declared and serialized word counts, and explicitly rejects truncated data. It compares every physical GFX in sequence order, including seq2+seq3, without assigning an unsupported semantic explanation to the extra CS.

Result in `vktest-clear-attachments-dev-ib-diff.log`:

- 121 complete physical GFX pairs corresponding to 120 application frames; 0 truncated.
- **48,672 equal dwords / 0 different**.
- A/B SHA-256 both `834addd09674cf704413cde34ca3f68bbec9a263b5903ee2ed54042254954df5`.
- `result=IDENTICAL including split first-frame seq2+seq3`.

## Requested dev smoke

All were run serially with the ClearAttachments dev ICD and passed with exit 0:

- `vktest-clear`: PASS.
- `vktest-copy-texture`: PASS.
- Direct GPU CopyImage A: PASS, exact 32-byte dst result and guards.
- Tight readback A: PASS, raw `00 7f 00 ff`.
- Padded readback A: PASS, exact 32-byte result and guards.

Logs/statuses use `vktest-clear-ca-dev-smoke.*`, `vktest-copy-texture-ca-dev-smoke.*`, `vktest-copy-image-ca-dev-smoke.*`, `vktest-tight-ca-dev-smoke.*`, and `vktest-padded-ca-dev-smoke.*`; raw smoke outputs are retained.

## Limitations and artifact index

Only color attachment0, base layer0/layer count1, active-render-pass clears are implemented. Depth/stencil, no-active-framebuffer calls, other attachment indices/layers, and invalid regions are rejected. Multiple attachment/rect loops are implemented but not separately pixel-tested beyond one of each. State restoration is explicit; the non-full-mask test isolates clear-mask bypass, not a separate immediate post-clear draw mask test. No performance or full Vulkan-conformance claim is made.

Under `/home/houge/infra/research/lg110`:

- Driver: `vkgl-clear-attachments-dev.patch`, base source, empty dev build log, research dev shared library/JSON.
- Test: `vktest-clear-attachments.c`, binary, `.frag`, `.frag.spv`, strict C and shader build logs.
- A/B: per-region raw buffers, stdout/exit/full traces, hexdump log.
- IB: `compare-clear-attachments-full.py`, strict result log, and generic-comparator rejection log.
- Five smoke logs/statuses and raw results named above.
- This report: `vkgl-clear-attachments-dev.md`.
