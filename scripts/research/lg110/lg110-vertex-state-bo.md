# LG110 vertex binding offset/stride BO differential (2026-11-07)

This is passive research on installed vkgl 0.1.31. It is not a native Vulkan
implementation, complete VAO decoder or license to write arbitrary GPU memory.

## Constant-pixel controls

`vktest-vertex-state.c` keeps both VBO byte streams identical across three runs:

1. position binding offset 0, UV stride 8;
2. position binding offset 8, UV stride 8;
3. position binding offset 8, UV stride 16.

Cyclic positions make offsets 0 and 8 cover the center with the same winding.
Every possible float2 UV is `{0,0.5}`, so both UV strides yield the same center.
All six exact-public captures produce raw `007f00ff`.

The two 16-dword XDMA IBs and the 672-dword GFX IB are byte-identical across all
three states. Therefore these Vulkan fields are not directly represented by a
changed word in the captured IBs. This does not mean that the hardware ignores
them; correct pixels already prove the GL state has an effect.

## Guarded auxiliary BO reads

The GFX stream references small BOs at `0x1022c8000` and `0x1022cc000`. Neither
has a persistent tracked user mapping. The tracer extension accepts only an
explicit `LG110_TRACE_VA`, resolves it through an already tracked VA mapping and
handle, then creates a temporary page-aligned read-only GEM mapping for at most
the selected 64 dwords. It unmaps the range after logging. It never writes the BO.

For each BO and each of the three states, the first 64 dwords are identical at
all three traced submissions. They are also identical across offset and stride
controls. The supported negative result is limited to:

- the two XDMA IBs and 672-dword GFX IB;
- the first 256 bytes of BO `0x1022c8000`;
- the first 256 bytes of BO `0x1022cc000`.

No vertex offset/stride field is assigned. Other BOs, later bytes and transient
state remain unresolved.

With no `LG110_TRACE_VA`, no VADUMP is emitted and the old tracer path remains
passive. `LG110_TRACE_VA=0x1` returns `no_va_mapping`, handle 0, and performs no
GEM mapping; the draw still passes. See `vertex-state-public-audit.log` and the
`default-no-va`/`invalid-va` traces.

## Reproduce

```sh
scripts/research/lg110/build-vertex-state.sh /tmp/lg110-vertex-state
cd /tmp/lg110-vertex-state/run
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export LD_PRELOAD=/tmp/lg110-vertex-state/lg110-draw-count-mutation.so
export LG110_TRACE_MAX=4 LG110_TRACE_AUX_MAX=64 LG110_TRACE_IB_DWORDS=2048
export LG110_TRACE_VA=0x1022cc000
unset LG110_MUTATE_DRAW_FIELD
LG110_TEXTURE_VARIANT=A LG110_DRAW_VERTEX_COUNT=3 LG110_DRAW_INSTANCE_COUNT=1 \
LG110_DRAW_FIRST_INSTANCE=0 LG110_DRAW_INDEXED=0 LG110_DRAW_INDIRECT=0 \
LG110_INDEX_TYPE=16 LG110_FIRST_INDEX=0 LG110_VERTEX_OFFSET=0 \
LG110_EXPECT_DRAW_COUNT=3 LG110_FRAME_LIMIT=1 \
LG110_POSITION_OFFSET=8 LG110_UV_STRIDE=16 \
LG110_READBACK_PIXEL=/tmp/vertex-state.rgba \
  /tmp/lg110-vertex-state/vktest-vertex-state
```

Run serially. Repeat offset/stride=0/8 and 8/8 for the full differential. The VAs
are capture-specific observations, not stable hardware addresses for other
processes; select them only after resolving the current trace.
