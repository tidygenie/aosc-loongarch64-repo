# LG110 UNORM vertex formats and the RGBA16 candidate fix

Date: 2026-11-10

## Scope and release state

This extends [`lg110-vendor-vertex-format.md`](lg110-vendor-vertex-format.md)
from four 32-bit float formats to four normalized integer UV formats.  It also
fixes one discovered vkgl compatibility defect in the public source.

The pre-release baseline was SHA-256
`afac548f68ed43574b546b76e541110fba46e9c329f66a3318fcecc7dc4d2b3b`.
The fixed library was subsequently installed and released as 0.1.32-1 with
SHA-256 `a84c177852cac3f405c1c155d75affcf4ef6d3f7e33dcceed42b6dfb73e041b3`.
`candidate_installed=false` in the summary describes the capture-time matrix,
not the later release.  The installed vendor file remains
`/usr/lib/gsgpu/dri/gsgpu_dri.so`, SHA-256
`aff7101ab877db0026fa0f8274e03a579f1b4fd2c16fabe2478d08f224d0da0a`.
No vendor driver or kernel module was modified.  GDB writes were limited to
descriptors in short-lived test processes.

This is Vulkan-over-GL compatibility and descriptor research, not native
Vulkan or a performance claim.  The vertex-pair harness is not claimed to be
validation-clean.

## Defect and one-line candidate fix

`gl_type_for_format()` already handled RG8/RGBA8/RG16 UNORM but lacked
`VK_FORMAT_R16G16B16A16_UNORM`.  Its default branch selected four GL_FLOAT
components with normalization disabled.  The candidate adds only:

```c++
case VK_FORMAT_R16G16B16A16_UNORM:
   *components = 4; *normalized = GL_TRUE; return GL_UNSIGNED_SHORT;
```

Baseline and fixed candidates use the identical direct GCC 16
`-O3 -march=loongarch64 -mtune=la664 -flto` build.  The baseline output is
byte-identical to the installed library (`afac548f…`), eliminating a build
layout confound.  Fixed candidate SHA-256 is
`a84c177852cac3f405c1c155d75affcf4ef6d3f7e33dcceed42b6dfb73e041b3`.

## Final test and machine-checked summary

`vktest-vertex-pair.c` accepts `rg8unorm`, `rgba8unorm`, `rg16unorm`, and
`rgba16unorm`.  Strict `-Wall -Wextra -Wpedantic -Werror` rebuilding succeeded.
The final test binary SHA-256 is
`351b5588666a96d5207dfa62a71553b756abd0150be4ec2a12d2978dcdb8e9ca`.

The machine-readable summary is
[`lg110-vertex-unorm-summary.json`](lg110-vertex-unorm-summary.json), SHA-256
`7cdd42eedde471e700b135040d17e45cb01903e8daa4afbdc8723a3bc2d15d77`.
It records the fixed candidate hash and `candidate_installed=false`.
`summarize-vertex-unorm.py` rejects missing entries, incorrect pixels,
unexpected mutations, or mapping violations instead of copying hand-written
values.

## Raw metadata and final descriptors

Each read-only fixed-candidate GDB run captured 11 UV constructions.  Every
entry retained words 0/1/2 = `0x022c4000`, `0x00100001`, `0x00000040` and
satisfied:

```
word3[18:0] = raw_metadata[50:32]
```

| Vulkan UV format | Raw 64-bit metadata | Final word 3 |
| --- | ---: | ---: |
| R8G8_UNORM | `0x0001822c00000000` | `0x0001822c` |
| R8G8B8A8_UNORM | `0x00050fac00000000` | `0x00050fac` |
| R16G16_UNORM | `0x0002822c00000000` | `0x0002822c` |
| R16G16B16A16_UNORM, fixed | `0x00060fac00000000` | `0x00060fac` |

Before the fix, RGBA16 incorrectly produced the RGBA32-float word
`0x00077fac`.  The independent `0x00060fac` value after the one-line fix agrees
with the numeric conversion result below.

## Native A/B/A pixel controls

