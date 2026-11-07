# vkgl-loonggpu 0.1.29-1 signed vertexOffset evidence (2026-11-07)

## Defect and fix

Exact 0.1.28 already carried firstIndex and UINT16/UINT32 correctly but ignored
signed vertexOffset. With indices `{0,1,2,1,2,3}` and a safe four-vertex shader:

- base (0,0) and firstIndex-only (3,0) passed on direct and indirect paths;
- positive (0,+1) incorrectly stayed gradient instead of clear;
- negative cancellation (3,-1) incorrectly stayed clear instead of gradient.

All eight 0.1.28 baseline raw files, exit codes and logs are retained as
`offset-baseline-0.1.28-*`.

0.1.29 uses the hardware-advertised `GL_ARB_draw_elements_base_vertex` entry only
when vertexOffset is nonzero. `base-vertex-capability-0.1.29.log` records both
the extension and resolved `glDrawElementsBaseVertex` entry. Zero-offset draws
keep the previous glDrawElements path. If the entry is absent, the driver reports
it and does not silently issue an offset-ignored draw.

The installed matrix covers UINT16/UINT32 × direct/indirect ×
base/positive/negative/firstIndex: all **16 cases** pass raw pixels with zero
validation events. Evidence: `offset-0.1.29-u{16,32}-indirect{0,1}-*`.
The earlier 16-case instance/index-type matrix also passes on exact 0.1.29;
candidate and installed dmabuf/wl_shm vulkaninfo, vkcube and five-scene vkmark
smoke have zero validation events. See `index-offset-regression-0.1.29.log`.

## Native fields

Passive differentials isolate word269 for firstIndex and word274 for signed base
vertex. Guarded active mutations prove:

- word269 `0→3` alone reproduces firstIndex=3 clear;
- word274 `0→1` alone reproduces vertexOffset=+1 clear;
- word274 `0xffffffff→0` while keeping firstIndex=3 removes negative-offset
  cancellation and produces clear.

Each active case has unchanged XDMA and exactly one changed GFX dword; two
wrong-input guards have zero changes. All earlier nonindexed and index-type
native regressions pass. See
[`../../research/lg110/lg110-index-offset-field.md`](../../research/lg110/lg110-index-offset-field.md)
and `draw-index-offset-public-*` there.

## Performance controls (no stable attribution)

Three initial pairs followed by five nonconcurrent repeat pairs, 640x480 FIFO,
five seconds per run:

| Set | Scene | 0.1.28 mean FPS | 0.1.29 mean FPS | Change |
|---|---|---:|---:|---:|
| initial 3 | vertex | 194.00 | 198.00 | +2.06% |
| initial 3 | shading | 156.67 | 144.67 | -7.66% |
| repeat 5 | vertex | 115.00 | 121.80 | +5.91% |
| repeat 5 | shading | 99.20 | 104.40 | +5.24% |

The repeat set had much lower absolute FPS and the initial shading set moved in
the opposite direction. These sets demonstrate system/noise sensitivity; neither
a regression nor an improvement is attributed to this correctness fix. Both raw
sets are retained as `index-offset-ab-0.1.29-{initial,repeat}.{jsonl,log}`.

## Release

- Public candidate, package and installed library SHA-256:
  `5ad81dfb43c85a39439130fb46d770a698228c5618a66feb8e2fb4361ab13614`.
- Deb SHA-256:
  `01067fc80517c5f99d51eee559e4aa13e5b85fc14fa461c61328ac4b376da7fb`.
- Signed AOSC publication passed dry-run/deploy, 30-entry indexes, MD5,
  InRelease, HTTPS and isolated apt checks. Server and HTTPS deb bytes match;
  highest indexed vkgl-loonggpu is 0.1.29-1.
- Logs: `package-0.1.29.log`, `install-0.1.29.log`, `publish-0.1.29.log`.

This remains Vulkan-over-GL with software instancing, not native Vulkan, native
hardware instancing, a complete packet decoder or maximum-performance proof.
