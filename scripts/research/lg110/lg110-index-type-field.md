# LG110 native UINT16/UINT32 word268 causality (2026-11-07)

This follows the passive indexed differential and uses exact public vkgl 0.1.28.
It establishes index-width causality for word268 in the known 672-dword GFX
stream; it is not a complete DRAW decoder, native Vulkan implementation, replay,
or shader/address proof.

## Passive type differential

The public test allocates indices `{0,1,2}` as either six-byte UINT16 or
12-byte UINT32, then records an indexed draw with one instance and
firstInstance=1. Both correct runs produce raw gradient `007f00ff`.

- Both captures contain two 16-dword XDMA IBs and one 672-dword GFX IB.
- The index-upload XDMA changes word0 `0x010814c0→0x010800c0` and word1 `6→3`;
  these raw observations are not assigned byte-count semantics.
- The complete GFX diff is exactly word268 `0x40000402→0x40000404`.
- Word281 remains 1 in both, disproving the earlier hypothesis that it might be
  the index-width field. Word280 remains the valid index GEM VA.

## Active one-dword proof

`LG110_MUTATE_DRAW_FIELD=index32-to16` requires the complete indexed UINT32
signature, word270=3, word276=`0xffffffff`, word268=`0x40000404`, a nonzero
index VA, a writable tracked mapping and post-store readback. It changes only
word268 to the observed UINT16 value `0x40000402` and forwards the original CS.

Both XDMA IBs remain byte-identical; the GFX diff is exactly word268
`0x40000404→0x40000402`. Raw becomes `663300ff`, byte-identical to the broken
0.1.27 UINT32-as-UINT16 output. The valid 12-byte allocation remains present, so
this reverse mutation does not require reading beyond that allocation.

Requesting the same mutation on UINT16 input is rejected with no mutation record
and zero IB differences; raw remains gradient `007f00ff`. All eight earlier
firstVertex/count/topology/word276 regressions also pass with the extended
public tracer. Word281 remains unassigned.

## Evidence and reproduction

Raw evidence:
`draw-index-type-public-{native-u16,native-u32,active-index32-to16,rejected-index16}.{rgba,stdout.log,trace.log}`.

```sh
scripts/tests/vkgl/build-0.1.28-tests.sh /tmp/vkgl-0.1.28-tests
scripts/research/lg110/build-draw-count-mutation.sh /tmp/lg110-draw
cd /tmp/vkgl-0.1.28-tests/instance
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export LD_PRELOAD=/tmp/lg110-draw/lg110-draw-count-mutation.so
export LG110_TRACE_MAX=4 LG110_TRACE_AUX_MAX=64 LG110_TRACE_IB_DWORDS=2048
export LG110_MUTATE_DRAW_FIELD=index32-to16
LG110_TEXTURE_VARIANT=A LG110_DRAW_VERTEX_COUNT=3 LG110_DRAW_INSTANCE_COUNT=1 \
LG110_DRAW_FIRST_INSTANCE=1 LG110_DRAW_INDEXED=1 LG110_DRAW_INDIRECT=0 \
LG110_INDEX_TYPE=32 LG110_EXPECT_DRAW_COUNT=2 LG110_FRAME_LIMIT=1 \
LG110_READBACK_PIXEL=/tmp/index32-to16.rgba \
  /tmp/vkgl-0.1.28-tests/vktest-draw-instance
```

Run serially and only on the exact guarded stream. This proves the 16/32 index
encoding effect in this workload, not arbitrary packet modification safety.
