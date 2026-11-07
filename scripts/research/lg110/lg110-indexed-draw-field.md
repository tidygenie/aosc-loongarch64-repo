# LG110 indexed versus indirect native draw differential (2026-11-07)

This is a passive installed-vkgl 0.1.27 experiment, not a packet decoder, native
Vulkan driver, replay system, or active indexed-field mutation. All four cases
use the public instance shader with one instance, firstInstance=0, three indices
or vertices, and the same clear-at-center raw output `663300ff`.

## Capture alignment and VA control

- direct and indirect each have one 16-dword XDMA IB and one 672-dword GFX IB.
- indexed and indexed-indirect add a second 16-dword XDMA IB for the six-byte
  UINT16 index upload, followed by the 672-dword GFX IB. Comparisons use the
  final GFX IB rather than comparing the added XDMA with a GFX IB by position.
- direct→indirect and indexed→indexed-indirect each differ only at the same 12
  GPU-VA-bearing positions: 74, 98, 206, 230, 338, 362, 450, 470, 494, 582, 602,
  626. Vulkan indirect recording is expanded by vkgl to the same GL draw; no
  native indirect packet is claimed.

## Stable indexed-associated fields

After aligning the GFX IBs, direct→indexed and indirect→indexed-indirect have
exactly the 12 VA differences plus these four fields:

| Word | Nonindexed | Indexed | Evidence/limit |
|---:|---:|---:|---|
| 268 | `0x40000400` | `0x40000402` | Indexed-associated low bit; existing topology bits remain `0x400`. Not actively toggled. |
| 276 | `2` | `0xffffffff` | Last-index-like nonindexed value becomes sentinel for indexed; earlier active word276-only mutation did not alter pixels. |
| 280 | `0` | `0x022c0000` | Exactly the traced index GEM VA. Its separate XDMA upload has byte count 6 and destination `0x022c0000`; GEM allocation is aligned to `0x4000`. |
| 281 | `0` | `1` | Present on this UINT16 path; index-type semantics require a UINT32 differential and remain unassigned. |

Words270=3, 273=3, 274=0 and 275=0 are unchanged. No word273/275/276 semantic
assignment is added. In particular, setting indexed bit 0x2 in an otherwise
nonindexed stream would leave word280 null and is deliberately not attempted.

## Evidence and reproduction

Raw files:
`draw-indexed-public-{direct,indexed,indirect,indexed-indirect}.{rgba,stdout.log,trace.log}`.
Each trace includes GEM_VA, XDMA and complete GFX data. Build the public test and
tracer without executing GPU work:

```sh
scripts/tests/vkgl/build-0.1.27-tests.sh /tmp/vkgl-0.1.27-tests
scripts/research/lg110/build-draw-count-mutation.sh /tmp/lg110-draw
cd /tmp/vkgl-0.1.27-tests/instance
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export LD_PRELOAD=/tmp/lg110-draw/lg110-draw-count-mutation.so
export LG110_TRACE_MAX=4 LG110_TRACE_AUX_MAX=64 LG110_TRACE_IB_DWORDS=2048
unset LG110_MUTATE_DRAW_FIELD
LG110_TEXTURE_VARIANT=A LG110_DRAW_VERTEX_COUNT=3 LG110_DRAW_INSTANCE_COUNT=1 \
LG110_DRAW_FIRST_INSTANCE=0 LG110_DRAW_INDEXED=1 LG110_DRAW_INDIRECT=0 \
LG110_EXPECT_DRAW_COUNT=2 LG110_FRAME_LIMIT=1 \
LG110_READBACK_PIXEL=/tmp/indexed.rgba \
  /tmp/vkgl-0.1.27-tests/vktest-draw-instance
```

Run serially. Repeat with indexed/indirect=0/0, 0/1 and 1/1. The four raw pixels
must remain clear; these observations do not establish indexed replay or full
native draw semantics.
