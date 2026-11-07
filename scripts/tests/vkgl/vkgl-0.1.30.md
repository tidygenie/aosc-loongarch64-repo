# vkgl-loonggpu 0.1.30-1 independent vertex-binding evidence (2026-11-07)

## Defect and fix

Exact 0.1.29 stored only one VkVertexInputBindingDescription and one executor
VBO/offset. Multiple bindings therefore overwrote each other. In the new public
test, binding 0 supplies positions with binding offset 8 and stride 8, while
binding 1 supplies UVs with stride 16. Both direct and indirect 0.1.29 runs
returned clear `663300ff` instead of gradient `007f00ff`; raw files, exit codes
and logs are retained as `binding-baseline-0.1.29-*`.

0.1.30 stores all 16 advertised binding descriptions and executor VBO/offset
slots. Each vertex attribute selects its own binding for upload, stride and
`voff + attribute.offset`. The final direct/indexed × direct-recorded/indirect
matrix has four installed cases, all with correct gradient and zero validation
events. Evidence: `binding-0.1.30-indexed{0,1}-indirect{0,1}.*`.

All 32 earlier instance/index-type/signed-offset functional cases and all 21
previous native causal/guard cases pass on the exact public candidate. Candidate
and installed dmabuf/wl_shm vulkaninfo, vkcube and five-scene vkmark smoke have
zero validation events. See `binding-regression-0.1.30.log` and
`smoke-installed-0.1.30-*.log`.

## Native observations and limits

The two VBOs generate distinct XDMA uploads. Their base VAs do not occur as
literal 32-bit words in the 672-dword GFX IB; only the index VA still appears at
word280. Direct→indirect changes only the established 12 VA-bearing positions.
No vertex-address semantics or active address mutation are claimed. See
[`../../research/lg110/lg110-vertex-binding-stream.md`](../../research/lg110/lg110-vertex-binding-stream.md)
and `draw-vertex-binding-public-*` there.

## Performance control

Three alternating pairs per scene, 640x480 FIFO, five seconds per run:

| Scene | 0.1.29 mean FPS | 0.1.30 mean FPS | Change |
|---|---:|---:|---:|
| vertex | 234.67 | 230.33 | -1.85% |
| shading | 177.00 | 176.33 | -0.38% |

This small sample does not establish a regression or improvement. Raw data:
`binding-ab-0.1.30.{jsonl,log}` and
`binding-benchmark-environment-0.1.30.log`.

## Release

- Public candidate, package and installed library SHA-256:
  `bebbb931fa10f79ad90b6b8a80dbe6296f5ebeaaf961bd29bccf9c48a62acdab`.
- Deb SHA-256:
  `1226bdeabffdc69083862a02b6f05c1aaffa18adc7aa8810ca3c3dab81eb22f7`.
- Signed AOSC publication passed dry-run/deploy, 31-entry indexes, MD5,
  InRelease, HTTPS and isolated apt checks. Server and HTTPS deb bytes match;
  highest indexed vkgl-loonggpu is 0.1.30-1.
- Logs: `package-0.1.30.log`, `install-0.1.30.log`, `publish-0.1.30.log`.

This remains Vulkan-over-GL with software instancing. Per-binding vertex-rate
state is preserved, but instance-rate attribute stepping is not implemented by
this release. No native Vulkan or maximum-performance claim is made.