The first and third pipelines remain R32G32_SFLOAT; only the second UV format
changes.  All four fixed-candidate runs passed exact A/B/A assertions:

| Second format | A / B / A pixel bytes |
| --- | --- |
| R8G8_UNORM | `007010ff / 000000ff / 007010ff` |
| R8G8B8A8_UNORM | `007010ff / 000000ff / 007010ff` |
| R16G16_UNORM | `007010ff / 001000ff / 007010ff` |
| R16G16B16A16_UNORM, fixed | `007010ff / 001000ff / 007010ff` |

These shared-input runs isolate format routing; they are not the numeric
conformance evidence.  Separate buffers below supply that evidence.

## Non-endpoint numeric equivalence

`LG110_PAIR_MODE=numeric` uses a float A buffer and a separate normalized B
buffer.  With stride 16 and a 64-byte allocation there are exactly four
vertices; float pairs reside at float indices `0,4,8,12`.  Both representations
contain these four equivalent samples:

```
(0,0), (128,64), (64,192), (26,230)  -- 8-bit sample units
```

The 16-bit buffer multiplies every sample by 257.  Thus the test includes
non-zero, non-endpoint values and does not merely check 0 and 1.

For every one of RG8/RGBA8/RG16/RGBA16 on the fixed candidate, A/B/A pixels were
exactly `004030ff / 004030ff / 004030ff`.  The RGBA16 baseline instead produced
`004030ff / 000000ff / 004030ff` and failed, while the one-line fixed candidate
passed.  This is a measured compatibility correction, not a relaxed assertion.

Two early harness attempts incorrectly packed the float pairs contiguously;
one also wrote eight stride-16 vertices into a 64-byte buffer and aborted in the
test process.  Those attempts are retained in the evidence directory and are
not attributed to the hardware.  The final four-vertex layout passes.

## Single-B causal restoration

`gsgpu-mutate-vertex-format-pair.gdb` receives source and destination word 3
values with GDB `-ex` options.  At the three submitted UV BO offsets it permits
A = `0x0005f22c`, mutates only the matching B descriptor, and counts unexpected
old values.  For each of the four fixed-candidate UNORM formats:

```
B source -> 0x0005f22c (R32G32_SFLOAT)
MUTATIONS=1 UNEXPECTED=0
```

The sole write was at BO offset `0x8f90`.  Every B pixel then became exactly
`007010ff`, producing byte-identical A/A/A pixels and passing the explicit
`LG110_EXPECT_FORMAT_MUTATION=1` assertion.  The new RGBA16 word therefore has
both numeric-equivalence and direct-descriptor causal evidence.

## Regression matrix

The final matrix passed:

- four plain format controls, four metadata runs, four numeric-equivalence
  runs, and four single-B mutation runs;
- three legacy r32/rgb32/rgba32 controls;
- all twelve instance-rate direct/indirect × indexed/nonindexed ×
  two/first/zero cases;
- baseline and fixed dmabuf/wl_shm vulkaninfo, 60-frame vkcube, and five-scene
  vkmark smoke, with zero validation events in each of the four logs.

A separate loader-debug run proved that `VK_LAYER_KHRONOS_validation` was
inserted at instance and device scope.  The zero counts are not attributed to
the RGBA16 change because baseline also has zero.  No llvmpipe/swrast fallback
was used.

The zero-instance test expectation was also corrected to the clear colour
`663300ff`; its previous green expectation contradicted both Vulkan semantics
and the existing 0.1.31 report.  All twelve cases passed after this test-only
correction.

## Artifacts and remaining work

Raw logs and per-frame pixels are outside Git under
`/home/houge/infra/gpu/evidence/lg110-vertex-unorm-20261110/` and
`/home/houge/infra/gpu/evidence/lg110-rgba16-unorm-smoke/`.
The public summary stores the UNORM log SHA-256 values.

Release validation, paired performance controls, package identity, and AOSC
publication are recorded in
[`../../tests/vkgl/vkgl-0.1.32.md`](../../tests/vkgl/vkgl-0.1.32.md).
No maximum-performance, general-format conformance, native Vulkan, or
whole-objective completion claim is made.
