# LG110 native word273 single-dword negative causality (2026-11-10)

This extends the known 672-dword vendor GSPKT draw-stream experiment on the
installed vkgl 0.1.32. It is not a complete DRAW decoder, native Vulkan driver,
replay system, or performance result.

## Reason for the experiment

Word270 already has guarded vertex-count causality. Word273 stayed 3 in the
previous vertex-count, firstVertex and six-topology differentials, but had never
been changed independently. Correlation or stability alone does not assign its
semantics.

The public tracer adds opt-in `LG110_MUTATE_DRAW_FIELD=word273`. It requires the
original nonindexed 672-dword signature, word270=3, word273=3, word276=2, the
known triangle word268, a writable tracked mapping and post-store readback. It
changes only word273 from 3 to 2 immediately before forwarding the unchanged CS
ioctl. Existing indexed mode numbers and guards are unchanged.

## Exact results

All four processes completed normally with the expected pixels. Each capture
contained one 16-dword XDMA IB and one 672-dword GFX IB.

| Case | Mutation record | Complete GFX difference from base | Raw BGRA center |
|---|---|---|---|
| base draw3 | none | reference | `007f00ff` |
| active word273 | word273 `3→2`, observed 2, patched=1 | **only word273 `3→2`** | `007f00ff` |
| rejected on input draw2 | zero records | no mutation | `663300ff` |
| word270 count regression | word270 `3→2`, observed 2, patched=1 | **only word270 `3→2`** | `663300ff` |

The active XDMA is byte-identical to the base XDMA. The count regression also
leaves XDMA unchanged. A parser asserted every dumped dword count and every
array element; the machine-readable assertion output is `word273-summary.json`
in the external evidence directory.

Thus word273 alone is **not sufficient** to suppress the triangle in this
stream. It must remain unassigned: this does not prove that the field is unused,
and it does not negate the established word270 count causality.

## Build and reproduction

Installed library SHA-256:
`a84c177852cac3f405c1c155d75affcf4ef6d3f7e33dcceed42b6dfb73e041b3`.
Tracer SHA-256:
`d2d986cbfbdefdcd56e7c1ca408e3e945b61d561afa357053619fd8a720b5ca8`.
Test SHA-256:
`dd7c3e2c2ad5cd0a9b805bad995aa4e4773bda121db3c1bcb43f26d9c7c25333`.

```sh
scripts/research/lg110/build-draw-count-mutation.sh /tmp/lg110-draw
cd scripts/research/lg110
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export LD_PRELOAD=/tmp/lg110-draw/lg110-draw-count-mutation.so
export LG110_TRACE_MAX=4 LG110_TRACE_AUX_MAX=64 LG110_TRACE_IB_DWORDS=2048
export LG110_MUTATE_DRAW_FIELD=word273
LG110_TEXTURE_VARIANT=A LG110_DRAW_VERTEX_COUNT=3 LG110_EXPECT_DRAW_COUNT=3 \
LG110_FRAME_LIMIT=1 LG110_READBACK_PIXEL=/tmp/word273.rgba \
  /tmp/lg110-draw/vktest-draw-count-readback
```

Run serially and only on the exact guarded stream. External raw evidence:
`/home/houge/infra/gpu/evidence/lg110-word273-20261110/`; binaries and full dumps
are intentionally not committed to Git. Do not generalize this one-word result
to shader ISA, vertex addressing, tiling, native replay, or arbitrary packet
modification safety.
