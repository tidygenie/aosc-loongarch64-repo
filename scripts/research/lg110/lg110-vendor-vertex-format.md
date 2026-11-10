# LG110 vertex descriptor word 3: format metadata and causal mutation

Date: 2026-11-10

## Scope and identity

This extends the stride and indirect-VA proof in
[`lg110-vendor-vertex-descriptor.md`](lg110-vendor-vertex-descriptor.md).  The
installed vendor file remains `/usr/lib/gsgpu/dri/gsgpu_dri.so`, SHA-256
`aff7101ab877db0026fa0f8274e03a579f1b4fd2c16fabe2478d08f224d0da0a`.
No installed file was patched.  All writes below are GDB writes in the test
process only.

## Isolated Vulkan format control

`vktest-vertex-pair.c` now has `LG110_PAIR_MODE=format`.  It requires UV stride
16 and alternates only the second pipeline's UV attribute:

```
A: VK_FORMAT_R32G32_SFLOAT
B: VK_FORMAT_R32_SFLOAT
A: VK_FORMAT_R32G32_SFLOAT
```

Addresses, binding stride, input bytes, position format and fragment shader stay
unchanged.  The attribute-sensitive fragment control produced:

```
A 00 70 10 ff
B 00 00 10 ff
A 00 70 10 ff
```

The strict build used `-Wall -Wextra -Wpedantic -Werror`; test binary SHA-256 is
`f2db6c09f892e652b4c0c0ddab85c568d4a828177116fa0ee51349601206af01`.
The pre-existing test harness still emits a swapchain semaphore-reuse validation
warning; this experiment does not claim a validation-clean run.

## Observed words and raw metadata

The read-only observer `gsgpu-vertex-descriptor-stride.gdb` now stops at
`0x17bc50`, immediately after the 64-bit metadata load and before any bit
extraction.  For UV entries it observed:

| Vulkan UV format | Raw 64-bit metadata | Descriptor word 3 |
| --- | ---: | ---: |
| R32G32_SFLOAT | `0x000005f22c00000000` | `0x0005f22c` |
| R32_SFLOAT | `0x000002720400000000` | `0x00027204` |
| R32G32B32_SFLOAT | `0x000006f3ac00000000` | `0x0006f3ac` |
| R32G32B32A32_SFLOAT | `0x0000077fac00000000` | `0x00077fac` |

The latter two are selected with `LG110_UV_FORMAT=rgb32` and `rgba32`.  Their
first two components produce the same `00 70 10 ff` pixel as the A control, as
expected; R32 produces `00 00 10 ff`.  All three expanded runs passed their
pixel patterns and all 11 UV entries per run passed the raw/word mapping.  The
expanded test binary SHA-256 is
`b1109ff1a484599787502ecb307dc149b56f7833858ae12ef1da8c1c7d0b575c`;
machine-readable values and log hashes are in
[`lg110-vertex-format-summary.json`](lg110-vertex-format-summary.json).

Position remains R32G32 with word 3 `0x0005f22c`.  UV words 0, 1 and 2 remain
`0x022c4000`, `0x00100001` and `0x00000040`; word 1 therefore still contains the
unchanged stride.  One metadata run captured 11 UV constructions (7 A, 4 B).
Construction count is allocator/run dependent and is not assumed to be 12.

## Exact bit mapping in the constructor

The constructor at `0x17bb40` writes word 3 at descriptor offset 12 using these
instructions:

| Instructions | Output field | Raw metadata source |
| --- | --- | --- |
| `17bc58..17bc60` | bits 2:0 | bits 34:32 |
| `17bc70..17bc78` | bits 5:3 | bits 37:35 |
| `17bc80..17bc88` | bits 8:6 | bits 40:38 |
| `17bc90..17bc98` | bits 11:9 | bits 43:41 |
| `17bcac..17bcb8` | bits 14:12 | bits 46:44 |
| `17bc9c..17bca8` | bits 18:15 | bits 50:47 |

Together, every captured entry satisfies:

```
word3[18:0] = raw_metadata[50:32]
```

The four measured formats decode as follows.  These are bit-range names, not
invented semantic names for an undocumented encoding:

| Format | 2:0 | 5:3 | 8:6 | 11:9 | 14:12 | 18:15 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| R32 | 4 | 0 | 0 | 1 | 7 | 4 |
| R32G32 | 4 | 5 | 0 | 1 | 7 | 11 |
| R32G32B32 | 4 | 5 | 6 | 1 | 7 | 13 |
| R32G32B32A32 | 4 | 5 | 6 | 7 | 7 | 14 |

The constructor reads the old word 3 and does not clear bits 31:19 here, so this
proof is limited to the low 19 bits.  The raw metadata values are not claimed to
be a public format enum or a portable UAPI.

## Direct causal proof

`gsgpu-mutate-vertex-format.gdb` uses the all-R32G32 `pipeline` control and only
the three submitted UV entries at BO offsets `0x2b10`, `0x8f90`, `0xf390`:

```
0x0005f22c -> 0x00027204
```

Exactly three writes occurred, no unexpected old value was seen, and external
byte assertions changed from the unmutated `00 70 10 ff` on every frame to:

```
frame 0: 00 00 10 ff
frame 1: 00 00 10 ff
frame 2: 00 00 10 ff
```

Thus word 3 low 19 bits causally control the vendor's UV format interpretation,
in addition to the previously proven word 1 stride field.  The causal log SHA-256
is `3a9c00fcc403e6a21ec46fbc55b28ca270980913fc1df828f5b0c8245e3b1fff`;
each causal pixel file is
`d92849ac451023a25885cd7aba9c6339f2c4d5b12c5c07fd86b59b04b7878b1e`.
The expanded table binary also reran the same three-write/R32-pixel assertion;
its log SHA-256 is
`fcbb3beeb61e110db6e6f211099467b756096449b32f75f8bdce2414b5a6ac93`.

## Failed attempts corrected

- Printing `$t1` or `$t2` at the final `17bcbc` breakpoint yielded only already
  extracted bit fields (`0x7`, then `0xb/0x4`), not raw metadata.  The correct
  observation is the independent `17bc50` breakpoint before extraction.
- The first format mutation made zero writes because masking the CPU pointer
  with `0xffffff` wrongly assumed a 16 MiB-aligned mapping.  That run stayed
  BBB and failed the assertion.  The script now derives mapping base from the
  first UV entry at `0x2b10`; the retry made exactly three writes.  The same fix
  was applied to `gsgpu-mutate-vertex-stride.gdb`, whose three writes and
  `00 40 40 ff` pixels were independently reproduced.
- A regression loop initially invoked the old harness's forbidden
  `attribute_sensitive + offset` combination and stopped at its usage gate.
  Re-running offset in its intended non-attribute/default-shader mode, plus
  same/stride/pipeline/same-UV16/format attribute controls, passed 6/6.

Raw logs, disassembly and pixels are retained under
`/home/houge/infra/gpu/evidence/lg110-descriptor-format-20261110/` and
`/home/houge/infra/gpu/evidence/lg110-descriptor-format-table-20261110/`.  This is a
vendor-format descriptor result, not a native Vulkan driver, performance claim,
or proof for formats other than the four tested.  The later normalized-integer
extension is in [`lg110-vendor-vertex-unorm.md`](lg110-vendor-vertex-unorm.md).
