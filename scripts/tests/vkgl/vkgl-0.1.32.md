# vkgl-loonggpu 0.1.32-1 RGBA16 UNORM release

Date: 2026-11-10

## Compatibility correction

0.1.31 omitted `VK_FORMAT_R16G16B16A16_UNORM` from
`gl_type_for_format()`, so the default branch interpreted the buffer as four
unnormalized GL_FLOAT components.  Non-endpoint numeric A/B/A testing produced
`004030ff / 000000ff / 004030ff` instead of float-equivalent colours.

0.1.32 adds only the four-component, normalized GL_UNSIGNED_SHORT case.  The
full numeric, descriptor, and single-B causal evidence is in
[`../../research/lg110/lg110-vendor-vertex-unorm.md`](../../research/lg110/lg110-vendor-vertex-unorm.md).

The baseline direct build is byte-identical to the old installed library:

```
0.1.31 baseline: afac548f68ed43574b546b76e541110fba46e9c329f66a3318fcecc7dc4d2b3b
0.1.32 fixed:    a84c177852cac3f405c1c155d75affcf4ef6d3f7e33dcceed42b6dfb73e041b3
```

Thus the result is not confounded by PGO, compiler flags, or link layout.

## Correctness and regression evidence

- Four plain format, four metadata, four numeric-equivalence, and four single-B
  mutation runs passed on the final fixed candidate; three legacy float controls
  also passed.
- All four numeric conversions passed again through the installed 0.1.32 ICD.
- All twelve instance-rate direct/indirect, indexed/nonindexed,
  two/first/zero-instance cases passed.
- Baseline, fixed candidate, and installed 0.1.32 dmabuf/wl_shm vulkaninfo,
  vkcube, and five-scene vkmark smoke passed.  Validation events were zero;
  loader-debug separately proved actual Khronos layer insertion.
- No llvmpipe/swrast fallback was used.  The vertex-pair harness itself is not
  claimed to be validation-clean.

The zero-instance test's expected pixel was corrected to the clear colour
`663300ff`; this was a test bug, not a driver change.

## Paired performance control

`vkgl-paired-benchmark.py` ran six serial AB/BA rounds at 640x480 FIFO, four
seconds per scene, with the validation layer enabled.  It rejects the wrong GPU,
software rendering, validation errors, missing FPS, and failed processes.

| Scene | Baseline median FPS | Fixed median FPS | Median paired change | Pair range |
| --- | ---: | ---: | ---: | ---: |
| vertex | 212.0 | 210.0 | +2.62% | -26.22% to +7.51% |
| texture | 141.0 | 140.5 | +2.42% | -5.63% to +7.87% |
| shading | 171.5 | 182.0 | +2.62% | -7.22% to +17.95% |

The large opposing samples show substantial run noise.  These numbers are a
release regression control, **not a demonstrated performance improvement**.
The raw summary SHA-256 is
`265672dbf67a21782c422a4caa5e0a5ca64536440c25db71b89a423c76685441`.

## Package, installation, and publication

- Package: `vkgl-loonggpu_0.1.32-1_loongarch64.deb`.
- Deb SHA-256:
  `f5da94650e4cf4c61488a46c64906836bbfabf2eb50bd91dbb26b3130699dc06`.
- The packaged and installed library both match the fixed candidate SHA above.
- The authenticated 0.1.31 rollback deb was retained before installation.
- Signed AOSC publication passed dry-run/deploy, 37-entry Packages indexes,
  MD5/Release/InRelease checks, HTTPS availability, and isolated apt update.
- Local, server, and HTTPS deb bytes match the published SHA-256.

Evidence remains outside Git under
`/home/houge/infra/gpu/evidence/vkgl-rgba16-unorm-ab-20261110/` and
`/home/houge/infra/gpu/evidence/vkgl-rgba16-unorm-smoke/`.

This release improves Vulkan-over-GL compatibility.  It is not native Vulkan,
zero-copy vertex fetching, a maximum-performance proof, or completion of the
larger LoongArch/AOSC objective.
