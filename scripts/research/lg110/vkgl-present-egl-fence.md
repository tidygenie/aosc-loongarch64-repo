# vkgl dmabuf present: EGL fence instead of glFinish (negative result)

## Scope

The installed vkgl 0.1.31 ended every dmabuf `QueuePresentKHR` with `glFinish`.
The isolated candidate created a real `EGL_KHR_fence_sync`, waited forever with
`eglClientWaitSyncKHR`, destroyed it, and retained `glFinish` on every failure.
It did not remove synchronisation or change wl_shm. The exact source change is
[`vkgl-present-egl-fence.patch`](vkgl-present-egl-fence.patch).

Installed library SHA-256:
`afac548f68ed43574b546b76e541110fba46e9c329f66a3318fcecc7dc4d2b3b`.
Candidate SHA-256:
`9ca099660320bb43d591ddf6cb9c77ca67fb97901aa869ea5108c89337889923`.

Both candidate presentation paths passed the published strict smoke suite:
vkcube plus five vkmark scenes, zero validation errors and no software renderer.
The candidate was never installed.

## Paired measurement

[`vkgl-present-egl-fence-benchmark.py`](vkgl-present-egl-fence-benchmark.py)
ran six 640x480 FIFO rounds with validation enabled, alternating installed→fence
and fence→installed. Every process ran vertex, texture and shading for four
seconds. All 12 processes passed, all 36 scene results were recorded, and every
log was required to identify `LoongGPU LG110 (vkgl GLES2)` with no
llvmpipe/swrast or validation error. Homebrew lanes remained active, so the
same-round pair and medians matter more than absolute FPS or the first-round
installed outlier.

| Scene | Installed median FPS | Fence median FPS | Median same-round change | Positive pairs |
|---|---:|---:|---:|---:|
| vertex | 168.0 | 166.5 | -0.61% | 2/6 |
| texture | 123.0 | 122.5 | -0.81% | 2/6 |
| shading | 152.0 | 146.5 | -1.29% | 1/6 |

The complete values and pair percentages are in
[`vkgl-present-egl-fence-summary.json`](vkgl-present-egl-fence-summary.json).
This is not evidence of a regression either: it is a failure to demonstrate a
reproducible benefit under this load. The candidate is rejected for performance
and the installed driver remains unchanged.

## OS update boundary recorded during this experiment

Only `spirv-llvm-translator 22.1.8`, `wayland-protocols 1.50` and vim 9.2.1310
were upgraded; simulation proved that this subset did not pull Mesa. Strict
installed-vkgl dmabuf/wl_shm smoke then passed with zero validation errors.
Mesa 26.2.6 was downloaded but not installed. Its file list replaces
`/usr/lib/libgbm.so.1.0.0`, which vkgl links directly, as well as Mesa EGL and
Vulkan layers. It therefore remains deferred until a separately controlled
candidate and full GPU regression review; no blind graphics-stack upgrade was
performed.
