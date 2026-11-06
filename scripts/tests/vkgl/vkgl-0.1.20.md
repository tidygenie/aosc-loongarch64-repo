# vkgl-loonggpu 0.1.20-1 (2026-11-06)

This remains a Vulkan-1.0-subset translation layer over the vendor GL/GLES
stack. It is **not native Vulkan**. The release contains two independently
validated changes; unrelated rejected hot-path experiments are documented in
[`hotpath-experiments-20261106.md`](hotpath-experiments-20261106.md).

## Changes

1. **Tight `CopyBufferToImage` direct source path.** When
   `bufferRowLength` is zero or equals the copied width, the validated source
   rows are already contiguous. Optimal-tiling uploads now pass that pointer
   directly to `glTexSubImage2D` instead of allocating and filling another
   packed CPU snapshot. Padded rows retain the old path. Linear destinations
   also retain a packed snapshot when source and destination use the same
   allocation, preventing shadow writes from overwriting later input rows.
   Validation and the dmabuf presentation path are unchanged.
2. **Real queue-submission fences.** With runtime-probed
   `EGL_KHR_fence_sync`, queue submission creates an
   `EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR` sync and flushes it. Status/wait
   operations observe actual completion, including waitAll/waitAny, reset,
   never-submitted zero-timeout waits, shared deadlines, and cleanup. Missing
   extension/entry points or sync creation failure conservatively use
   `glFinish`. Acquire fences remain host-signaled. The dmabuf present
   `glFinish()` is deliberately unchanged.

## Exact artifacts

- Installed `/usr/lib/vkgl/libvkgl.so` SHA-256:
  `d9aad6dea8cb6b39e538722cb8b1fccb8f3bc9aa44380dc5944725d0eea0067d`.
- `vkgl-loonggpu_0.1.20-1_loongarch64.deb` SHA-256:
  `1665530c2b600172cbd2ee00ed882f92bb10458078a36265d2357e9d404184a7`.
- The package library and a fresh build from the public source were
  byte-identical. `dpkg-query` reports `vkgl-loonggpu 0.1.20-1 loongarch64`.
- [Published deb](https://homebrewloongarch64.site/aosc/pool/main/v/vkgl-loonggpu/vkgl-loonggpu_0.1.20-1_loongarch64.deb).
  The 19-package signed index, MD5/SHA metadata, HTTPS, isolated apt update,
  and remote deb bytes all verified.

## Installed 0.1.19 versus 0.1.20 paired upload timing

The exact 0.1.19 deb library (`04e51457…`) and installed 0.1.20 library were
loaded by two explicit manifests. The **same public test binary** ran six
correctness cases before each benchmark. Five process pairs alternated order;
each process measured seven batches of 64 1024x1024 RGBA optimal-tiling
uploads. Recording/allocation were outside the timed
`QueueSubmit + QueueWaitIdle` interval.

| Mode | 0.1.19 median seconds | 0.1.20 median seconds | Throughput change | Faster pairs |
|---|---:|---:|---:|---:|
| tight, rowLength=0 | 0.682755353 | 0.640047151 | **+6.67%** | **5/5** |
| padded reference | 0.685325511 | 0.688964799 | -0.53% | 0/5 |

The median-time-derived tight throughputs are 374.95 and 399.97 MiB/s.
The padded control does not improve. These numbers apply only to this upload
microbenchmark, not vkmark FPS, native Vulkan, or all applications. Raw logs
and pair data are `tight-upload-installed-ab-0.1.20.{log,json}`. An earlier
independent tight-only candidate also produced +7.03%, 5/5, with a -0.41%
padded control (`tight-upload-ab-0.1.20.{log,json}`).

## Correctness and hardware regression matrix

- Public strict-warning test build succeeded; 11 deterministic fence state
  machine cases passed without GPU.
- Vendor EGL reported `EGL_KHR_fence_sync=1`. The installed 0.1.19 negative
  control incorrectly returned success for a never-submitted zero-timeout
  wait. Installed 0.1.20 returned `VK_TIMEOUT`, then passed real submission
  waitAll, reset, zero-timeout waitAny, reuse without semaphores, and exact
  `663300ff` pixel readback.
- Tight/padded six-case offscreen comparisons passed: nonzero buffer offset,
  a 17x11 subrectangle at image offset (5,7), rowLength 0/width, optimal and
  linear tiling, untouched borders, and full expected pixels.
- Installed dmabuf and wl_shm suites each passed 5/5: device enumeration,
  60-frame vkcube, five vkmark scenes, zero validation errors, and GPU-only
  mappings. The two-second FPS lines in the logs are smoke results only.
- Published tight/padded/CopyImage/ClearAttachments A/B tests all passed with
  exact expected pixels, guards, and overwritten sentinels.
- Acquire exclusivity/fence passed on both paths. Capturing during the live
  three-second copy-test window produced exactly 153600 red and 153600 blue
  pixels on both dmabuf and wl_shm.

Logs: `smoke-0.1.20-installed-{dmabuf,shm}.log`,
`submit-fence-{host,gpu}-0.1.20*.log`, and
`pixel-regressions-0.1.20-installed.log`.

Build the public tests without running them:

```sh
scripts/tests/vkgl/build-0.1.20-tests.sh /tmp/vkgl-0.1.20-tests
/tmp/vkgl-0.1.20-tests/vktest-submit-fence
/tmp/vkgl-0.1.20-tests/vktest-tight-upload --check
```

The Wayland fence integration additionally needs the active hardware session
and `LG110_EXPECT_CLEAR_RED=0` plus an `LG110_READBACK_PIXEL` output path.
