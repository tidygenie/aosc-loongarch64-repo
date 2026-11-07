# vkgl-loonggpu 0.1.28-1 UINT32 index evidence (2026-11-07)

## Defect and fix

Exact 0.1.27 preserved instanceCount/firstInstance but hardcoded every indexed
draw to `GL_UNSIGNED_SHORT`. On a legal UINT32 `{0,1,2}` index buffer, both
direct and indexed-indirect firstInstance=1 cases returned clear `663300ff` and
exit 1 instead of gradient `007f00ff`; their UINT16 controls passed.

0.1.28 records the bound VkIndexType in command-buffer state, snapshots it into
each DrawIndexed and DrawIndexedIndirect command, and selects GL_UNSIGNED_INT or
GL_UNSIGNED_SHORT at execution. Recording-time capture prevents a later rebind
from changing an already-recorded draw. No other draw state optimization is
introduced.

The installed public matrix covers UINT16 and UINT32, direct and indirect, and
one/two/firstInstance/zero instances: all **16 cases** pass raw pixels with zero
validation events. Evidence:
`index-0.1.28-u{16,32}-indirect{0,1}-{one,two,first,zero}.*`; failing and passing
0.1.27 baselines are retained as `index-baseline-0.1.27-*`.

## Native field consequence

On exact public 0.1.28, passive UINT16→UINT32 changes only GFX word268
`0x40000402→0x40000404`; word281 stays 1. A guarded active reverse mutation
changes only word268 back to `0x40000402`, leaves both XDMA IBs unchanged, and
exactly reproduces the 0.1.27 clear output. UINT16 input rejection has zero IB
changes. Thus word268 has direct index-width causal evidence in this stream;
word281 remains unassigned. See
[`../../research/lg110/lg110-index-type-field.md`](../../research/lg110/lg110-index-type-field.md)
and `draw-index-type-public-*` under that directory.

All eight earlier firstVertex/count/topology/word276 regressions pass on the
exact candidate (`index-type-native-regression-0.1.28.log`). Candidate and
installed dmabuf/wl_shm vulkaninfo, vkcube and five-scene vkmark smoke have zero
validation events (`smoke-installed-0.1.28-*.log`).

## Ordinary-scene performance control

Three alternating pairs per scene, 640x480 FIFO, five seconds per run:

| Scene | 0.1.27 mean FPS | 0.1.28 mean FPS | Change |
|---|---:|---:|---:|
| vertex | 220.67 | 219.33 | -0.60% |
| shading | 178.67 | 184.00 | +3.21% |

The shading candidate won 2/3 pairs while vertex had one tie and one win. These
are noisy small-sample results, not a performance improvement claim. Raw data:
`index-type-ab-0.1.28.{jsonl,log}` and
`index-type-benchmark-environment-0.1.28.log`.

## Release

- Public candidate, package and installed library SHA-256:
  `0688025cfcef4bdfe86a30fdbd15a6479f28acd7449d56a2f501ec1385f6be3c`.
- Deb SHA-256:
  `ebd7df024274e783a665e786088496a579ea8fc3a60b3f5c10e64a2e3eab4a0d`.
- Signed AOSC publication passed dry-run/deploy, 29-entry indexes, MD5,
  InRelease, HTTPS and isolated apt checks. Server and HTTPS deb bytes match;
  highest indexed vkgl-loonggpu is 0.1.28-1.
- Logs: `package-0.1.28.log`, `install-0.1.28.log`, `publish-0.1.28.log`.

This remains Vulkan-over-GL with software instancing. It is not native Vulkan,
native hardware instancing, complete indexed-state support or a maximum-performance
claim. Existing indexed vertexOffset behavior is unchanged.
