# LG110 repeated-submit anomaly discovered during uniform-range research

Date: 2026-11-11

## Status

A repeatable three-frame workload does not observe later clear or uniform states
in its readbacks on installed vkgl 0.1.35, even with zero Khronos validation
errors. The root cause is **not yet assigned** to vkgl, the vendor GL driver, or
swapchain-image usage. No driver change or performance optimization is promoted
from this experiment.

The workload was initially built to validate a uniform-upload optimization. A
clear-only control failed on the unmodified installed driver, so it cannot yet
certify a cache candidate. This report deliberately preserves that negative
result instead of treating candidate FPS as valid.

## Uniform-range measurements that motivated the test

The default vkmark UBO has nine vec4 elements. Whole-block caching found zero
hits in round 83, but individual elements show partial reuse:

| Upload strategy | Total vec4 | Uploaded/changed vec4 | GL upload calls/runs |
|---|---:|---:|---:|
| vertex, exact 16-byte runs | 3240 | 1672 | 1210 |
| texture, exact 16-byte runs | 2160 | 1443 | 479 |
| shading, exact 16-byte runs | 2160 | 1121 | 801 |
| vertex, coalesced | 3240 | 2522 | 360 |
| texture, coalesced | 2160 | 1682 | 240 |
| shading, coalesced | 2160 | 1682 | 240 |

Exact runs reduce data but split one original call into many calls. Coalescing
changes separated by at most three unchanged vec4 elements retains one call per
nonidentical update but still uploads about 78% of the original data. Per-index
counters show that indices 0, 2, 4, 5, and 6 change while 7 and 8 usually remain
static.

The range instrumentation library was SHA-256
`b6d1872b6204f4c99f59204db1ce74b0200f00c2a3865fcb9306933839819cde`;
its readable patch is
[`vkgl-uniform-range.patch`](vkgl-uniform-range.patch), SHA-256
`97fe464ab9ce67b3b06329bd8d707bebad492a79e2a55069d9c2f6b86a5ffa2f`.
The initial coalesced instrumentation library was SHA-256
`1243dc431ac6534a43d85854608851b696752199554d8c93ccb2bec6cbfca116`.

These are counter measurements only. Debug FPS is not a paired benchmark. See
[`uniform-range-summary.txt`](uniform-range-summary.txt) for the counters and
raw-log hashes.

## Three-frame test construction

[`vktest-uniform-array.c`](vktest-uniform-array.c) uses one acquired swapchain
image, three sequential command-buffer submissions, and a fence wait plus two
one-pixel readbacks after each submission. It presents only after the third
submission. The first submission alone waits for the acquire semaphore, and the
last alone signals the present semaphore. This removes the semaphore-reuse
validation errors present in the discarded initial version.

[`build-uniform-array.sh`](build-uniform-array.sh) compiles the workload and
text shaders without running GPU work. Workload SHA-256:
`3d5ad6acce53b90928e98da7a2809630ca2d114f49aef80be44a930d0d582fba`.

The normal mode uses [`uniform-array.frag`](uniform-array.frag) and a 144-byte
nine-vec4 buffer:

1. Initial full state: red, zero prefix.
2. Change prefix indices 0, 2, 4, and 5 to 0.25 while keeping the red tail;
   expected top BGRA is `1010ffff`.
3. Change the tail to blue, move the 0.25 contribution from index 0 to index 6;
   expected top BGRA is `ff1010ff`.

The bottom half remains a black clear in this mode. A tail-only shader,
[`uniform-tail.frag`](uniform-tail.frag), removes the prefix arithmetic. A
clear-only mode skips the draw and requests full-image red, green, and blue
clears in successive submissions.

## Exact results on installed vkgl 0.1.35

Installed library SHA-256:
`78c963855189836046520f1ee9f327057bd61d7a75c2b8e40b8988f3ea473668`.
All final runs below had zero `Validation Error:` records and nonzero exits
because their exact pixel checks failed.

| Mode | Expected top pixels by frame | Actual top pixels by frame |
|---|---|---|
| array delta | red, red+16, blue+16 | red, red, red |
| tail only | red, red, blue | red, red, red |
| clear only | red, green, blue | red, red, red |

Raw logs:

- [`repeat-submit-array.log`](repeat-submit-array.log)
- [`repeat-submit-tail.log`](repeat-submit-tail.log)
- [`repeat-submit-clear.log`](repeat-submit-clear.log)

Because clear-only fails without using fragment uniforms, the anomaly is not
specific to the proposed uniform cache, `vkCmdUpdateBuffer`, or the nine-vec4
shader.

## Negative controls and excluded explanations

The following isolated experiments did not make the later frames appear:

- persistent host-mapped uniform writes instead of `vkCmdUpdateBuffer`;
- nine individual `glUniform4fv` calls instead of one array call;
- `glUseProgram(0)` followed by rebinding the program for each draw;
- `glFinish()` immediately before `glReadPixels` in `CopyImageToBuffer`.

The final glFinish clear-only control is
[`repeat-submit-clear-finish.log`](repeat-submit-clear-finish.log), byte-identical
to the installed clear log. Its isolated library SHA-256 is
`a94f0dbbae6febd6e77391e67c9403db6c65c46af7d041272b9dd6fd3df0cb80`.
A debug `glGetUniformfv` check observed the new host-side GL program values with
`glGetError()==0`; that alone does not prove what the GPU consumed.

## Complete clear-only IB differential

For clear-only, each of the three GFX submissions contains 272 dwords. Between
successive frames, only two state-address dwords change:

```text
seq2 -> seq4: word110 0x010c7000 -> 0x010cfc00
              word242 0x010c8400 -> 0x010d1000
seq4 -> seq6: word110 0x010cfc00 -> 0x010d8800
              word242 0x010d1000 -> 0x010d9c00
```

There is no literal red/green/blue field in these IBs. The referenced BO state
must be decoded before assigning a reason; the pointer changes alone do not
prove that the new clear values reached the correct hardware input.

Report:
[`repeat-submit-clear-ib-differential.txt`](repeat-submit-clear-ib-differential.txt),
SHA-256 `a6ef92e5c71ddad7ea01efe8c752c59238ab69fa2d6be805cd9dad940d4b58f1`.
Raw trace: [`repeat-submit-clear-full.trace.log`](repeat-submit-clear-full.trace.log).

## Decision and next isolation

Uniform-range/coalescing candidates receive no paired FPS claim, installation,
package version, or deb while the baseline multi-submit control fails. The
installed library remains unchanged.

The next isolation should render to a non-swapchain offscreen image with the
same three clear submissions and readbacks. That will remove acquire/present
semantics entirely. Only after the clear-only control passes can this harness
certify uniform-cache correctness or performance. No maximum-performance,
native-Vulkan, zero-copy, or general vendor-bug claim is made here.
