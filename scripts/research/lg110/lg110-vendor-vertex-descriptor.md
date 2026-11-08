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
observed vertex descriptor is not embedded directly in the captured IB.

## Final storage in handle 8 and per-frame proof

The descriptor array is allocated through the vendor resource allocator at
`0xe92a8`: its CPU pointer is cached at context offset 3896, its resource at
3912, and its mapped size at 3908.  Runtime inspection showed that the resource
GPU base is `0x1010c0000`, exactly handle 8.  `/proc/<pid>/maps` identified a
16 MiB `/dev/dri/renderD128` mapping; the first descriptor pair is at CPU mapping
offset and BO offset `0x2b00`.  This is a GPU BO mapping, not an ordinary stack
allocation.

Each later construction uses a new suballocation.  The first pair constructed
for each frame is the one already present when that frame's CS is sampled:

| Frame | CS | Pair BO offset | UV word 1 offset | Pipeline UV | Stride UV |
| --- | --- | --- | --- | --- | --- |
| 0 | 3 | `0x2b00` | `0x2b14` | `0x00100001` (16) | `0x00080001` (8) |
| 1 | 4 | `0x8f80` | `0x8f94` | `0x00100001` (16) | `0x00100001` (16) |
| 2 | 5 | `0xf380` | `0xf394` | `0x00100001` (16) | `0x00080001` (8) |

For all three pairs, the position descriptor, UV GPU address `0x1022c4000`, UV
size `0x40`, and word 3 `0x0005f22c` are identical across pipeline and stride.
Only UV word 1 changes, exactly following the A-B-A pixel sequence.  All six
targeted programs returned success.  The assertion output SHA-256 is
`50bde8d11be759ab32526cec204f6d068f95723d667d58bb96a7154410324483`.

### IB indirect reference and pointer writer

The complete IB contains the descriptor VA even though it does not contain the
descriptor words.  The low address words are `0x010c2b00` at seq3 word220,
`0x010c8f80` at seq4 word218, and `0x010cf380` at seq5 word218; each following
high word is one.  These are the exact per-frame pair offsets above.

A write watchpoint on the first drawcall's `+336` slot caught the actual writer.
The function at `0x181fe8`, called by real `draw_vbo`, executes:

```
182050: ldptr.d   $t1, $s1, 3912   # descriptor resource
182058: ldptr.w   $t2, $s1, 3908   # current suballocation offset
18205c: ld.d      $t1, $t1, 64     # resource GPU base
18206c: add.d     $t1, $t1, $t2
182074: st.d      $t1, $s3, 336    # 64-bit descriptor VA in drawcall
```

The watchpoint reports the following instruction at `0x182078`, after the store
has executed.  In the first draw, resource base `0x1010c0000` plus offset
`0x2b00` produces exactly the observed IB value.  The supplied
`gsgpu-descriptor-pointer-writer.gdb` reproduces this watch.  Thus the complete
path is: API stride -> cached VBO entry -> handle-8 descriptor word 1 ->
drawcall+336 indirect descriptor VA -> GPU-visible A-B-A change.

Two failed approaches are retained as evidence boundaries.  A hardware read
watchpoint on word 1 reported no later userspace read; direct GPU consumption
does not require such a read.  An earlier targeted handle-8 dump appeared
identical only because the tracer's single `values` buffer truncated it to 256
dwords (1 KiB), which did not cover offset `0x2b00`.  That negative comparison
is retracted; it was not a valid descriptor comparison.  A later attempt to
use the fourth construction offsets also failed at seq3/seq4 because those
entries had not yet been constructed.  No failed assertion was forced to pass.

## Causal mutation: the field changes the rendered pixels

Passive correlation was followed by a test-process-only mutation using
`gsgpu-mutate-vertex-stride.gdb`.  The exact binary SHA-256 was checked before
launch.  Starting from pipeline/UV16 (BBB), the script changes only the first
UV entry submitted for each frame, at UV-entry offsets `0x2b10`, `0x8f90`, and
`0xf390`; these are pair offsets plus `0x10`.  It refuses to write unless the
old word is exactly `0x00100001`, preserves all other bits, and replaces the
stride field with 8 (`0x00080001`).

Exactly three writes occurred, with no unexpected descriptor.  Raw pixels
changed from BBB (`007010ff` three times) to AAA (`004040ff` three times).
Success was asserted externally from the three raw files and mutation count,
not from the test program's own `pattern_valid` flag.  The assertion SHA-256 is
`d3806dd12a570d2d5024232df6d14856c7fde101d2ddf6c76e62339c537bf087`.

An initial mutation attempt used pair bases rather than UV-entry addresses and
made zero writes; pixels stayed BBB and the assertion failed.  That attempt is
not evidence against the field.  Correcting the three addresses to pair+0x10
produced the causal result above.  The installed driver was never patched.

## Reproduction and evidence

Use the established `vktest-vertex-pair` environment and run GPU tests serially.
The GDB scripts require the exact binary hash above.  Start with the descriptor
script and `LG110_PAIR_MODE=stride`, `LG110_FRAME_LIMIT=3`,
`LG110_UV_STRIDE=8`, `LG110_ATTRIBUTE_SENSITIVE=1`, and
`LG110_FRAGMENT_SHADER=vktest-attribute-color.frag.spv`.

For complete IBs, use the public tracer with `LG110_TRACE_IB_DWORDS=2048`,
`LG110_TRACE_MAX=8`, and `LG110_TRACE_WAIT_CS=1`; do not use the old 32-dword
prefix as a full-packet comparison.

For the direct BO proof on this exact run, keep the sensitive stride controls
and set `LG110_TRACE_BO_DWORDS=8` plus, serially:

```
LG110_TRACE_VA=0x1010c2b00  # frame 0, read seq3
LG110_TRACE_VA=0x1010c8f80  # frame 1, read seq4
LG110_TRACE_VA=0x1010cf380  # frame 2, read seq5
```

Repeat with pipeline/UV16.  These VAs and offsets must be re-derived if the
binary, allocator, or test changes; they are not a general UAPI.

Raw disassembly, GDB output, complete IBs, pixels, and SHA manifests are retained
under `/home/houge/infra/gpu/evidence/` and
`/home/houge/infra/gpu/evidence/lg110-sensitive-allva-20261108/`.  Large raw
traces are not committed.

This establishes the concrete vendor vertex-descriptor field, its handle-8
BO storage, the drawcall's indirect descriptor VA, and a causal pixel mutation.
It does not claim a native Vulkan implementation, zero-copy, or any performance
improvement.
