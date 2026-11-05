# LG110 full-IB differential: clear vs. gradient

## Capture and exact counts

- Date: 2026-11-05. Installed package: `vkgl-loonggpu 0.1.14-1`; device reported as `LoongGPU LG110 (vkgl GLES2)`.
- Working directory: `/home/houge/infra/vkgl` (required by gradient's relative SPIR-V paths). ICD: `/home/houge/infra/vkgl/vkgl_icd.json`, which selects installed `/usr/lib/vkgl/libvkgl.so`.
- Active Wayland: `XDG_RUNTIME_DIR=/run/user/1000`, `WAYLAND_DISPLAY=wayland-0`.
- Both captures used `LG110_TRACE_MAX=512` and `LG110_TRACE_IB_DWORDS=512`; each test ran its full 120-frame loop, printed PASS, and exited 0.
- Every CS below returned 0. Word offsets are zero-based indices into each dumped IB.

| Test | CS / IB count | IP types and complete dumps | Dump failures | Truncated dumps |
| --- | ---: | --- | ---: | ---: |
| vktest-clear | 120 | 120 `ip_type=0`, 576 bytes, 144/144 dwords | 0 | 0 |
| vktest-gradient | 121 | 1 `ip_type=2`, 64 bytes, 16/16 dwords; 120 `ip_type=0`, 1088 bytes, 272/272 dwords | 0 | 0 |

The gradient XDMA submission is sequence 1; its GFX submissions are sequences 2–121. Thus the bounded 512-submission setting includes all GFX traffic after XDMA.

## Observed header words / opcodes

Only the already-defined `NOP` (`0x80`) and `XDMA_COPY` (`0xc0`) names are used. Opcodes `0x88` and `0x89` remain unknown.

| IB set | `0x00010088` (op `0x88`, n=1) | `0x00810089` (op `0x89`, n=129) | `0x00000080` (NOP) |
| --- | --- | --- | --- |
| Clear GFX, all 120 | words 0, 132 | word 2 | words 134–143 |
| Gradient GFX, first (sequence 2) | words 0, 132, 264 | words 2, 134 | words 266–271 |
| Gradient GFX, remaining 119 | words 0, 262 | words 2, 132 | words 264–271 |

Gradient sequence 1 instead starts with `0x010800c0` (observed op `0xc0`, n=264), and has NOP words at offsets 9–15.

## First-GFX differential

Clear sequence 1 and gradient sequence 2 have identical data at words 0–133. Within the clear IB's 144-word range, only these words differ:

| Word offset | Clear sequence 1 | Gradient sequence 2 |
| ---: | ---: | ---: |
| 134 | `0x00000080` | `0x00810089` |
| 135 | `0x00000080` | `0x0000000f` |
| 136 | `0x00000080` | `0x40000400` |
| 137 | `0x00000080` | `0x00000000` |
| 138 | `0x00000080` | `0x00000003` |
| 139 | `0x00000080` | `0x00000000` |
| 140 | `0x00000080` | `0x00000001` |
| 141 | `0x00000080` | `0x00000003` |
| 142–143 | `0x00000080` | `0x00000000` |

Gradient has additional words 144–271, with no clear counterpart. No meaning is assigned here to the additional block or to either unknown opcode.

## Observed varying word offsets

Across all 120 clear GFX IBs, only three positions vary:

- Word 54: two alternating values, `0x02300001` and `0x02700001`.
- Word 87: 60 values; the sequence begins `0x00000000`, `0x3c888889`, `0x3d088889`, matching the test's changing red clear-value bit pattern.
- Word 110: 120 values; it begins `0x010c2000`, `0x010c4800`, `0x010c7000` and ends at `0x011eb800`.

Across all 120 gradient GFX IBs, 99 positions vary, grouped only by their observed number of distinct values:

- Two values (90 offsets): `25-26,28-29,35-36,54,120-127,132-136,138-142,144,154-161,163-168,172-176,183,185-187,202-207,209-211,214,220-221,223,225-235,242-243,250-260,262-265`.
- Three values: word 184 (`0x00000000` on the first GFX IB, then alternating `0x02700001` / `0x02300001`).
- Sixty values: word 87, with the same observed clear-red progression as clear.
- 120 values: words `110,212,216,218,236,238,240`.

The first-gradient-only header layout accounts for some two-value positions; the table above deliberately does not infer why the other positions alternate or increment.

## Unresolved data and next hypothesis

There are no unresolved `not_dumped` records, descriptor read failures, short chunks, or truncated IBs in either final log.

Concrete next hypothesis to test: the gradient-only region anchored by the second observed `0x00810089` is the most specific observed delimiter between clear-only and drawing traffic. Capture `vktest-texture` next under the same bounds and compare its steady-state GFX words 132–263 against gradient; then vary only shader or draw parameters to distinguish invariant words from workload-dependent words. This is a comparison plan, not a semantic assignment.
