# LG110 vkgl GCC 16 PGO candidate (rejected, 2026-11-10)

## Scope and build

This tests a compiler optimization on the clean vkgl-loonggpu 0.1.31 source.
It does not include the rejected EGL present-fence change and does not modify
the installed driver. The source SHA-256 is
`9f74d1408db4fd92248d8c88504c703ecf2e661fcd80fcff6b5db62b2d97f02b`.
The common flags remain `-O3 -march=loongarch64 -mtune=la664 -flto` on GCC
16.2.0; LA664 is scheduling/tuning only and does not raise the ISA baseline.

`vkgl-pgo-build.sh` first builds with
`-fprofile-generate=/home/houge/infra/gpu/candidates/vkgl-pgo-gcc16/profiles`.
The instrumented ICD ran vertex, texture and shading for ten seconds each on
`LoongGPU LG110 (vkgl GLES2)`, with validation enabled and no validation error
or software renderer. Normal process exit produced one real
`libvkgl-vkgl.gcda` (SHA-256
`7e86e9eb8f852c362a0a11c5ceb4bb8972e9cfc42c4a5d209790526edc8ec895`).
The final build used `-fprofile-use=<same directory> -fprofile-correction`.
It had no missing/mismatched-profile diagnostic and no residual gcov
instrumentation. Candidate SHA-256:
`89c3769711626019c10863db86fe810774dcdf550037067a6cc044e83eb1ad94`.

The candidate uses an explicit compile-then-link pipeline so profile artifacts
are retained. That differs from the installed direct compile-and-link build;
this experiment therefore does not causally attribute every layout difference
to PGO alone. The candidate is rejected on its measured outcome, so no further
promotion or installation is justified.

## Paired measurement

`vkgl-pgo-benchmark.py` ran six AB/BA rounds. Every run used 640x480, FIFO,
the validation layer and vertex/texture/shading for four seconds each. It
required the LG110 device string and rejected validation errors,
llvmpipe/swrast or a nonzero exit. All twelve runs passed those gates.

| Scene | Installed median FPS | PGO median FPS | Paired median | Positive pairs |
|---|---:|---:|---:|---:|
| vertex | 186.0 | 180.0 | -2.67% | 2/6 |
| texture | 129.5 | 127.5 | 0.00% | 2/6 |
| shading | 164.0 | 164.0 | -2.68% | 1/6 |

The complete values, pair order and timing data are in
`vkgl-pgo-summary.json` (SHA-256
`20c16cee6acba344b67235fa8189486b876378a371363ecb5b13842e18b1cf45`).
There is substantial run-to-run variance, especially in early rounds; no
warm-up samples were discarded to manufacture a gain. A smaller text section
is not treated as an FPS improvement.

## Decision

Reject and do not install this PGO candidate. `/usr/lib/vkgl/libvkgl.so`
remains SHA-256
`afac548f68ed43574b546b76e541110fba46e9c329f66a3318fcecc7dc4d2b3b`.
No maximum-performance, general-application or native-Vulkan claim is made.
The candidate was not promoted, so the full pixel/pour/release suite and a new
package version were not run.

Provenance hashes: build script
`61251c6da1acbe7b8bfe079a170c1e13c36c84c388a0354f0e7c611bf2195948`,
benchmark `280b023d6214684d25f260a1a895d4620fada94ae3b9985fb5ed9e6f7ae2d239`,
training log `f58220e7f2a5de4c96c38b93e9903db72908621a17a1325fda654d2542d496cd`.
Large binaries, gcda and per-round logs remain outside Git.
