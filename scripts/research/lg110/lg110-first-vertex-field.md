# LG110 firstVertex / word274 causality (2026-11-07)

This extends the known 672-dword vendor GSPKT draw-stream experiment. It proves
one first-vertex causal field in this exact stream; it is not a complete DRAW
packet decoder, native Vulkan driver, vertex-address/shader-ISA proof, replay
system, or performance result.

## Safe passive differential

The original three-vertex shader would index a nonexistent fourth vertex with
`vertexCount=3, firstVertex=1`. The public `grad4.vert` instead defines four
vertices: indices 0,1,2 form the original center-covering triangle; indices
1,2,3 form a complementary triangle that leaves the center at the clear color.
All shader indices remain in bounds.

With installed vkgl 0.1.26, the same binary, fragment shader, draw count and
topology changed only firstVertex:

| firstVertex | Raw BGRA center | GFX differences from first=0 |
|---:|---|---|
| 0 | `007f00ff` | reference |
| 1 | `663300ff` | word274 `0→1`, word275 `0→1`, word276 `2→3` |

The 16-dword XDMA IBs are identical; both GFX IBs contain exactly 672 dwords.
Together with the earlier count experiment (`draw3→draw2`: word276 `2→1`),
word276 correlates with `firstVertex + vertexCount - 1`. Correlation alone does
not assign its semantics.

## Active one-dword separation

The tracer remains passive unless one exact mode is selected. Every active run
requires the fixed GFX signature, original values, writable tracked mapping,
post-store readback and the unchanged original CS ioctl.

- `LG110_MUTATE_DRAW_FIELD=word274`: only word274 changes `0→1`; word275 remains
  0 and word276 remains 2. Raw becomes `663300ff`, byte-identical to passive
  first=1. Thus word274 causally controls enough of the first-vertex state to
  select the other triangle in this stream.
- `LG110_MUTATE_DRAW_FIELD=word275`: only word275 changes `0→1`; raw remains
  `007f00ff`. Word275 alone is not sufficient for the observed output change
  and remains unassigned.
- The word276 regression changes only `2→1`; raw also remains `007f00ff`, so the
  correlated last-index-like value alone is not sufficient either.
- Requesting word274 against an already-first1 stream is rejected with zero
  mutation records and zero IB changes.
- Existing count and topology regressions still change only word270 and word268
  respectively and produce the expected clear pixel.

Evidence:
`draw-first-public-{first0,first1,active274,active275,rejected274-first1,regression-word276,regression-count,regression-topology}.*`.
Each case contains raw pixel, stdout and full trace. The initial deliberately
wrong expectation for active274 is not needed for the causal proof; the final
public run uses the correct clear expectation and passes normally.

## Reproduce

```sh
scripts/research/lg110/build-draw-count-mutation.sh /tmp/lg110-draw
cd /tmp/lg110-draw/first-vertex
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export LD_PRELOAD=/tmp/lg110-draw/lg110-draw-count-mutation.so
export LG110_TRACE_MAX=4 LG110_TRACE_AUX_MAX=64 LG110_TRACE_IB_DWORDS=2048
LG110_MUTATE_DRAW_FIELD=word274 LG110_TEXTURE_VARIANT=A \
LG110_DRAW_VERTEX_COUNT=3 LG110_DRAW_FIRST_VERTEX=0 LG110_EXPECT_DRAW_COUNT=2 \
LG110_DRAW_TOPOLOGY=triangle LG110_FRAME_LIMIT=1 \
LG110_READBACK_PIXEL=/tmp/first-active.rgba \
  /tmp/lg110-draw/vktest-draw-count-readback
```

Run serially. Word274's proof is limited to this stream and output. Do not infer
vertex buffer addresses, shader ISA, tiling, replay, or generalized packet
semantics. Word273, word275 and word276 remain unassigned.
