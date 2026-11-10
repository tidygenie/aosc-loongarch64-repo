# LG110 fragment-uniform state: BO differential and causal mutation

Date: 2026-11-11

## Scope and binary identity

This experiment locates one fragment-uniform value in the installed vendor
OpenGL command path. It is not a native Vulkan implementation, a portable UAPI,
or a performance optimization. The observed GEM handle and byte offset are
specific to this workload and binary layout and must be re-derived after a
change.

- vkgl: 0.1.35, library SHA-256
  `78c963855189836046520f1ee9f327057bd61d7a75c2b8e40b8988f3ea473668`.
- Vendor `gsgpu_dri.so` SHA-256:
  `aff7101ab877db0026fa0f8274e03a579f1b4fd2c16fabe2478d08f224d0da0a`.
- Workload binary SHA-256:
  `8fe10d351674a8639b540df0fce9511c8836f40f4ca10609fe30882acafbf120`.
- Full-IB tracer SHA-256:
  `9d2edb1deda5a278869a0ea0a989efa81ecc87f9d730fe9c004ee171c0c6cfe2`.
- BO-hash/snapshot tracer SHA-256:
  `f3807c03e884cab76db298de5e4001302286743584748bc1eee8e155519d3f89`.
- Isolated mutation tracer SHA-256:
  `3d8fa55fa21351d3f4f09c7bf6bfecd722a1c8631ef117cb55ed242347a4a7c2`.

## Workload and passive controls

[`vktest-uniform-stream.c`](vktest-uniform-stream.c) binds a 16-byte fragment
uniform buffer. It records either red `(1,0,0,1)` or blue `(0,0,1,1)`, mutates
the source payload, and restores the opposite color through the persistent host
mapping before submission. Consequently, both successful modes require the
vkgl 0.1.35 submit-time UpdateBuffer behavior; neither can be satisfied by the
final host contents.

The exact BGRA readbacks on the same installed driver were:

| Recorded color | Host value before submit | Required top/bottom pixels | Result |
|---|---|---|---|
| red | blue | `0000ffff000000ff` | PASS |
| blue | red | `ff0000ff000000ff` | PASS |

[`build-uniform-stream.sh`](build-uniform-stream.sh) builds the workload, text
shaders, and tracers without executing a GPU test. The workload uses the shared
`update-uniform.frag`; SPIR-V is generated locally and is not committed.

## The complete IBs do not contain the color difference

The passive full-line tracer was run with at most four submissions and 1024
IB dwords. The initial attempt requested 2048 and correctly retained the
32-dword default because it exceeded the tracer's hard limit; those truncated
logs were not used for the conclusion. At 1024, every submitted IB was captured
in full.

| Submission | Contents | Dwords | Red/blue differences |
|---|---|---:|---:|
| 1 | XDMA | 16 | 0 |
| 2 | GFX draw | 544 | 0 |
| 3 | GFX readback | 272 | 0 |

Thus the color is not encoded in these 832 captured IB dwords. This does not
mean that uniforms never appear in an IB for another workload.

Text report: [`uniform-stream-ib-differential.txt`](uniform-stream-ib-differential.txt),
SHA-256 `4640a6d9552cfc8e51fb7c7f55cbd085ad7646d8a56a07afb8dcacaebbeab08b`.

## Bounded BO fingerprints isolate handle 8

`lg110-iotrace-bohash.c` is a separate passive tracer. Its extra features are
disabled by default. With `LG110_TRACE_BO_HASH=1`, it hashes only live tracked
GEM user mappings after the original submission returns. Each mapping is capped
at 16 MiB, each submission has a 64 MiB total budget, and any skip is explicitly
logged. It does not change the mapping.

The first, 1 MiB-capped pass found only fence-handle timing noise; that was not
attributed to color. The final bounded pass had zero skips. Handle 8, a 16 MiB
mapping, differed between red and blue at all three submissions. Same-sized
handles 4 and 18 and all other tracked state were identical in that comparison.
Handle 5 is the fence BO and was not treated as uniform storage.

Text report:
[`uniform-stream-bohash-large-differential.txt`](uniform-stream-bohash-large-differential.txt),
SHA-256 `cc8ace4b8619ecfc68b14d7d5346d807ae26e03114df60ab77ac143325a1422c`.

For local analysis only, `LG110_TRACE_BO_DUMP_HANDLE=8` and an explicit dump
directory saved the three 16 MiB snapshots per color. These 96 MiB of raw BO
files remain under the local evidence directory and are not committed.

## Exact color vector and changed bytes

Each corresponding red/blue handle-8 snapshot had exactly four differing bytes:

- `0x3cb2..0x3cb4` (two bytes);
- `0x3cba..0x3cbc` (two bytes).

The complete little-endian vec4 at offset `0x3cb0` was unique and exact in all
three submissions:

| Offset | Red | Blue |
|---:|---:|---:|
| `0x3cb0` | `0x3f800000` (1.0) | `0x00000000` (0.0) |
| `0x3cb4` | `0x00000000` | `0x00000000` |
| `0x3cb8` | `0x00000000` | `0x3f800000` (1.0) |
| `0x3cbc` | `0x3f800000` | `0x3f800000` |

Text report:
[`uniform-stream-handle8-byte-differential.txt`](uniform-stream-handle8-byte-differential.txt),
SHA-256 `a3598350b3cf35d141838c1c6ec13bcd38ca164aabfe5f4f7398f1d82b25393e`.

## Guarded causal mutation

`lg110-uniform-mutation.c` is intentionally a different, active experiment;
its file header is not labeled read-only. It is inert unless
`LG110_MUTATE_UNIFORM=1` is explicitly supplied. Immediately before submission
2 it requires all of the following:

- the tracked mapping is handle 8;
- its length covers `0x3cb0 + 16` without address overflow;
- the existing four dwords exactly equal red `(1,0,0,1)`.

Only then does it replace those 16 bytes with green `(0,1,0,1)` and issue a
memory fence before forwarding the unchanged ioctl. A missing mapping or a
different old value rejects the mutation. No production driver, IB, register,
or ioctl payload is modified.

Results:

| Run | Mutation record | Raw top/bottom BGRA | Test result |
|---|---|---|---|
| passive control | none | `0000ffff000000ff` | PASS |
| active green | exactly one `status=mutated` | `00ff00ff000000ff` | expected failure against red expectation, exit 1 |

The active run had no `guard_rejected` or `not_found`. This establishes causal
control of the fragment output color by this 16-byte BO field for the tested
stream. It does not establish a general uniform-buffer descriptor format, a
stable handle/offset, correctness for other shader stages, or a faster path.

## Reproduction boundaries

Use an active hardware-rendered Wayland session and run GPU workloads serially.
The passive capture uses `LD_PRELOAD`, `LG110_TRACE_MAX=4`,
`LG110_TRACE_IB_DWORDS=1024`, `LG110_TRACE_AUX_MAX=0`, and
`LG110_TRACE_BO_HASH=1` for the BO-hash tracer. The workload argument is `red`
or `blue`, and the existing `LG110_READBACK_PIXEL` output is mandatory.

The active mutation must remain a separate one-frame research run with the
same bounded trace settings and `LG110_MUTATE_UNIFORM=1`. A green raw pixel,
not a zero test exit, is its expected result. Do not add this preload to the
desktop, system driver configuration, or production environment.

Complete local logs and raw snapshots are in
`/home/houge/infra/gpu/evidence/lg110-uniform-stream-20261111/`. Public raw
text traces use the `uniform-stream-` prefix beside this report; compiled
binaries, SPIR-V, and raw BO snapshots are excluded from Git.
