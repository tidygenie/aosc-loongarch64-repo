# LG110 sensitive all-VA follow-up: attribute the two output BOs

Date: 2026-11-08

## Scope

This follow-up uses the installed vkgl library, not a native LG110 Vulkan
implementation.  Its purpose is to determine whether the large BO differences
in the sensitive UV-stride A/B/A test are hidden vertex state or rendered
output.  No installed driver or kernel component was replaced.

Running kernel: `7.3.1-mainline-16k-rt-gcc-o3`.  The installed
`/usr/lib/vkgl/libvkgl.so` SHA-256 remains
`afac548f68ed43574b546b76e541110fba46e9c329f66a3318fcecc7dc4d2b3b`.
The final tracer SHA-256 is
`db622df1f072ce062a01a2bb2fe16b56ad6384f25f1fca455c46f60c87c4ae59`.

The test explicitly selects `vktest-attribute-color.frag.spv`.  Pipeline and
same use UV stride 16; stride uses the 8 -> 16 -> 8 pipeline sequence.  These
runs retain position offset zero.  Their absolute values therefore must not be
mixed with earlier no-tracer runs that used different controls.

## Persistent sensitive reproduction

All three programs returned success and wrote per-frame pixels under the
persistent evidence directory.  In this run:

| Mode | frame 0 | frame 1 | frame 2 |
| --- | --- | --- | --- |
| pipeline | `007010ff` | `007010ff` | `007010ff` |
| stride | `004040ff` | `007010ff` | `004040ff` |
| same | `007010ff` | `007010ff` | `007010ff` |

Each trace contains five CS submissions (three setup/draw sequences plus the
two initial upload submissions, numbered 1 through 5).  The original
`LG110IBDUMP` records cover only the first 32 dwords of each draw IB; those
prefixes are identical across pipeline, stride, and same.  A later complete-IB
follow-up also found identical 544/528-dword arrays and located the changing
vertex stride in a separate descriptor; see
[`lg110-vendor-vertex-descriptor.md`](lg110-vendor-vertex-descriptor.md).
There is no additional copy CS numbered 6 through 8 in this test.

All ten hash records per mode for the known input handles 15 and 16 are exactly
equal across all three modes.  Thus the sensitive result still has identical
captured input VBO bytes and identical originally captured IB prefixes.

## Direct BO attribution, not a size-only guess

Handles 11 and 12 each occupy `0x1e0000` bytes, equal to
`1024 * 480 * 4`: the 640x480 swapchain images with a 4096-byte row stride.
Size alone was not treated as proof.

The tracer now supports `LG110_TRACE_SAMPLE_HANDLE` and
`LG110_TRACE_SAMPLE_OFFSET`.  For a tracked VA covering a BO no larger than
`LG110_TRACE_BO_MAX_BYTES`, it maps the BO from base zero with its actual size,
then reads exactly one dword at the requested absolute BO offset.  The mapping
is `PROT_READ`; no mutation mode is enabled.  This is the same mapping shape
used by the successful full-VA hash path.  Nonzero-offset and incorrectly sized
short mappings were rejected with `EINVAL` and were never treated as pixels.

After the bounded `WAIT_CS`, sampling the center texel at

```
(320 + 240 * 1024) * 4 = 0xf0500
```

produced the following exact correspondence in the final stride run:

| CPU frame | CS | BO | dword | byte order |
| --- | --- | --- | --- | --- |
| 0 | 3 | 11 | `0xff404000` | `004040ff` |
| 1 | 4 | 12 | `0xff107000` | `007010ff` |
| 2 | 5 | 11 | `0xff404000` | `004040ff` |

Handle 11 remains A at seq4 while the CPU readback is B; handle 12 supplies B
at seq4.  This negative control rules out the mistaken interpretation that
handle 11 is the current output on every frame.

Consequently, the 120-chunk handle-11 hash differences between pipeline and
stride are explained by the two actual swapchain image contents.  They are not
evidence for a hidden vertex-stride register.  Handle 12 already has the B
content in both pipeline and stride at seq4, so it does not appear as a
cross-mode difference there.

The remaining small differences in handles 6, 8, 22, 25, 26, and 27 are not
named or assigned semantics here.  Likewise, the CPU readback mechanism is not
inferred from the absence of a separate copy CS.

## Validation

- Strict tracer/test build with `-Wall -Wextra -Wpedantic -Werror`: passed.
- Passive non-indexed draw=3 control: `007f00ff`, within the declared tolerance
  of `008000ff`, program returned success.
- Guarded active word270 count `3 -> 2`: `663300ff`, expected draw=2, program
  returned success.
- Sensitive pipeline, stride, and same controls: all returned success with
  valid patterns.
- Final direct swapchain assertion: `FINAL_SWAPCHAIN_DIRECT_MATCH=PASS`.

## Reproduction and evidence

Build outside `/tmp` so evidence survives reboots:

```sh
scripts/research/lg110/build-vertex-state.sh \
  /home/houge/infra/gpu/build/lg110-vertex-state
cd /home/houge/infra/gpu/build/lg110-vertex-state/run
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export LD_PRELOAD=../lg110-draw-count-mutation.so
export LG110_TRACE_MAX=8 LG110_TRACE_WAIT_CS=1
export LG110_TRACE_BO_MAX_BYTES=0x200000
export LG110_TRACE_SAMPLE_OFFSET=0xf0500
export LG110_ATTRIBUTE_SENSITIVE=1
export LG110_FRAGMENT_SHADER=vktest-attribute-color.frag.spv
export LG110_TEXTURE_VARIANT=A LG110_FRAME_LIMIT=3
export LG110_PAIR_MODE=stride LG110_UV_STRIDE=8
LG110_TRACE_SAMPLE_HANDLE=11 LG110_READBACK_PIXEL=/path/h11.rgba \
  ../vktest-vertex-pair
LG110_TRACE_SAMPLE_HANDLE=12 LG110_READBACK_PIXEL=/path/h12.rgba \
  ../vktest-vertex-pair
```

Raw logs, pixels, comparison output, and SHA manifests are retained locally at
`/home/houge/infra/gpu/evidence/lg110-sensitive-allva-20261108/`.  The final
assertion file SHA-256 is
`98fbbfce2b48da08bb92ec014d31463200a415d69d208c0302988da62aa41fd6`.
Large raw traces are deliberately not committed to the public repository.

This result narrows future work to the unresolved vendor state path; it does
not claim a native vertex-stride field, native Vulkan, zero-copy, or a measured
performance improvement.
