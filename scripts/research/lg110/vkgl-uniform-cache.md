# Rejected vkgl content-keyed uniform upload cache (2026-11-11)

## Hypothesis and candidate

The fragment-uniform BO investigation in
[`lg110-uniform-state.md`](lg110-uniform-state.md) showed that uniform uploads
reach vendor state outside the captured IB. `vkgl_execute_draw` nevertheless
calls `glUniform4fv` for every matching descriptor on every draw.

An isolated candidate added a byte vector to each `Pipeline::PipeUbo`. It
skipped `glUniform4fv` only when the complete flattened value was byte-identical
to the previously uploaded value, and otherwise uploaded and replaced the cached
bytes. Descriptor switches, dynamic offsets, mapped writes, and UpdateBuffer
therefore remained content-driven rather than revision-driven. Per-UBO hit and
upload counters were added for this experiment.

The candidate used the unchanged production flags
`-O3 -march=loongarch64 -mtune=la664 -flto`. Candidate library SHA-256:
`d66ff7296ec011fdb0ce34715309c58a64c7fbbf6358b107dc30ad5375065a2a`.
The readable source change is
[`vkgl-uniform-cache.patch`](vkgl-uniform-cache.patch), SHA-256
`525fe5a4fddc88a42037af451092f9b801adb445c3d79470ebe3eb0719d2fbb8`.

## Correctness controls passed

Before measuring cache hits, the candidate passed the public deferred
UpdateBuffer test, exact fragment-uniform red/black test, and exact vertex
consumer test with Khronos validation enabled. All three logs contain zero
`Validation Error:` records. The uniform and vertex raw pixels matched the
installed 0.1.35 references byte-for-byte.

These controls establish that the experiment did not break the tested paths;
they do not justify promotion by themselves.

## Actual default-workload hit rate

Serial 640x480 FIFO vkmark runs used the candidate ICD and `VKGL_DEBUG=1` only
to expose the per-UBO counters. Debug FPS is not a performance comparison.

| Scene | Cache hits | Actual uploads |
|---|---:|---:|
| vertex | 0 | 360 |
| texture | 0 | 240 |
| shading | 0 | 240 |

Every draw's uniform bytes changed. No GL upload was skipped in any measured
scene, so the candidate added comparison/bookkeeping work without a measured
benefit. There is no reason to run a paired FPS promotion series for this
candidate on these workloads.

Raw debug logs:

- [`vkgl-uniform-cache-stats-shading.log`](vkgl-uniform-cache-stats-shading.log),
  SHA-256 `8491688d90a09b97090092a9ec42a30f426f5f2e8e6b48323d4cad56bffcb0ba`.
- [`vkgl-uniform-cache-stats-vertex-texture.log`](vkgl-uniform-cache-stats-vertex-texture.log),
  SHA-256 `03b35084e6857e7bb29d277d3ff725a1a57297abeb82a23ccf7077e549d5e333`.
- [`vkgl-uniform-cache-summary.txt`](vkgl-uniform-cache-summary.txt) also records
  the zero-validation-error exact regressions.

## Decision and remaining scope

The candidate is rejected, not installed, and not packaged. Installed vkgl
0.1.35 remains SHA-256
`78c963855189836046520f1ee9f327057bd61d7a75c2b8e40b8988f3ea473668`.

This is a default-vkmark rejection, not proof that no application can have
static uniforms. A future candidate needs a controlled repeated-draw/static-
uniform workload that first demonstrates substantial real cache hits and then
shows paired latency or FPS improvement without changing exact pixels. No such
result is claimed here.
