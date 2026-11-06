# LG110 draw topology causality and vkgl 0.1.21 (2026-11-06)

This is a six-mode differential plus a single-dword causal experiment in one
known 672-dword GFX stream. It is not a complete DRAW decoder, native Vulkan
implementation, replay system, or performance claim.

## Negative control exposed a vkgl bug

With installed vkgl 0.1.20 (`d9aad6de…`), requesting point versus triangle
topology produced identical complete XDMA/GFX IBs and the same gradient pixel
`007f00ff`; the point test correctly failed its clear-pixel expectation. The
cause was in vkgl: pipeline topology was recorded, but both `glDrawArrays` and
`glDrawElements` were hardcoded to `GL_TRIANGLES`.

Evidence: `draw-topology-v0120-{triangle,point}.*`.

vkgl 0.1.21 maps the six basic Vulkan topologies to their actual GL modes and
rejects adjacency/PATCH pipeline creation with `VK_ERROR_FEATURE_NOT_PRESENT`
instead of silently drawing triangles.

## Passive native differential after the fix

The same public binary, shaders, texture variant A, vertexCount=3 and one frame
were used for every mode. Each run has an identical 16-dword XDMA IB and a
complete 672-dword GFX IB. Against triangle-list, the only GFX difference is
word268:

| Vulkan topology | GL mode | word268 | Raw BGRA center |
|---|---:|---:|---|
| point | 0 | `0x40000000` | `663300ff` (clear) |
| line | 1 | `0x40000100` | `663300ff` (clear) |
| line-strip | 3 | `0x40000300` | `663300ff` (clear) |
| triangle | 4 | `0x40000400` | `007f00ff` (gradient) |
| triangle-strip | 5 | `0x40000500` | `007f00ff` (gradient) |
| triangle-fan | 6 | `0x40000600` | `007f00ff` (gradient) |

For these six values the mode-dependent bits match `GL_mode << 8`; the high
`0x40000000` bit and all other words remain unchanged. No wider mask or unknown
field is inferred. In particular, word273 remains 3 in every mode and is not a
topology field. The earlier draw-count report's descriptive label “topology
word273” is corrected to “unassigned word273”; its count mutation and evidence
are unaffected.

Evidence: `draw-topology-{point,line,line-strip,triangle,triangle-strip,triangle-fan}.*`.

## Active single-dword proof and guards

The final `lg110-draw-count-mutation.c` supports two independent opt-in fields:

- `LG110_MUTATE_DRAW_FIELD=count`: word270 `3→2` only;
- `LG110_MUTATE_DRAW_FIELD=topology`: word268
  `0x40000400→0x40000000` only.

Both require the exact 672-dword GFX ring0 signature, complete writable mapping,
selected and nonselected old values, one mutation per process, and post-write
readback. The original CS ioctl and all BO/VA layout remain unchanged.

Starting with triangle-list, active topology changed only word268 and produced
raw `663300ff`, byte-identical to passive point. The count regression still
changed only word270 and produced the same clear pixel. Running topology against
an already-point stream was rejected: zero mutation records and zero XDMA/GFX
differences. Adjacency and PATCH both returned result -8
(`VK_ERROR_FEATURE_NOT_PRESENT`) before any draw/readback.

Evidence:

- `draw-topology-active-point.*`
- `draw-topology-regression-count.*`
- `draw-topology-rejected-point.*`
- `draw-topology-unsupported-{adjacency,patch}.*`

All results were regenerated from `build-draw-count-mutation.sh` using installed
vkgl 0.1.21, not a development ICD.

## Reproduce

```sh
scripts/research/lg110/build-draw-count-mutation.sh /tmp/lg110-draw
cd scripts/research/lg110
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export LD_PRELOAD=/tmp/lg110-draw/lg110-draw-count-mutation.so
export LG110_TRACE_MAX=4 LG110_TRACE_AUX_MAX=64 LG110_TRACE_IB_DWORDS=2048
LG110_MUTATE_DRAW_FIELD=topology LG110_TEXTURE_VARIANT=A \
LG110_DRAW_VERTEX_COUNT=3 LG110_DRAW_TOPOLOGY=triangle \
LG110_EXPECT_TOPOLOGY=point LG110_FRAME_LIMIT=1 \
LG110_READBACK_PIXEL=/tmp/topology-active.rgba \
  /tmp/lg110-draw/vktest-draw-count-readback
```

Run serially. Do not mutate neighboring words by analogy or claim vertex
addressing, shader ISA, tiling, replay, or general DRAW semantics from this
field.
