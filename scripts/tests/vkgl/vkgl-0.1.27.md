# vkgl-loonggpu 0.1.27-1 software-instancing evidence (2026-11-07)

## Baseline defect

This release fixes Vulkan instancing on the vendor GL 2.1 path. It remains
Vulkan-over-GL, not native Vulkan, and does not claim hardware instancing.

On exact 0.1.26, a non-instance shader with `instanceCount=1` versus 2 produced
byte-identical XDMA/GFX IBs and the same raw `007f00ff` pixel. The draw command
recorded `instance_count`, but execution called only one `glDrawArrays` or
`glDrawElements`; `firstInstance` was discarded.

A shader that actually reads `gl_InstanceIndex` failed pipeline creation with
`VK_ERROR_INITIALIZATION_FAILED` (-3). SPIRV-Cross emitted
`#extension GL_ARB_draw_instanced : require`, while the hardware compatibility
context reports GL 2.1/GLSL 1.20 and no ARB/EXT/ANGLE instanced-arrays extension.
See `instance-baseline-0.1.26-*`, `instance-builtin-baseline-0.1.26-*`, and
`instance-gl-capabilities-0.1.26.log`.

## Implementation

0.1.27 rewrites only the SPIRV-Cross-generated `gl_InstanceID` token to the
internal `vkgl_InstanceID` uniform, removes the unsupported draw-instanced
extension directive, and retains `SPIRV_Cross_BaseInstance` for firstInstance.
Pipeline link caches both uniform locations. The executor then repeats the
existing GL draw for each instance, setting instance ID and firstInstance before
each draw. Direct, indexed and both indirect command paths preserve these
parameters. `instanceCount=0` now emits no draw.

No unadvertised GL entry point is called. Single-instance shaders retain the same
GL draw path; coherent per-submit upload coalescing and dispatchable teardown are
unchanged. This is software emulation and its CPU/driver overhead can grow with
instance count.

## Public pixel and validation matrix

`vktest-instance.vert` makes InstanceIndex=0 select a triangle that misses the
center and InstanceIndex=1 select the covering triangle. The test uses dynamic
viewport/scissor and a legal initial texture transition. The matrix covers
`direct`, `indexed`, `indirect`, and `indexed-indirect`:

| Case | Parameters | Required/observed raw BGRA |
|---|---|---|
| one | instanceCount=1, firstInstance=0 | `663300ff` (clear) |
| two | instanceCount=2, firstInstance=0 | `007f00ff` (gradient) |
| first | instanceCount=1, firstInstance=1 | `007f00ff` (gradient) |
| zero | instanceCount=0 | `663300ff` (clear) |

All 16 installed-package cases passed with **zero validation events**. Evidence:
`instance-0.1.27-{direct,indexed,indirect,indexed-indirect}-{one,two,first,zero}.{rgba,stdout.log,stderr.log}`.
Build without executing GPU work via `build-0.1.27-tests.sh`; SPIR-V is generated
in the output directory and no new SPIR-V binary is committed.

The exact candidate library also passed all eight prior native
firstVertex/count/topology/word276 cases with the same guarded dword diffs; see
`instance-native-regression-0.1.27.log`. Installed dmabuf and wl_shm smoke each
completed vulkaninfo, 60-frame vkcube and five vkmark scenes with **zero
validation events** (`smoke-installed-0.1.27-*.log`).

## Noninstanced performance control

Four alternating pairs per scene compared exact 0.1.26 with the candidate at
640x480 FIFO, five seconds per run:

| Scene | 0.1.26 mean FPS | 0.1.27 mean FPS | Change |
|---|---:|---:|---:|
| vertex | 235.25 | 234.25 | -0.43% |
| shading | 190.50 | 189.00 | -0.79% |

Candidate paired wins were 1/4 in each scene, with one vertex tie. These small
differences are within the observed run variance; no performance improvement or
universal non-regression guarantee is claimed. Raw data:
`instance-ab-0.1.27.{jsonl,log}` and
`instance-benchmark-environment-0.1.27.log`.

## Package and publication

- Candidate, package and installed library SHA-256:
  `b67f1a246f9223f0a0f5d2a4ddc45d8dffe86d96729eb3ee57ac7c18e58698c9`.
- Deb SHA-256:
  `6c3a48f42eb6f046255698df0f709b649b052c47eadd097e2d701fef6b54b86c`.
- The signed AOSC repository has 28 entries; local MD5, InRelease, HTTPS,
  isolated apt update, server bytes and HTTPS deb bytes passed. Highest indexed
  vkgl-loonggpu is 0.1.27-1.
- Logs: `package-0.1.27.log`, `install-0.1.27.log`, `publish-0.1.27.log`.

The matrix does not prove every possible instanced shader or performance at high
instance counts. Existing indexed vertexOffset behavior was not changed by this
release.
