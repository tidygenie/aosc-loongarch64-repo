# vkgl-loonggpu 0.1.24-1 compiler evidence (2026-11-06)

## Scope: same source, compiler flags only

The Vulkan/GL source is unchanged from 0.1.23. The build now uses GCC 16.2.0
with these flags for both the generated Wayland C protocol and the C++ ICD:

```text
-O3 -march=loongarch64 -mtune=la664 -flto
```

`-march=loongarch64` keeps the baseline ISA. LA664 is used only for scheduling
and tuning; this is not a `-march=native` or LASX-only package. The package
recipe produced the exact same library bytes as the measured LTO candidate.

Three binaries were compared:

| Variant | Library SHA-256 |
|---|---|
| Installed 0.1.23 O2 | `1a2ee24ee1add7a39c78b1d52ea8615bd76d1c49b336f58eadc9e4d11e11e0be` |
| O3 + LA664 | `2e601b41296307cc6331b4a2210e101f2b5bd9fbba2ca33aefac76a777e7c7b1` |
| O3 + LA664 + LTO | `f3c17974dc01ac5438a94273f8f5523563b8172f9c69efc3e2eaab67fb061e3a` |

## Paired measurements

Each comparison used five rounds, three seconds per 640x480 FIFO vkmark scene,
and alternated base/candidate order. Homebrew builds remained active. Load was
approximately 7.8–8.2 during both comparisons; these are not idle-system or
large-application results.

### O3 without LTO — not selected

| Scene | O2 median FPS | O3 median FPS | Change | Paired wins |
|---|---:|---:|---:|---:|
| vertex | 195 | 183 | -6.15% | 1/5 |
| texture | 121 | 124 | +2.48% | 3/5 |
| shading | 144 | 159 | +10.42% | 5/5 |

O3 alone was rejected because the shading gain came with a vertex regression.

### O3 with LTO — selected

| Scene | O2 median FPS | O3+LTO median FPS | Change | Paired wins |
|---|---:|---:|---:|---:|
| vertex | 184 | 197 | +7.07% | 5/5 |
| texture | 120 | 127 | +5.83% | 4/5 |
| shading | 150 | 153 | +2.00% | 4/5 |

This supports a limited, measured improvement in these three small scenes under
the recorded load. It does not prove maximum hardware performance or universal
application gains. Raw JSONL and environment logs are published alongside this
report; no failed or unfavorable O3 result was removed.

## Correctness and presentation

Both compiler variants passed host format/sampler/fence tests, exact split-
texture LINEAR pixels (`007f22ff`), and dmabuf/wl_shm hardware smoke. The
selected LTO candidate additionally passed:

- uniform A/B gradient and padded readback;
- CopyImage and ClearAttachments A/B pixels;
- all six tight/padded upload cases;
- real EGL submission fence and both acquire paths.

Candidate and installed 0.1.24 smoke remain at **28 events / four VUID classes**,
unchanged from 0.1.23. Compiler optimization is not being used to hide validation
issues. No llvmpipe/swrast substitution was used.

Evidence: `pixel-regressions-0.1.24-candidate.log`,
`sampler-pixel-linear-0.1.24-{candidate,installed}.log`, and
`smoke-{o3,lto-candidate,installed}-0.1.24-{dmabuf,shm}.log`.

## Artifacts and publication

- `vkgl-loonggpu 0.1.24-1 loongarch64`
- Candidate/package/installed library SHA-256:
  `f3c17974dc01ac5438a94273f8f5523563b8172f9c69efc3e2eaab67fb061e3a`
- Deb SHA-256:
  `7b39464c262fdb3c46bd5bb74202de15706e2fb154321fba67476952c307a00a`
- Signed 23-package AOSC index, MD5, InRelease, HTTPS and isolated apt checks
  passed. Server/HTTPS bytes match and 0.1.24-1 is the highest indexed version.

Logs: `compiler-target-0.1.24.log`, `compiler-build-hashes-0.1.24.log`,
`package-0.1.24.log`, `install-0.1.24.log`, and `publish-0.1.24.log`.
