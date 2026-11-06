# LG110 native clear-red causal mutation (2026-11-06)

This is a bounded reverse-engineering experiment, **not a native Vulkan driver**
and not a performance claim. It establishes one causal field in the exact
native GSPKT clear stream produced by the installed vendor stack.

## Method

`lg110-native-mutation.c` is separate from the passive tracer. Mutation is off
unless `LG110_MUTATE_CLEAR_RED=1`. Before forwarding the original CS ioctl it
requires all of the following:

- one readable GFX ring-0 IB with `ip_instance=0`, `flags=0`, and at least 144
  dwords;
- a tracked writable userspace mapping covering every `ib_bytes`, with aligned
  access;
- an exact 15-word clear signature, including words 0/2 and the surrounding
  clear-color constants;
- original word 87 equal to `0x00000000`.

Only word 87 is changed to `0x3f800000`; the write happens at most once per
process. No ioctl, BO, VA, packet, chunk, fence, or CS field is added or
replaced. The word is deliberately not restored, so the supplied workload is
one frame/one process.

`vktest-clear-readback.c` has no shader, pipeline, texture, or draw. It performs
one 640x480 clear and a one-pixel `CopyImageToBuffer`, uses bounded acquire and
fence waits, saves the raw four bytes, and checks the sentinels. The initial
compile-only scaffolding was rejected before CS because it omitted
`VK_KHR_swapchain`; enabling that device extension fixed the test before any
active experiment.

## Direct result

The passive baseline and active run each had one 1600-byte/400-dword GFX IB.
The baseline signature matched all 15 words and word 87 was zero.

| Run | Raw BGRA bytes | Result |
|---|---|---|
| passive baseline | `66 33 00 ff` | PASS; sentinel overwritten |
| word 87 = 1.0f | `66 33 ff ff` | PASS; sentinel overwritten |

The complete captured IB arrays differed at **exactly one of 400 dwords**:
word 87 changed `0x00000000 -> 0x3f800000`. The raw pixels differed at
**exactly one byte**: red `00 -> ff`; green, blue, and alpha were identical.
Both CS ioctls returned zero. No new GPU/CS timeout, fault, or reset was found
in the kernel log after the experiment.

Baseline raw SHA-256:
`b7ffb7acfae49cf37e85a9387f078f3b2d7b573e6f0eb159c70c11599312c584`.
Mutated raw SHA-256:
`0b8276e3362f2a96dc0b913ff254e630f75042c180f7575d9c71cc4aa3fd1ae0`.

This establishes causal control of the clear-red field in this tested stream.
The public relative-path sources were then rebuilt with strict warnings and
rerun as a fresh passive/active pair; that pair also produced `663300ff` and
`6633ffff`, with word 87 again being the only one of 400 differing dwords.
Its raw outputs use the `clear-readback-public-{base,mutated}.*` names.
It does not establish the semantics of opcodes 0x88/0x89, other payload words,
address/tile state, shader ISA, or a self-contained replay.

## Reproduce

Build from the public repository without running a GPU workload:

```sh
scripts/research/lg110/build-native-mutation.sh /tmp/lg110-native-public
```

Run serially in the active hardware Wayland session. First passively capture
and save the baseline (unset `LG110_MUTATE_CLEAR_RED`), then use a separate
process with `LG110_MUTATE_CLEAR_RED=1` and `LG110_EXPECT_CLEAR_RED=1`.
Set `LG110_READBACK_PIXEL` to distinct output paths and
`LG110_TRACE_IB_DWORDS=512`. Do not relax the signature to force a match.
`patched=1` alone proves only a userspace write; raw pixel A/B is mandatory.

Raw stdout, full traces, and four-byte results are published alongside this
report as `clear-readback-{base,mutated}.*`.
