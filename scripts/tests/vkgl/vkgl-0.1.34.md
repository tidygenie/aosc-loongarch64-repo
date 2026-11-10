# vkgl-loonggpu 0.1.34-1 scissor origin correction

Date: 2026-11-11

## Correctness fix

0.1.33 passed Vulkan `VkRect2D` scissor coordinates directly to OpenGL even
though Vulkan uses a top-left origin and OpenGL uses a bottom-left origin.
0.1.34 converts only the Y coordinate while executing `Cmd::SetScissor`:

```c
gl_y = fb_height - sc.offset.y - sc.extent.height;
```

The intermediate calculation uses `int64_t`. Command recording, viewport state,
draw dispatch, primitive-restart handling, upload paths, and native command
submission are otherwise unchanged.

This is a correctness release for Vulkan-over-GLES2. It is not a native Vulkan
implementation and does not claim a performance improvement.

## Independent exact-pixel test

`vktest-scissor.c` retains the real Wayland/swapchain path. It clears to exact
black, draws a solid-red fullscreen triangle through scissor
`{offset={0,0}, extent={640,240}}`, and copies pixels at `(320,120)` and
`(320,360)` into an eight-byte readback buffer. There is no pixel tolerance.
The observed swapchain format was `B8G8R8A8_UNORM`.

| Driver | Top pixel | Bottom pixel | Result |
| --- | --- | --- | --- |
| Installed 0.1.33 negative control | `000000ff` black | `0000ffff` red | fails as expected |
| Candidate 0.1.34 | `0000ffff` red | `000000ff` black | exact pass |
| Public test rebuilt from Git sources | `0000ffff` red | `000000ff` black | exact pass |
| Installed 0.1.34 package | `0000ffff` red | `000000ff` black | exact pass |

All runs enumerated `LoongGPU LG110 (vkgl GLES2)`. Build the suite with
`build-0.1.34-tests.sh`, run from its `scissor` directory, set
`LG110_READBACK_PIXEL` and `LG110_FRAME_LIMIT=1`, and use the normal vkgl ICD
and Wayland environment. The 0.1.33 negative control must exit nonzero; the
fixed driver must exit zero.

## Regression controls

The candidate passed seven serial primitive-restart cases with exact pixels:
direct UINT16/UINT32 triangle-strip, direct UINT16 triangle-fan, indirect
UINT16/UINT32 strip, restart-disabled, and a two-instance restarted draw.
The ordinary indexed list path also retained its exact `007f40ff` pixel.
A 120-frame mapped-index test changed the restart sentinel on the second frame;
installed 0.1.33 and the candidate both produced exact `007f40ff` after the
update. The existing strict `vktest-draw-instance` binary SHA-256 was
`3e9489527171493a19c7435113b7895273bedc35c7ccaec8573829e196230c42`.

Six serial AB/BA vkmark rounds used the exact candidate, 640x480 FIFO,
four seconds per scene, and Khronos validation. Wrong-GPU, software-renderer,
validation-error, missing-FPS, and failed-process guards all passed.

| Scene | Baseline median FPS | Candidate median FPS | Median paired change | Pair range |
| --- | ---: | ---: | ---: | ---: |
| vertex | 192.5 | 187.0 | -1.55% | -14.29% to +5.73% |
| texture | 129.0 | 131.5 | +1.94% | -2.99% to +13.22% |
| shading | 163.5 | 162.0 | +1.18% | -12.12% to +10.19% |

These noisy results are a default-path regression control only. The vertex
result must not be presented as a gain, and the positive texture/shading
medians do not establish a speedup. The raw summary SHA-256 is
`741185b9a8192ecbb31d8001465d62a264cc4be8606c670c57cb8dd19ced9fb9`.

After package installation, the public scissor test passed byte-for-byte and
the standard Vulkan smoke suite passed 5/5 checks: LG110 enumeration, vkcube,
five vkmark scenes, zero validation errors, and no llvmpipe mapping.

## Package, installation, and publication

- Package: `vkgl-loonggpu_0.1.34-1_loongarch64.deb`.
- Deb SHA-256:
  `f881c78245ec88335c59b49b845dbeb3d24e1d047f8e0771e773281074bb93b2`.
- Candidate, packaged, and installed library SHA-256:
  `54e2f3563bd3ecdddd3bcd0d85c78f7bc82a6a1d6999949ce8dc776efe99ca84`.
- Production compiler flags remain
  `-O3 -march=loongarch64 -mtune=la664 -flto`.
- The authenticated 0.1.33 rollback was retained. Guarded latest-plus-one
  retention removed only 0.1.32 after verifying its exact path and SHA-256.
- Signed AOSC publication passed dry-run/deploy, local index validation,
  Release/InRelease verification, and isolated apt update. Final local, VPS,
  and HTTPS indexes contain 19 deb entries; each pool contains 19 deb files.
- Local, VPS, and HTTPS bytes match for both 0.1.34 and the 0.1.33 rollback.
- The generated root README lists only 0.1.34 as the latest vkgl package.

Evidence remains outside Git under
`/home/houge/infra/gpu/evidence/lg110-scissor-y-20261111/` and
`/home/houge/infra/gpu/evidence/vkgl-scissor-y-ab-20261111/`.

This release fixes the tested scissor-origin semantics. It is not native
Vulkan, generalized packet documentation, a numeric-conformance claim,
maximum-performance proof, or completion of the larger LoongArch/AOSC objective.
