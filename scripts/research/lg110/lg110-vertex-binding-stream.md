# LG110 independent vertex-binding native stream observations (2026-11-07)

This is a passive exact-public-vkgl 0.1.30 capture. It is not a native Vulkan
implementation, complete attribute decoder or proof that arbitrary GPU addresses
can be edited safely.

The public test uses two actual vertex buffers:

- binding 0: five float2 positions, one leading dummy, VkBuffer binding offset 8,
  stride 8; shader location 0 uses the four real positions;
- binding 1: four float2 UVs separated by two padding floats, binding offset 0,
  stride 16; shader location 1 uses the UVs.

All nonindexed/indexed × direct/indirect cases produce raw gradient `007f00ff`.
Nonindexed captures have two 16-dword XDMA IBs followed by a 672-dword GFX IB;
indexed adds a third XDMA IB before the same-sized GFX IB.

## XDMA and VA observations

- The two vertex uploads have destination VAs `0x022c0000` and `0x022c4000` in
  these captures. Their XDMA word1 values are 10 and 16 respectively. These are
  raw values only; no byte/element-count semantics are assigned.
- Neither vertex-buffer base VA appears as a literal 32-bit word anywhere in the
  672-dword GFX IB. The test also has binding offset 8; no unsupported pointer
  field is inferred from that absence.
- Indexed adds destination `0x022c8000`, which appears exactly at GFX word280,
  consistent with the earlier index-VA evidence. This does not imply that vertex
  and index buffers share one addressing mechanism.
- Direct→indirect changes only the same 12 VA-bearing GFX positions seen in
  earlier tests: 74, 98, 206, 230, 338, 362, 450, 470, 494, 582, 602, 626. The
  Vulkan indirect command is still expanded by vkgl to GL draws; no native
  indirect packet is claimed.

No active VBO address mutation is attempted. Resolving vertex addressing requires
additional controlled layout/address evidence, not guessing from these negative
literal-address results.

## Evidence and reproduction

Raw evidence:
`draw-vertex-binding-public-native-indexed{0,1}-indirect{0,1}.{rgba,stdout.log,trace.log}`.

```sh
scripts/tests/vkgl/build-0.1.30-tests.sh /tmp/vkgl-0.1.30-tests
scripts/research/lg110/build-draw-count-mutation.sh /tmp/lg110-draw
cd /tmp/vkgl-0.1.30-tests/bindings
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export LD_PRELOAD=/tmp/lg110-draw/lg110-draw-count-mutation.so
export LG110_TRACE_MAX=4 LG110_TRACE_AUX_MAX=64 LG110_TRACE_IB_DWORDS=2048
unset LG110_MUTATE_DRAW_FIELD
LG110_TEXTURE_VARIANT=A LG110_DRAW_VERTEX_COUNT=3 LG110_DRAW_INSTANCE_COUNT=1 \
LG110_DRAW_FIRST_INSTANCE=0 LG110_DRAW_INDEXED=0 LG110_DRAW_INDIRECT=0 \
LG110_INDEX_TYPE=16 LG110_FIRST_INDEX=0 LG110_VERTEX_OFFSET=0 \
LG110_EXPECT_DRAW_COUNT=3 LG110_FRAME_LIMIT=1 \
LG110_READBACK_PIXEL=/tmp/vertex-bindings.rgba \
  /tmp/vkgl-0.1.30-tests/vktest-vertex-bindings
```

Run serially. Repeat indexed/indirect=0/1, 1/0 and 1/1 for the full matrix.
