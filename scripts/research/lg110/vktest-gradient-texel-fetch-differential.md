# LG110 gradient texelFetch control: pipeline-creation failure

## Variant scope and independent artifacts

- Source/binary: `vktest-gradient-texel-fetch.c` / `vktest-gradient-texel-fetch`.
- Based on `vktest-gradient-sample.c`; retains original `grad.vert.spv`, geometry, render pass, viewport/scissor, clear, `vkCmdDraw(3,1,0,0)`, the same valid binding-0 combined-image-sampler descriptor, and the 120-frame loop in source.
- Independent fragment source/SPIR-V: `vktest-gradient-texel-fetch.frag` / `vktest-gradient-texel-fetch.frag.spv`.
- The only shader-operation substitution is `texelFetch(tex, ivec2(0,0), 0)` in place of `texture()`. Fetched RGB is multiplied by `0.000001` and added to the original gradient RGB; alpha remains 1.0.

## Build and SPIR-V proof

- C build used `gcc -std=c11 -O2 -Wall -Wextra -Werror`; `vktest-gradient-texel-fetch-build.log` is empty.
- `glslangValidator -V -H` output in `vktest-gradient-texel-fetch-shader-build.log` contains exactly **one `ImageFetch ... Lod 0`** and **zero `ImageSampleImplicitLod`**.
- The observed output dependency is `ImageFetch -> VectorShuffle -> Load fetched -> VectorTimesScalar -> FAdd -> CompositeConstruct -> Store outColor`, so the fetched result is live in the shader output.

## Installed-vkgl run result

Environment was installed `vkgl-loonggpu 0.1.15-1`, installed `/usr/lib/vkgl/libvkgl.so`, active `/run/user/1000/wayland-0`, the existing tracer with `LG110_TRACE_MAX=512` and `LG110_TRACE_IB_DWORDS=512`, and no concurrent GPU test.

The test did **not** pass and did **not** complete 120 frames:

- `vkCreateGraphicsPipelines` returned `-3` at variant source line 185; process exit status is **1**.
- Stdout stops after swapchain-image enumeration; there is no PASS line.
- Stderr reports GLSL compilation failure: `no function with name 'texelFetch2D'`, followed by a type mismatch and uninitialized `fetched` warning in the translated shader.
- Read-only inspection of `/home/houge/infra/vkgl/vkgl.cpp` lines 2506–2510 shows the installed source path configuring SPIRV-Cross with `opts.version = 120` and `opts.es = false`. The emitted failing shader is correspondingly marked `#version 120` and calls `texelFetch2D`.
- No research-only Vulkan source change can alter that installed translator setting. The installed driver was not modified.

Because pipeline creation fails before command-buffer recording/submission, the trace contains **0 LG110CS, 0 IB descriptors, and 0 IB dumps**. Exact submission counts are therefore:

| Item | Count |
| --- | ---: |
| CS | 0 |
| XDMA submissions | 0 |
| GFX submissions | 0 |
| Complete IB dumps | 0 |
| Truncated IB dumps | 0 (no IBs exist) |
| Unresolved dump records | 0 (no dump was attempted) |
| Completed frames | 0 |

The run records are named `vktest-gradient-texel-fetch-failed.log`, `.stdout.log`, and `.exit` to avoid presenting this as a full capture.

## Differential status and limitations

No 119-group steady-state frame-matched comparison against sample, texture, original gradient, or descriptor-only is possible: this variant produced no GFX IBs. Accordingly, there are no absolute or block-alignment equal/different/mixed counts to report.

Whether a successfully executing texelFetch variant would produce the 400-dword layout or third `0x00810089` remains **unresolved on installed vkgl 0.1.15**. The observed pipeline-compiler rejection is not evidence that texelFetch would omit that layout.

This result covers only the installed translator/compiler path. SPIR-V inspection proves the live fetch instruction and output dependency, but no shader pipeline or GPU execution was reached. No unknown LG110 opcode or field semantics are inferred. No installed driver or public repository was changed.
