# LG110 gradient actual-sampling control

## Variant scope and shader validation

- Independent source/binary: `vktest-gradient-sample.c` / `vktest-gradient-sample`.
- Based on `vktest-gradient-descriptor.c`; retains the original `grad.vert.spv`, geometry, render pass, viewport/scissor, clear sequence, `vkCmdDraw(3,1,0,0)`, valid binding-0 descriptor creation/update/bind, and 120-frame loop.
- Independent fragment source/SPIR-V: `vktest-gradient-sample.frag` / `vktest-gradient-sample.frag.spv`.
- The fragment shader executes one fixed-UV `texture()` per invocation and computes `outColor.rgb = original_gradient.rgb + sampled.rgb * 0.000001`; alpha remains 1.0.
- `glslangValidator -V -H` output in `vktest-gradient-sample-shader-build.log` contains exactly one `ImageSampleImplicitLod`. Its result has the observed dependency chain `VectorShuffle -> Load sampled -> VectorTimesScalar -> FAdd -> CompositeConstruct -> Store outColor`.
- Final C build used `gcc -std=c11 -O2 -Wall -Wextra -Werror`; `vktest-gradient-sample-build.log` is empty.

## Capture and exact completeness

- Installed `vkgl-loonggpu 0.1.15-1`, installed `/usr/lib/vkgl/libvkgl.so`, active `/run/user/1000/wayland-0`.
- Ran from `/home/houge/infra/vkgl` for the unchanged relative vertex-SPIR-V path; the new fragment SPIR-V is loaded from its absolute research path.
- Existing tracer with `LG110_TRACE_MAX=512` and `LG110_TRACE_IB_DWORDS=512`; no concurrent GPU test.
- Printed `PASS vktest-gradient-sample` and exited 0. All 121 CS calls returned 0.

| IP type | Count | IB bytes | Dumped dwords | Complete | Truncated | Unresolved |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `ip_type=2` XDMA, sequence 1 | 1 | 64 | 16 | 1 | 0 | 0 |
| `ip_type=0` GFX, sequences 2–121 | 120 | 1600 | 400 | 120 | 0 | 0 |
| Total | 121 | — | — | 121 | 0 | 0 |

The 512-dword cap was sufficient and was not changed.

## Length and observed header layout

Offsets are zero-based. No semantic names are assigned to `0x88` or `0x89`.

| Workload | GFX dwords | `0x00010088` positions | `0x00810089` positions | NOP positions |
| --- | ---: | --- | --- | --- |
| Sample variant, all 120 GFX | 400 | 0, 132, 264, 396 | 2, 134, 266 | 398–399 |
| Texture, all 120 GFX | 400 | 0, 132, 264, 396 | 2, 134, 266 | 398–399 |
| Original gradient / descriptor-only, remaining 119 GFX | 272 | 0, 262 | 2, 132 | 264–271 |

Thus actual sampling **does reproduce** texture's extra `0x00810089` at 266 and its 400-dword layout, unlike the descriptor-only control.

## Frame-matched absolute comparison

All comparisons use sequences 3–121 in each log: 119 steady-state frame-matched pairs. Each equality class below holds for every pair; there are no mixed-class offsets.

| Sample variant compared with | Compared absolute range | Equal | Different | Mixed |
| --- | --- | ---: | ---: | ---: |
| Original gradient 0.1.15 | 0–271 (common 272 dwords) | 179 | 93 | 0 |
| Descriptor-only 0.1.15 | 0–271 (common 272 dwords) | 181 | 91 | 0 |
| Texture 0.1.15 | 0–399 (all 400 dwords) | 386 | 14 | 0 |

Sample words 272–399 have no counterpart in original gradient or descriptor-only and are not counted in their common-range rows.

Against texture, the 14 consistently different absolute offsets are `110,242,268,336-337,346,350,352,361,364,370,372-374`. Regional totals are:

| Sample vs. texture region | Equal | Different | Mixed |
| --- | ---: | ---: | ---: |
| 0–131 | 131 | 1 | 0 |
| 132–263 | 131 | 1 | 0 |
| 264–395 | 120 | 12 | 0 |
| 396–399 | 4 | 0 | 0 |

## `0x00810089` block alignment

Each compared block is 130 dwords beginning with `0x00810089`.

### Sample against original gradient and descriptor-only

Original gradient and descriptor-only both have their second block at 132–261. Sample has its second at 134–263 and third at 266–395.

| Sample block | Compared with second block of | Equal | Different | Mixed |
| --- | --- | ---: | ---: | ---: |
| Second, 134–263 | Original gradient | 97 | 33 | 0 |
| Third, 266–395 | Original gradient | **119** | **11** | **0** |
| Second, 134–263 | Descriptor-only | 97 | 33 | 0 |
| Third, 266–395 | Descriptor-only | **119** | **11** | **0** |

The best alignment is therefore sample's third block against either baseline's second block. Its 11 differing sample offsets are `338,346,348,350,352,360-362,370,372,374`, paired respectively with baseline offsets `204,212,214,216,218,226-228,236,238,240`.

### Sample against texture

| Sample block | Texture block | Equal | Different | Mixed |
| --- | --- | ---: | ---: | ---: |
| Second, 134–263 | Second, 134–263 | **129** | **1** | **0** |
| Third, 266–395 | Third, 266–395 | 118 | 12 | 0 |
| Second, 134–263 | Third, 266–395 | 101 | 29 | 0 |
| Third, 266–395 | Second, 134–263 | 98 | 32 | 0 |

The best sample/texture block alignment is corresponding second blocks: only sample/texture word 242 differs. Corresponding third blocks differ at `268,336-337,346,350,352,361,364,370,372-374`.

## Within-sample varying offsets and limitations

Across its 119 steady-state GFX IBs, sample has 389 invariant offsets and 11 varying offsets: two values at 54 and 318; 60 values at 87; and 119 values at `110,242,346,350,352,370,372,374`.

Limitations: these results cover only the installed vkgl 0.1.15 paths and this shader/descriptor combination. SPIR-V inspection verifies a live sampling instruction and output dependency; PASS verifies test execution, but no pixel readback or independent hardware-level sampling observation was added. The tracer establishes IB contents and completeness only. No unknown opcode or field semantics are inferred.

Bounded conclusion: with geometry and draw held constant, actual fragment sampling is sufficient in this variant to reproduce texture's extra `0x89` and 400-dword layout; descriptor binding without sampling did not do so.
