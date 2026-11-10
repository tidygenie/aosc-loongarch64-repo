# vkgl-loonggpu 0.1.35-1 deferred UpdateBuffer correction

Date: 2026-11-11

## Correctness fix

0.1.34 implemented `vkCmdUpdateBuffer` by modifying the destination CPU shadow
immediately during command recording. Its `Cmd::UpdateBuffer` execution case
was empty. This violated both required timing rules: the source bytes were not
owned by the command, and the destination changed before queue submission.

0.1.35 copies the source bytes into `Cmd::update_payload` at record time. The
execute path validates the recorded destination range and applies the payload
in command order at submit time, incrementing the existing memory revision so
subsequent upload users can observe the change. Re-recording or resubmitting a
command buffer reuses the owned payload. The old raw `update_data` pointer is
removed. Bounds checks use subtraction rather than overflow-prone addition.

This is a Vulkan-over-GLES2 correctness fix, not native Vulkan or a claimed
performance optimization.

## Independent exact record/submit test

`vktest-update-buffer.c` creates a four-byte host-visible buffer on the real
LG110 vkgl device and records `vkCmdUpdateBuffer` with payload `0x11223344`.
It then changes the source to `0xaabbccdd`, checks that the destination still
contains initial value `0x10203040`, and overwrites the destination with
`0x5a5a5a5a` before submission.

After the first submit the destination must contain the recorded payload. The
test then writes `0xdeadbeef`, submits the same command buffer again, and
requires the same recorded payload after the second submit.

| Driver | Before submit | After first submit | After second submit | Result |
| --- | ---: | ---: | ---: | ---: |
| Installed 0.1.34 negative control | `11223344` | `5a5a5a5a` | `deadbeef` | fails as expected |
| Candidate 0.1.35 | `10203040` | `11223344` | `11223344` | exact pass |
| Public test rebuilt from Git sources | `10203040` | `11223344` | `11223344` | exact pass |
| Installed 0.1.35 package | `10203040` | `11223344` | `11223344` | exact pass |

Every run enumerated `LoongGPU LG110 (vkgl GLES2)`. Build the strict suite with
`build-0.1.35-tests.sh`; the test uses only the Vulkan loader and needs no
window or shader. The 0.1.34 negative control must exit nonzero and the fixed
driver must exit zero. The public test binary SHA-256 was
`e73f87eb1099b19e6e1955da18cdd336ae109622401a3506751200e10e8ef7c8`.

## Rendering and compatibility regressions

The candidate passed the 0.1.34 scissor test with exact bytes
`0000ffff000000ff` (top red, bottom black), all seven serial primitive-restart
cases used for the previous release, and the full Vulkan smoke suite. Smoke
covered LG110 enumeration, vkcube, five vkmark scenes, zero Khronos validation
errors, and no llvmpipe mapping. The installed package repeated the exact
scissor and UpdateBuffer tests and passed the same 5/5 smoke checks.

Six serial AB/BA vkmark rounds used 640x480 FIFO, four seconds per scene, and
Khronos validation. Hardware, software-renderer, validation-error, missing-FPS,
and failed-process guards all passed.

| Scene | 0.1.34 baseline median FPS | Candidate median FPS | Median paired change | Pair range |
| --- | ---: | ---: | ---: | ---: |
| vertex | 227.0 | 229.5 | +0.43% | -6.17% to +5.45% |
| texture | 156.5 | 154.0 | -2.21% | -11.95% to +2.67% |
| shading | 196.5 | 197.0 | +0.50% | -3.66% to +3.19% |

These are noisy default-path regression controls, not evidence of a speedup or
a stable regression. In particular, the texture series contains a large
outlier and is not hidden or removed. Raw summary SHA-256:
`84d6d3b9737dc4871acd2aa33b2682c136bdb8ee7417a6ab81fbd90df17801c2`.

A second exact consumer test records an indirect-buffer update from two
vertices to three outside the render pass, mutates the source, and restores two
vertices through the host mapping before submission. UpdateBuffer must execute
before DrawIndirect. With Khronos validation enabled, retained 0.1.34 produced
black/black and failed; both the 0.1.35 candidate and installed package produced
red/black `0000ffff000000ff`. A third test updates vertex binding 0 through an
8-byte buffer-binding offset, then restores offscreen vertices through its host
mapping before submission. Retained 0.1.34 produced the clear color `663300ff`
and failed; both 0.1.35 binaries produced the gradient center `007f40ff` with
Khronos validation enabled. A fourth test records a red vec4 into a fragment
uniform buffer, mutates the source, and restores blue through the host mapping
before submission. Retained 0.1.34 produced blue/black `ff0000ff000000ff` and
failed; both 0.1.35 binaries produced red/black `0000ffff000000ff`. These tests
cover the specified indirect, vertex, and uniform consumers; they are not a
claim of complete Vulkan conformance.

## Package, installation, and publication

- Package: `vkgl-loonggpu_0.1.35-1_loongarch64.deb`.
- Deb SHA-256:
  `25fd8f043a5b4db68dde660961840f4898aeb2a207dbb74d83e6b9dd42289cda`.
- Candidate, packaged, and installed library SHA-256:
  `78c963855189836046520f1ee9f327057bd61d7a75c2b8e40b8988f3ea473668`.
- Production source SHA-256:
  `31ab843f0001e40a3e897b8e8e574876c15a14a15e709d408d9d79876c2f9fff`.
- Compiler flags remain `-O3 -march=loongarch64 -mtune=la664 -flto`.
- Signed AOSC dry-run/deploy, Release/InRelease and isolated apt checks passed.
- Guarded latest-plus-one retention removed only verified 0.1.33 bytes;
  0.1.35 and the 0.1.34 rollback remain. Final local, VPS, and HTTPS indexes
  contain 19 entries and each pool contains 19 deb files.
- Local, VPS, and HTTPS bytes match for both current and rollback packages.
- The generated root README lists only 0.1.35 as latest.

Evidence remains outside Git under
`/home/houge/infra/gpu/evidence/lg110-update-buffer-20261111/` and
`/home/houge/infra/gpu/evidence/vkgl-update-buffer-ab-20261111/`.

This release fixes the tested UpdateBuffer timing semantics without changing
native command formats. It is not maximum-performance proof or completion of
the larger LoongArch/AOSC objective.
