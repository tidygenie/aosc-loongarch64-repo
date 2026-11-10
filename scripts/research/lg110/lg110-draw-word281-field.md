# LG110 native indexed word281 single-dword negative causality (2026-11-10)

This extends the known 672-dword vendor GSPKT draw-stream experiment on the
installed vkgl 0.1.32. It is not a complete DRAW decoder, native Vulkan driver,
replay system, or performance result.

## Reason and guard

Word281 was 0 in the nonindexed stream and 1 in the indexed UINT16 stream, but
remained 1 for firstInstance=0/1 and for UINT16/UINT32 indices. Those correlations
rule out a simple firstInstance or index-width assignment but do not identify the
field.

The public tracer adds opt-in `LG110_MUTATE_DRAW_FIELD=word281` as mode 11. It
requires the exact indexed UINT16 672-dword signature, word270=3, word268=
`0x40000402`, word276=`0xffffffff`, a nonzero valid index VA, word281=1, a
writable tracked mapping and post-store readback. Only word281 changes `1→0`.
The local signature snapshot grows from 281 to 282 dwords so strict compilation
covers index 281; the actual IB and tracked-mapping requirements remain 672
dwords / 2688 bytes. Existing mode numbers are unchanged.

## Exact results

All five processes completed normally with their expected pixels. The indexed
captures each contain two 16-dword XDMA IBs and one 672-dword GFX IB.

| Case | Mutation | Complete GFX difference from matching base | Raw BGRA center |
|---|---|---|---|
| indexed UINT16 base | none | reference | `663300ff` |
| active word281 | word281 `1→0`, observed 0, patched=1 | **only word281 `1→0`** | `663300ff` |
| rejected nonindexed input | zero records | no mutation | `663300ff` |
| indexed UINT32 regression base | none | reference | `007f00ff` |
| index32-to16 regression | word268 `0x40000404→0x40000402` | **only word268** | `663300ff` |

Both XDMA IBs are byte-identical in the active/base comparison and in the paired
index-width regression. A parser asserted all dword counts and every array
element; output is `word281-summary.json` in the external evidence directory.

Thus word281 alone is **not sufficient** to change this indexed output. It must
remain unassigned. This does not prove that the field is unused, and does not
weaken the established word268 index-width causality.

## Build and reproduction

Installed library SHA-256:
`a84c177852cac3f405c1c155d75affcf4ef6d3f7e33dcceed42b6dfb73e041b3`.
Tracer SHA-256:
`2f1044fb9030ea5d0ef965c166eaa520fb7e7e59e3ecc396521b7ab51755b411`.
Instance-test SHA-256:
`e34e3cf2c80b2eff7e383826e88cbf3795d47b68b2c1b5caa0c1b121b3bca6e5`.
An independent rebuild of the tracer and draw-count test was byte-identical.

```sh
scripts/tests/vkgl/build-0.1.29-tests.sh /tmp/vkgl-tests
scripts/research/lg110/build-draw-count-mutation.sh /tmp/lg110-draw
cd /tmp/vkgl-tests/instance
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export LD_PRELOAD=/tmp/lg110-draw/lg110-draw-count-mutation.so
export LG110_TRACE_MAX=4 LG110_TRACE_AUX_MAX=64 LG110_TRACE_IB_DWORDS=2048
export LG110_MUTATE_DRAW_FIELD=word281
LG110_TEXTURE_VARIANT=A LG110_DRAW_VERTEX_COUNT=3 LG110_DRAW_INSTANCE_COUNT=1 \
LG110_DRAW_FIRST_INSTANCE=0 LG110_DRAW_INDEXED=1 LG110_DRAW_INDIRECT=0 \
LG110_INDEX_TYPE=16 LG110_EXPECT_DRAW_COUNT=2 LG110_FRAME_LIMIT=1 \
LG110_READBACK_PIXEL=/tmp/word281.rgba /tmp/vkgl-tests/vktest-draw-instance
```

Run serially and only on the exact guarded stream. External raw evidence:
`/home/houge/infra/gpu/evidence/lg110-word281-20261110/`; binaries and full dumps
are intentionally not committed to Git. This one-word result does not establish
shader ISA, arbitrary packet safety, native replay, tiling, or performance.
