# LG110 full-IB differential: texture vs. gradient, both vkgl 0.1.15

## Capture and completeness

- Capture date: 2026-11-05. Both primary captures used installed `vkgl-loonggpu 0.1.15-1`.
- Primary texture log: `vktest-texture-full.log`. Primary gradient log: `vktest-gradient-v0.1.15-full.log`, with matching `.stdout.log` and `.exit` files.
- The former texture-0.1.15 vs. gradient-0.1.14 comparison is **superseded** by this same-version comparison. The old `vktest-gradient-full.log` is retained only as a historical artifact and is no longer the comparison input.
- Working directory: `/home/houge/infra/vkgl`; installed ICD: `/home/houge/infra/vkgl/vkgl_icd.json` -> `/usr/lib/vkgl/libvkgl.so`.
- Active Wayland: `XDG_RUNTIME_DIR=/run/user/1000`, `WAYLAND_DISPLAY=wayland-0`.
- Both captures used `LG110_TRACE_MAX=512` and `LG110_TRACE_IB_DWORDS=512`, with no concurrent GPU test.
- Each test completed all 120 frames, printed its PASS line, and exited 0. All 121 CS calls in each capture returned 0; sequence 121 was logged below the 512-submission cap.

| Workload | `ip_type=2` XDMA | `ip_type=0` GFX | Complete | Truncated | Unresolved |
| --- | --- | --- | ---: | ---: | ---: |
| Texture | 1 × 64 bytes, 16/16 dwords | 120 × 1600 bytes, 400/400 dwords each | 121 | 0 | 0 |
| Gradient 0.1.15 recapture | 1 × 64 bytes, 16/16 dwords | 120 × 1088 bytes, 272/272 dwords each | 121 | 0 | 0 |

The 512-dword cap was sufficient for both captures, so it was not raised and the tracer was not rebuilt. As an additional cross-version check, all 121 old-gradient and new-gradient IB data arrays are identical dword-for-dword; this is why every recalculated differential count below is unchanged.

## Observed header words / opcodes

Word offsets are zero-based. `0x88` and `0x89` remain unknown; no semantic names are assigned.

- Every texture GFX IB has `0x00010088` (op `0x88`, n=1) at words **0, 132, 264, 396**.
- Every texture GFX IB has `0x00810089` (op `0x89`, n=129) at words **2, 134, 266**.
- Every texture GFX IB ends with `0x00000003` at word 397 and NOP (`0x00000080`) at words 398–399.
- Texture XDMA sequence 1 starts with `0x010800c0` (op `0xc0`, n=264) and has NOP words at offsets 9–15.
- For comparison, steady-state gradient GFX has `0x00010088` at 0 and 262, `0x00810089` at 2 and 132, and NOPs at 264–271.

## Within-workload invariant/varying offsets

“Steady-state” below means GFX sequences 3–121: 119 observations per workload, excluding each capture's first GFX submission. “Invariant” means invariant only across these observed dumps.

### Texture: 391 invariant and 9 varying offsets

- Two values: words **54, 318**.
- Sixty values: word **87**, following the test's clear-red bit-pattern sequence.
- 119 values: words **110, 242, 346, 350, 352, 374**.
- Invariant offsets: `0-53,55-86,88-109,111-241,243-317,319-345,347-349,351,353-373,375-399`.

### Gradient 0.1.15: 262 invariant and 10 varying offsets

- Two values: words **54, 184**.
- Sixty values: word **87**.
- 119 values: words **110, 212, 216, 218, 236, 238, 240**.
- Invariant offsets: `0-53,55-86,88-109,111-183,185-211,213-215,217,219-235,237,239,241-271`.

## Absolute texture/gradient comparison

The following pairs texture and gradient GFX by equal sequence number, hence equal steady-state frame number.

- Words **0–131**: 128 positions are equal in all 119 pairs; four differ in all 119 pairs: **74, 91, 98, 110**. No position has mixed equality.
- Words **132–263**: 51 positions are equal in all 119 pairs and 81 differ in all 119 pairs; no position has mixed equality.
- The 81 consistently different absolute offsets are: `132-136,138-140,142,154-161,163-168,172-174,176,183-187,202-207,209-212,214-219,221-223,226-236,238-243,250-260,262-263`.
- At the first steady-state frame, texture word 132 is `0x00010088` while gradient word 132 is `0x00810089`; texture word 134 is `0x00810089` while gradient word 134 is `0x40000400`.

## Aligned `0x00810089` block comparison

Each compared block contains 130 dwords beginning with `0x00810089`.

| Texture block | Gradient block | Equal in all 119 pairs | Different in all 119 pairs | Mixed |
| --- | --- | ---: | ---: | ---: |
| Second block: texture 134–263 | Second block: gradient 132–261 | 97 | 33 | 0 |
| Third block: texture 266–395 | Second block: gradient 132–261 | 114 | 16 | 0 |

For texture second block vs. gradient second block, the 33 differing texture offsets are: `136,141,144,156,159,174-175,185-187,204-206,214-223,225,228-230,232,238-242`; the other 97 positions are equal.

The stronger observed alignment is texture's **third** block. Its 114 equal texture offsets are `266-267,269-335,339-345,347,349,351,353-359,363,365-369,371,375-395`. The 16 differing texture offsets, with corresponding gradient offsets in parentheses, are:

`268(134),336(202),337(203),338(204),346(212),348(214),350(216),352(218),360(226),361(227),362(228),364(230),370(236),372(238),373(239),374(240)`.

At the first steady-state frame, the aligned block starts differ as follows: texture `266=0x00810089`, `267=0x0000000f`, `268=0x41010400`; gradient `132=0x00810089`, `133=0x0000000f`, `134=0x40000400`. These are observations only, not opcode or field interpretations.

## Same-version recheck conclusion

The same-version recapture replaces the old cross-version comparison without changing its numerical results. In particular, the texture-second/gradient-second `0x00810089` alignment remains **97 equal / 33 different / 0 mixed**, and the texture-third/gradient-second alignment remains **114 equal / 16 different / 0 mixed** across all 119 frame-matched steady-state pairs. No additional semantic interpretation is made.
