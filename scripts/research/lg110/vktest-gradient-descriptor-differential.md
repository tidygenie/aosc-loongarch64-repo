# LG110 descriptor-only control: gradient with an unused bound descriptor

## Variant scope and build

- Independent source/binary: `vktest-gradient-descriptor.c` / `vktest-gradient-descriptor` in `/home/houge/infra/research/lg110`.
- The variant retains the original `grad.vert.spv` and `grad.frag.spv`, render pass, viewport/scissor, `vkCmdDraw(3,1,0,0)`, clear sequence, and 120-frame loop.
- It adds the texture test's binding-0 `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` layout, a 64x64 image/memory/view/sampler, descriptor pool/set allocation, `vkUpdateDescriptorSets`, and `vkCmdBindDescriptorSets` before viewport/scissor/draw.
- Neither gradient shader samples the descriptor. The image exists only to make the descriptor update valid.
- Minimal test-source build fixes only: `_DEFAULT_SOURCE` exposes the existing `usleep` call under strict C11, and designated XDG listener initializers accommodate the current protocol structure. The descriptor set layout is initialized before its address is used in the pipeline layout.
- Final build used `gcc -std=c11 -O2 -Wall -Wextra -Werror`; `vktest-gradient-descriptor-build.log` is empty.

## Capture and exact completeness

- Installed `vkgl-loonggpu 0.1.15-1`, installed `/usr/lib/vkgl/libvkgl.so`, active `/run/user/1000/wayland-0`.
- Ran from `/home/houge/infra/vkgl` so the unchanged relative gradient SPIR-V paths resolve correctly.
- Existing tracer with `LG110_TRACE_MAX=512` and `LG110_TRACE_IB_DWORDS=512`; no concurrent GPU test.
- The variant printed `PASS vktest-gradient-descriptor` and exited 0. All 121 CS calls returned 0.

| IP type | Count | IB bytes | Dumped dwords | Complete | Truncated | Unresolved |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `ip_type=2` XDMA, sequence 1 | 1 | 64 | 16 | 1 | 0 | 0 |
| `ip_type=0` GFX, sequences 2–121 | 120 | 1088 | 272 | 120 | 0 | 0 |
| Total | 121 | — | — | 121 | 0 | 0 |

The 512-dword cap was sufficient and was not changed.

## Observed `0x88` / `0x89` positions

Offsets are zero-based. No semantics are assigned to the unknown opcodes.

| GFX set | `0x00010088` positions | `0x00810089` positions | NOP positions |
| --- | --- | --- | --- |
| Descriptor variant, first GFX (sequence 2) | 0, 132, 264 | 2, 134 | 266–271 |
| Descriptor variant, remaining 119 GFX | 0, 262 | 2, 132 | 264–271 |
| Original gradient 0.1.15, same sets | identical | identical | identical |
| Texture 0.1.15, every GFX | 0, 132, 264, 396 | 2, 134, 266 | 398–399 |

Thus descriptor-only retains the original gradient's 272-dword size and block layout. It does **not** produce texture's additional `0x00810089` block at 266 or its 400-dword length.

The variant's XDMA IB differs from original gradient only at word 4 in this capture: `0x026c0000` vs. `0x022bc000`.

## Frame-matched descriptor variant vs. original gradient

Steady-state means sequences 3–121 from each capture: 119 frame-matched pairs, excluding each first GFX submission.

- Across all 272 GFX words: **268 equal / 4 different / 0 mixed**, where each class holds across all 119 pairs.
- The four consistently different offsets are **74, 98, 204, 228**.
- First steady-state values at those offsets are:

| Word | Descriptor variant | Original gradient 0.1.15 |
| ---: | ---: | ---: |
| 74 | `0x022bc000` | `0x022b4000` |
| 98 | `0x022c0000` | `0x022b8000` |
| 204 | `0x022c4000` | `0x022fc000` |
| 228 | `0x022c8000` | `0x026c0000` |

The variant's within-workload classes are also unchanged from original gradient: 262 invariant offsets; two values at 54 and 184; 60 values at 87; and 119 values at 110, 212, 216, 218, 236, 238, 240.

## Frame-matched texture comparison and alignment

| Comparison | Equal | Different | Mixed |
| --- | ---: | ---: | ---: |
| Texture vs. descriptor variant, absolute words 0–131 | 130 | 2 | 0 |
| Texture vs. descriptor variant, absolute words 132–263 | 51 | 81 | 0 |
| Texture second `0x89` block 134–263 vs. variant second block 132–261 | 97 | 33 | 0 |
| Texture third `0x89` block 266–395 vs. variant second block 132–261 | 114 | 16 | 0 |

The absolute texture/variant differences at words 0–131 are offsets 91 and 110. The 81 differing offsets at 132–263, the 33 second-block offsets, and the 16 third-block-alignment offsets are identical to those cataloged in `vktest-texture-gradient-differential.md` for original gradient.

## Bounded conclusion

For this no-sampling control, descriptor creation/update/bind alone is insufficient to reproduce texture's extra block. It also does not change the best observed alignment: texture's third `0x00810089` block still aligns more closely to the variant's second block (**114/16/0**) than texture's second block does (**97/33/0**). This conclusion is limited to the observed variant and does not assign meaning to any unknown word or opcode.
