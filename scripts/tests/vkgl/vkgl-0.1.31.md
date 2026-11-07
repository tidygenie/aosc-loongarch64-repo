# vkgl-loonggpu 0.1.31-1 instance-rate attribute evidence (2026-11-07)

## Defect and fix

0.1.30 preserved independent bindings but kept every attribute pointer fixed
across software-instanced repeated draws. The new public test uses vertex-rate
positions/UVs plus instance-rate binding 2 with float variants `{0,1}` and
stride 4. On exact 0.1.30, eight two-instance/firstInstance=1 cases remain green
(`008000ff`) and fail the red expectation; four zero-instance controls stay clear
(`663300ff`). Raw files, stderr, exit codes and logs are retained as
`instance-rate-baseline-0.1.30-*`.

0.1.31 advances only attributes whose binding inputRate is INSTANCE:

```text
voff + attribute.offset + (firstInstance + instance) * stride
```

Vertex-rate pointers remain fixed. No unadvertised GL divisor/instanced-array
entry point is called; software instancing remains repeated GL draws.

## Validation

All twelve nonindexed/indexed × direct/indirect × two/first/zero cases pass on
the exact public candidate and after installation. Two-instance and
firstInstance=1 produce red; the tested swapchain is B8G8R8A8, so raw red is
`0000ffff`. Zero instances produce clear. Validation events are zero.
Evidence: `instance-rate-0.1.31-indexed{0,1}-indirect{0,1}-{two,first,zero}.*`.

All 36 earlier functional/independent-binding cases and all 21 previous native
causal/guard cases pass. Candidate and installed dmabuf/wl_shm vulkaninfo,
vkcube and five-scene vkmark smoke have zero validation events. See
`instance-rate-regression-0.1.31.log` and `smoke-installed-0.1.31-*.log`.

No new native packet field is inferred from this compatibility fix. Existing
word268/269/270/274 and guarded mutations remain regression tests, not a
complete native vertex/attribute decoder.

## Performance control

Three alternating pairs per scene, 640x480 FIFO, five seconds per run:

| Scene | 0.1.30 mean FPS | 0.1.31 mean FPS | Change |
|---|---:|---:|---:|
| vertex | 234.67 | 237.67 | +1.28% |
| shading | 190.67 | 192.33 | +0.87% |

These small differences are not claimed as a reproducible performance gain.
Raw data: `instance-rate-ab-0.1.31.{jsonl,log}` and
`instance-rate-benchmark-environment-0.1.31.log`.

## Release

- Public candidate, package and installed library SHA-256:
  `afac548f68ed43574b546b76e541110fba46e9c329f66a3318fcecc7dc4d2b3b`.
- Deb SHA-256:
  `938aebc08a494b2b5b01c3d970bacf48633d7874c8e09831a46ceb30a8e90b12`.
- Signed AOSC publication passed dry-run/deploy, 32-entry indexes, MD5,
  InRelease, HTTPS and isolated apt checks. Server and HTTPS deb bytes match;
  highest indexed vkgl-loonggpu is 0.1.31-1.
- Logs: `package-0.1.31.log`, `install-0.1.31.log`, `publish-0.1.31.log`.

This remains Vulkan-over-GL with software instancing, not native Vulkan,
zero-copy vertex fetching or maximum-performance proof.
