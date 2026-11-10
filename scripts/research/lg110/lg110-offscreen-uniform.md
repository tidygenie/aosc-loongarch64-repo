# Clean offscreen clear and uniform-array controls

Date: 2026-11-11

## Supersedes the driver-anomaly interpretation of the old array harness

The earlier [`lg110-repeat-submit.md`](lg110-repeat-submit.md) preserved an
unexplained failure in the original Wayland-derived array harness. A clean
headless control now passes on the **same installed vkgl 0.1.35**, including all
three changing uniform frames. Therefore the old red/red/red results must not be
used as evidence of a production clear or uniform driver bug. The old harness
is retained only as a historical diagnostic; its unresolved construction issue
is separate from the candidates below.

## Clean controls

[`vktest-offscreen-clear.c`](vktest-offscreen-clear.c) creates no surface,
swapchain, shader, or graphics pipeline. It performs three exact clears and
fence-waited readbacks on an optimal BGRA image. The 64x64 single-pixel version,
640x480 version, and 640x480 dual-pixel/accumulated-offset version all pass
red, green, and blue exactly on installed vkgl.

[`vktest-offscreen-uniform.c`](vktest-offscreen-uniform.c) is derived from that
passing control. It adds only the nine-vec4 UBO, graphics pipeline, top-half
scissor draw, and deferred `vkCmdUpdateBuffer` sequence. It remains headless and
uses two exact pixel reads per frame:

| Frame | Expected top BGRA | Expected bottom BGRA |
|---|---|---|
| 0 | `0000ffff` (red) | `000000ff` |
| 1 | `1010ffff` (red plus 16) | `000000ff` |
| 2 | `ff1010ff` (blue plus 16) | `000000ff` |

The 24-byte result is exactly:

```text
0000ffff000000ff1010ffff000000ffff1010ff000000ff
```

Installed vkgl, range instrumentation, and the clean coalesced candidate all
return this exact buffer with `valid_mask=7` and zero validation errors.

Candidate identities:

- installed: `78c963855189836046520f1ee9f327057bd61d7a75c2b8e40b8988f3ea473668`
- range: `b6d1872b6204f4c99f59204db1ce74b0200f00c2a3865fcb9306933839819cde`
- coalesced: `1243dc431ac6534a43d85854608851b696752199554d8c93ccb2bec6cbfca116`

The clean coalesced source was reconstructed without later rebind/glGet debug
experiments; rebuilding it reproduced the original `1243dc...` binary hash
byte-for-byte, so the counter run and correctness run use the same implementation.

## Additional exact regressions

Both candidates also pass the existing CPU-shadow `vkCmdUpdateBuffer`, fragment
uniform, vertex upload, indirect upload, and scissor tests on the real LG110.
Each Wayland test uses its required single-frame settings; indirect explicitly
sets `LG110_DRAW_INDIRECT=1`. Every final run has zero validation errors.

An earlier invocation incorrectly used the default 120-frame settings for tests
that require one frame. Those parameter-error logs and the old scissor
semaphore-reuse diagnostics are not candidate results and are not used here.

## Paired performance result: both candidates rejected

The established serial AB/BA harness ran six rounds per candidate on the real
LG110 at 640x480, with validation enabled and all three vkmark scenes measured
for four seconds. Every run exited normally, used `LoongGPU LG110 (vkgl GLES2)`,
and contained no software-renderer or validation-error marker. No outlier was
removed.

| Candidate | Scene | Baseline median FPS | Candidate median FPS | Paired median | Positive pairs |
|---|---|---:|---:|---:|---:|
| range | vertex | 225.5 | 219.0 | -2.89% | 1/6 |
| range | texture | 151.0 | 153.0 | -0.32% | 2/6 |
| range | shading | 191.5 | 193.5 | +0.52% | 3/6 |
| coalesced | vertex | 224.0 | 225.5 | +2.68% | 4/6 |
| coalesced | texture | 153.5 | 153.0 | +0.33% | 3/6 |
| coalesced | shading | 190.0 | 187.5 | -2.58% | 1/6 |

Range has a repeated vertex regression. Coalescing has a repeated shading
regression despite the vertex sample moving upward. Small movements in the
other scenes do not justify promoting either change. Installed production vkgl
remains 0.1.35; there is no version bump, deb, or performance claim.

Raw aggregate data:
[`uniform-range-paired-summary.json`](uniform-range-paired-summary.json) and
[`uniform-coalesce-paired-summary.json`](uniform-coalesce-paired-summary.json).

## Evidence

- [`offscreen-clear-installed.log`](offscreen-clear-installed.log)
- [`offscreen-clear-large-dual.log`](offscreen-clear-large-dual.log)
- [`offscreen-uniform-installed.log`](offscreen-uniform-installed.log)
- [`offscreen-uniform-range.log`](offscreen-uniform-range.log)
- [`offscreen-uniform-coalesce.log`](offscreen-uniform-coalesce.log)
- [`uniform-candidate-regressions.txt`](uniform-candidate-regressions.txt)

These results establish correctness for the tested paths only. They do not
establish performance, native Vulkan, zero-copy, or generalized numeric
conformance. No candidate has been installed or packaged.
