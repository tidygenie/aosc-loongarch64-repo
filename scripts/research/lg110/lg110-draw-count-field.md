# LG110 native draw-count field causality (2026-11-06)

This experiment establishes a causal vertex-count field in one known vendor
GSPKT draw stream. It is **not** a complete DRAW packet decoder, native Vulkan
driver, shader/address proof, replay implementation, or performance result.

## Passive differential first

`vktest-draw-count-readback.c` uses the published full-screen gradient shaders
and texture variant A, with one frame and a one-pixel `CopyImageToBuffer`.
Only Vulkan `vkCmdDraw` vertexCount changes from 3 to 2. With GL_TRIANGLES, two
vertices produce no primitive, so the center remains the frame-0 clear color.

Both runs used installed vkgl 0.1.20-1 (library SHA-256 `d9aad6de…`) and the
same public binary. Each produced two successful CS dumps:

1. identical 16-dword XDMA IB;
2. complete 2688-byte / 672-dword GFX IB.

| Passive input | Raw BGRA center | GFX differences from draw3 |
|---|---|---|
| draw3 | `007f00ff` | reference |
| draw2 | `663300ff` | word270 `3→2`, word276 `2→1` |

Thus two words correlate with the Vulkan count change. The experiment did not
assume that either word's name or that both were required.

## Active single-dword experiment

`lg110-draw-count-mutation.c` is passive unless
`LG110_MUTATE_DRAW_FIELD=count`. It requires:

- one GFX ring0 IB with exactly 672 dwords;
- a fully covered, aligned, `PROT_WRITE` tracked mapping;
- a fixed signature covering the known clear state and draw descriptor,
  including nonselected word276=2 and topology word273=3;
- original word270=3 and word276=2;
- at most one mutation per process, immediately before the unchanged original
  CS ioctl, followed by a readback check requiring `observed=2` and `patched=1`.

Starting from draw3, the active run changed **only word270 from 3 to 2**.
Word276 remained 2, deliberately not matching passive draw2's correlated
change. Nevertheless the center became exactly `663300ff`, byte-identical to
passive draw2, and the readback/sentinel test passed.

This proves that word270 causally controls vertex count sufficiently to suppress
the triangle in this stream. It also shows that changing word276 is not required
for this result. Word276 remains unassigned; no pair or last-field mutation is
exposed by the final source.

## Guard and public rebuild

Running the active count request against an input already generated with
draw2 fails the old-value/signature guard: zero `LG110DRAWMUTATE` records, zero
XDMA/GFX IB differences, and raw remains `663300ff`.

`build-draw-count-mutation.sh` builds the tracer and test with strict warnings
and executes no GPU test. A fresh public-relative-path build repeated all four
passive3/passive2/active/guard results with the same exact IB and pixel diffs.

Evidence:

- `draw-count-{passive-3,passive-2,active-word270,rejected-draw2}.*`
- `draw-count-public-{passive-3,passive-2,active,rejected}.*`

Each case contains a four-byte raw pixel, stdout, and complete trace.

## Reproduce

```sh
scripts/research/lg110/build-draw-count-mutation.sh /tmp/lg110-draw
cd scripts/research/lg110  # grad.vert.spv and fragment shader resolve here
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export LD_PRELOAD=/tmp/lg110-draw/lg110-draw-count-mutation.so
export LG110_TRACE_MAX=4 LG110_TRACE_AUX_MAX=64 LG110_TRACE_IB_DWORDS=2048
LG110_MUTATE_DRAW_FIELD=count LG110_TEXTURE_VARIANT=A \
LG110_DRAW_VERTEX_COUNT=3 LG110_EXPECT_DRAW_COUNT=2 LG110_FRAME_LIMIT=1 \
LG110_READBACK_PIXEL=/tmp/draw-active.rgba \
  /tmp/lg110-draw/vktest-draw-count-readback
```

Run GPU experiments serially. Do not bypass guard rejection, mutate word276 by
analogy, or infer vertex addresses, shader ISA, tiling, replay, or general DRAW
semantics from this single field.
