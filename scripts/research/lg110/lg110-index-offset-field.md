# LG110 native firstIndex and signed base-vertex causality (2026-11-07)

This uses exact public vkgl 0.1.29, a four-vertex shader and a six-element index
buffer `{0,1,2,1,2,3}`. It extends the known 672-dword GFX draw stream. It is not
a complete packet decoder, native Vulkan implementation, replay or arbitrary
buffer-safety proof.

## Passive separation

All captures have two 16-dword XDMA IBs and one 672-dword GFX IB. The complete
GFX differences are:

| Comparison | Raw output | GFX-only difference |
|---|---|---|
| base (firstIndex=0, vertexOffset=0) → positive (0,+1) | gradient → clear | word274 `0→1` |
| base → negative cancellation (firstIndex=3, vertexOffset=-1) | gradient → gradient | word269 `0→3`, word274 `0→0xffffffff` |
| base → firstIndex alone (3,0) | gradient → clear | word269 `0→3` |

Both XDMA IBs are unchanged in each comparison. Word268 remains the UINT16 value
`0x40000402`; word270=3, word276=`0xffffffff`, and the index VA remains valid.

## Active single-dword proofs

Each mutation requires the exact indexed UINT16 signature, word270=3,
word276=`0xffffffff`, word268=`0x40000402`, nonzero index VA, writable tracked
mapping, expected old companion fields and post-store readback. Only the listed
word is changed before forwarding the original CS:

| Mode | Mutation | Expected and observed raw | Exact passive equivalent |
|---|---|---|---|
| `first-index` | word269 `0→3` | `663300ff` clear | firstIndex=3 alone |
| `base-vertex` | word274 `0→1` | `663300ff` clear | vertexOffset=+1 |
| `base-neg-to-zero` | word274 `0xffffffff→0`, keeping word269=3 | `663300ff` clear | firstIndex=3 with offset 0 |

For all three, both XDMA IBs remain byte-identical and the GFX diff is exactly
the one target dword. The negative test does not form an invalid negative
effective index: it starts from the valid cancellation case and removes the
negative offset, leaving indices 1,2,3.

`first-index` on the already-positive-offset stream and `base-neg-to-zero` on the
base stream are both rejected with no mutation record and zero IB differences;
their raw outputs remain correct. All eight earlier nonindexed regressions and
the UINT16/UINT32 word268 active/rejection regressions pass on the final tracer.

This establishes firstIndex behavior for word269 and signed base-vertex behavior
for word274 in this workload. The same word274 also had nonindexed firstVertex
causality. No semantics are assigned to word275 or word281.

## Evidence and reproduction

Raw evidence:
`draw-index-offset-public-{native-base,native-positive,native-negative,native-first,active-first-index,active-base-vertex,active-base-neg-to-zero,rejected-first-on-positive,rejected-neg-on-base}.{rgba,stdout.log,trace.log}`.

```sh
scripts/tests/vkgl/build-0.1.29-tests.sh /tmp/vkgl-0.1.29-tests
scripts/research/lg110/build-draw-count-mutation.sh /tmp/lg110-draw
cd /tmp/vkgl-0.1.29-tests/offset
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export LD_PRELOAD=/tmp/lg110-draw/lg110-draw-count-mutation.so
export LG110_TRACE_MAX=4 LG110_TRACE_AUX_MAX=64 LG110_TRACE_IB_DWORDS=2048
export LG110_MUTATE_DRAW_FIELD=base-vertex
LG110_TEXTURE_VARIANT=A LG110_DRAW_VERTEX_COUNT=3 LG110_DRAW_INSTANCE_COUNT=1 \
LG110_DRAW_FIRST_INSTANCE=0 LG110_DRAW_INDEXED=1 LG110_DRAW_INDIRECT=0 \
LG110_INDEX_TYPE=16 LG110_FIRST_INDEX=0 LG110_VERTEX_OFFSET=0 \
LG110_EXPECT_DRAW_COUNT=2 LG110_FRAME_LIMIT=1 \
LG110_READBACK_PIXEL=/tmp/base-vertex.rgba \
  /tmp/vkgl-0.1.29-tests/vktest-index-offset
```

Run serially, only on the exact guarded stream. These experiments do not claim
general safe packet editing or complete native draw replay.
