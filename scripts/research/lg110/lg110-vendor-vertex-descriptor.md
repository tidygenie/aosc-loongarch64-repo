# LG110 vendor vertex-buffer descriptor: stride is outside the inline IB

Date: 2026-11-08

## Scope and binary identity

This is a userspace reverse-engineering and read-only runtime observation of the
installed vendor Gallium driver.  It does not modify the driver, submit a
mutation, or claim native Vulkan.  The exact analyzed file is
`/usr/lib/gsgpu/dri/gsgpu_dri.so`, SHA-256
`aff7101ab877db0026fa0f8274e03a579f1b4fd2c16fabe2478d08f224d0da0a`, ELF
BuildID `79d0596ed0191fa5c6e36d6545a732801f56e446`.  The offsets below are not
portable to another binary without redoing the analysis.

The driver is stripped but retains diagnostic strings and unwind function
ranges.  Resolving `pcalau12i` plus low-12-bit references to
`set_vertex_buffers`, `draw_vbo`, and `gsgpu_drawcall_set_*` locates the Gallium
trace wrappers.  A one-frame GDB run then reads the real function pointers from
the underlying pipe context; it does not change registers or memory.

## Recovered real functions

| Purpose | File offset | Function range |
| --- | --- | --- |
| Gallium trace `set_vertex_buffers` wrapper | `0xfd568` | `0xfd568..0xfd6a8` |
| Gallium trace `draw_vbo` wrapper | `0xfe688` | `0xfe688..0xfe710` |
| Real `set_vertex_buffers` | `0x17ae08` | `0x17ae08..0x17b03c` |
| Real `draw_vbo` | `0x197d18` | `0x197d18..0x1981b4` |
| Drawcall/VBO descriptor construction | `0x17bb40` | `0x17bb40..0x17bd68` |

The trace wrapper stores the underlying pipe pointer at wrapper-context offset
944.  Its real `set_vertex_buffers` and `draw_vbo` vtable entries are at offsets
512 and 48 respectively.  The supplied `gsgpu-real-pipe-offsets.gdb` records the
relocated pointers and their file-relative offsets.

## VBO cache and descriptor construction

The real `set_vertex_buffers` does not emit a draw command.  It caches each
16-byte `pipe_vertex_buffer` at:

```
ctx + 0xe38 + 16 * start_slot
```

For each entry it copies source offset 0 as a 16-bit stride to destination
offset 0, source offset 4 to destination offset 4 (buffer offset), and source
offset 8 to destination offset 8 (resource pointer).  A non-null resource also
receives a dirty flag.

`draw_vbo` calls the drawcall constructor at `0x17bb40` before setting draw and
pipe state.  For each enabled vertex element, that constructor obtains the
binding's cached resource pointer at context offset `3648 + 16 * binding` and
buffer offset at `3644 + 16 * binding`.  It then advances the slot pointer by
`0x1000` and executes:

```
17bc28: ld.hu       $t5, $s5, -456
17bc34: bstrpick.d  $t2, $t0, 0x3f, 0x20
17bc38: bstrins.w   $t2, $t5, 0x1d, 0x10
17bc40: st.w        $t2, $s4, 4
```

Because `-456 + 0x1000 = 3640 = 0xe38`, this is exactly the cached 16-bit
stride.  `bstrins.w` inserts its low 14 bits into descriptor word 1 bits
`[29:16]`.  Other instructions construct a 48-bit GPU address from the resource
GPU address, buffer offset, and element offset; word 2 is a remaining size and
word 3 contains additional element-derived bits.  No semantics beyond the
observed fields are assigned here.

## Runtime proof on the sensitive A-B-A control

`gsgpu-vertex-descriptor-stride.gdb` breaks after the 16-byte entry is complete
and reads it without modifying it.  In the stride 8 -> 16 -> 8 run, each UV
state was constructed four times.  Representative entries are:

| Binding | GPU VA | Stride | word 0 | word 1 | word 2 | word 3 |
| --- | --- | --- | --- | --- | --- | --- |
| position | `0x0001022c0000` | 8 | `0x022c0000` | `0x00080001` | `0x00000028` | `0x0005f22c` |
| UV | `0x0001022c4000` | 8 | `0x022c4000` | `0x00080001` | `0x00000040` | `0x0005f22c` |
| UV | `0x0001022c4000` | 16 | `0x022c4000` | `0x00100001` | `0x00000040` | `0x0005f22c` |

The low one in word 1 is address bit 32.  Thus the two addresses exactly match
captured BO handles 15 and 16, whose input hashes remain unchanged.  The stride
change is instead represented by `8 << 16` versus `16 << 16` in descriptor word
1.  CPU readback remained the correct `004040ff -> 007010ff -> 004040ff`.

## Important evidence correction: prefix versus complete IB

The earlier sensitive traces dumped only the first 32 dwords of each draw IB.
Their equality was never evidence that complete draw packets were identical.
A new run with `LG110_TRACE_IB_DWORDS=2048` captured the complete lengths:

- seq1 and seq2: 16 dwords each;
- seq3: 544 dwords;
- seq4 and seq5: 528 dwords each.

The complete pipeline and stride IB arrays are nevertheless identical at every
word.  Neither `0x00080001` nor `0x00100001` appears inline.  Therefore the
observed vertex descriptor is not embedded directly in the captured IB.  Its
subsequent consumer or indirect storage remains unresolved.

A hardware read watchpoint on the first UV descriptor's word 1 reported no
userspace read before process exit; this is a recorded negative result, not
proof that no consumer exists.  Separately, targeted handle-8 dumps from a new
pipeline/stride pair were word-for-word identical, despite hash differences in
the earlier all-VA runs.  Those earlier handle-8 differences must not be called
vertex-descriptor changes without a stable reproduction.

## Reproduction and evidence

Use the established `vktest-vertex-pair` environment and run GPU tests serially.
The GDB scripts require the exact binary hash above.  Start with the descriptor
script and `LG110_PAIR_MODE=stride`, `LG110_FRAME_LIMIT=3`,
`LG110_UV_STRIDE=8`, `LG110_ATTRIBUTE_SENSITIVE=1`, and
`LG110_FRAGMENT_SHADER=vktest-attribute-color.frag.spv`.

For complete IBs, use the public tracer with `LG110_TRACE_IB_DWORDS=2048`,
`LG110_TRACE_MAX=8`, and `LG110_TRACE_WAIT_CS=1`; do not use the old 32-dword
prefix as a full-packet comparison.

Raw disassembly, GDB output, complete IBs, pixels, and SHA manifests are retained
under `/home/houge/infra/gpu/evidence/` and
`/home/houge/infra/gpu/evidence/lg110-sensitive-allva-20261108/`.  Large raw
traces are not committed.

This establishes a concrete vendor vertex-descriptor field.  It does not yet
establish its final storage path, a native Vulkan implementation, zero-copy,
or any performance improvement.
