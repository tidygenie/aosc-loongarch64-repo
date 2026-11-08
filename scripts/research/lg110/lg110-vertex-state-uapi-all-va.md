# LG110 vertex stride: raw UAPI, sensitive A/B/A and synchronized all-VA audit (2026-11-08)

Installed and published vkgl remains **0.1.31-1**. This is read-only research
except for the separately identified legacy draw-count mutation control. No
candidate driver, deb, kernel, GRUB or system graphics configuration was
changed. This is not native Vulkan and is not a performance or zero-copy
completion claim.

## Result in one paragraph

Gallium receives the correct position stride=8 and UV stride 8/16 sequence, and
an attribute-sensitive test proves that this difference changes real pixels.
Nevertheless, the two-distinct-pipeline/same-stride control and the 8/16/8
stride run emit exactly the same three GFX command streams. After an explicit
CS completion wait, every common hash record for every mapped VA in the three
draw submissions is also exactly equal, including complete 16 MiB BOs and VAs
not present in the CS bo_list. All device mappings match GEM_MMAP returns and
there are no direct DRM read/write syscalls. Therefore stride is **not stored
in any channel measured here**. Its remaining vendor CPU/context/firmware path
is unresolved; it would be incorrect to rename a GFX word as a stride field or
to claim that the hardware has no vertex stride implementation.

## 1. Raw LoongGPU UAPI inventory

`strace -e raw=ioctl` was decoded with the installed
`loonggpu_drm.h`, rather than trusting strace's unrelated AMD/other-driver
aliases. Counts for the constant-output same and stride runs were:

| Request group | same | stride |
|---|---:|---:|
| GEM_CREATE | 32 | 34 |
| GEM_MMAP | 21 | 23 |
| CTX | 2 | 2 |
| BO_LIST | 10 | 10 |
| CS | 5 | 5 |
| INFO | 10 | 10 |
| GEM_METADATA | 14 | 14 |
| GEM_VA | 41 | 43 |
| GEM_OP | 4 | 4 |

The only delta is CREATE/MMAP/VA +2 each from second-pipeline lazy
initialization. There is no additional request type and no separate write
ioctl. Fourteen METADATA calls and four GEM_OP calls per run normalize to the
same 22 records, SHA-256
`cf7217f9f44c7fb2cecbc23bb33d1c7a451687e0938ba6778146c66a79479d4a`.
METADATA has `data_size_bytes=0`. GEM_OP is `GET_GEM_CREATE_INFO`: its `value`
is a userspace output pointer, not a vertex-state value. The pointed create
records are also equal: size `0x1e0000`, alignment `0x4000`, domains=2,
flags=0.

## 2. Attribute-sensitive same-process controls

The old constant-green test can hide an attribute-state error. The new
`vktest-attribute-color.frag` directly emits `vec4(uv, 0, 1)`, and
`LG110_ATTRIBUTE_SENSITIVE=1` checks every retained frame:

| Pair mode | Meaning | Raw BGRA center pixels, frames 0/1/2 |
|---|---|---|
| same | one pipeline, UV stride=16 | `009f20ff, 009f20ff, 009f20ff` |
| stride | two pipelines, UV stride 8 -> 16 -> 8 | `008040ff, 009f20ff, 008040ff` |
| pipeline | two distinct pipelines, both UV stride=16 | `009f20ff, 009f20ff, 009f20ff` |

Thus stride has A != B and A == A semantics, while merely creating/alternating
two pipelines does not reproduce it. The final untraced sensitive runs all
pass their per-frame and pattern assertions.

Vendor `GALLIUM_TRACE` independently confirms the exact API inputs. Position
is always `[stride=8, buffer_offset=8]`; UV is `[stride=16, offset=0]` in all
three pipeline frames and `[8,16,8]` in the stride frames. This rejects the
hypothesis that vkgl failed to deliver the Vulkan binding stride.

## 3. GFX and shader comparison

The sensitive pipeline and stride captures have exactly equal GFX dword
streams at seq3, seq4 and seq5 (544, 528 and 528 dwords respectively). Their
seq4 -> seq5 difference lists are also exactly the same eleven words:

```
54 87 110 184 204 216 218 228 240 372 504
```

Only word204 and word228 are established here as shader-program VA selectors;
the other changing words remain normal, unnamed frame/state differences. The
A selectors use handles20/21 and the B selectors handles22/25 in these traces.
Correctly paired 16 KiB shader objects have matching complete-object FNV-1a
hashes across pipeline and stride controls:

- vertex FNV-1a: `0x24356227d25a99be`;
- fragment FNV-1a: `0x4292ec81d330ca20`.

An earlier mistaken handle25/handle23 cross-shader pairing was discarded and
is not evidence. The old word338/362 results likewise remain state-BO
selectors, not numeric stride fields.

## 4. Complete BO coverage, including non-list VAs

The tracer now supports opt-in bounded full-object hashing:

- `LG110_TRACE_BO_HASH=1` selects hashes instead of text dumps;
- `LG110_TRACE_BO_HASH_CHUNK=1024` means 1024 **dwords** (4 KiB), not bytes;
- `LG110_TRACE_BO_LARGE_FULL=1` covers a large BO in at most 2 MiB temporary
  read-only mappings, unmapping each chunk before the next;
- `LG110_TRACE_ALL_VA=1` additionally covers active tracked VAs omitted from
  the current CS bo_list, rather than assuming every useful BO is listed;
- `LG110_TRACE_WAIT_CS=1` waits on the returned CS handle with a bounded
  five-second absolute timeout before sampling.

A first unsynchronized all-VA run showed all 480 chunks of handle12 differing
only at seq4, then matching again at seq5. This was an asynchronous sampling
race, not stride evidence. With WAIT_CS, all ten waits (five per mode) returned
`ret=0, errno=0, status=0`. The exact common-record results became:

| Draw sequence | Records per mode | Common hash differences |
|---|---:|---:|
| seq3 | 13,516 | 0 |
| seq4 | 13,524 | 0 |
| seq5 | 13,524 | 0 |

These records include complete 16 MiB objects and non-bo_list VAs. The only
cross-mode lifetime mismatch was pipeline having an additional handle20
16 KiB initialization snapshot at seq2; it is not a draw-state delta. A
separate constant-output run restricted to CS resources also matched all
14,016 draw records across pipeline and stride.

This is a stronger exclusion than the previous <=2 MiB/bo_list-only reports,
but it is still limited to VAs known to this tracer in this process. It does
not inspect undocumented vendor CPU structures, kernel context memory or
firmware-internal registers.

## 5. Device mapping and file-I/O channels

Each sensitive mode made 20 device `mmap` calls. In the combined ioctl+mmap
trace, every device mmap offset matched a preceding GEM_MMAP output (19 unique
offsets, one mapping repeated). No unmatched/page-sized MMIO mapping was
observed. A separate path-filtered trace of
`read/readv/pread64/write/writev/pwrite64` on both DRM nodes produced zero
records while the sensitive A/B/A pixels still passed.

Together with the raw ioctl inventory, this excludes the observed direct
DRM file-I/O and non-GEM-mapping explanations. It does not justify guessing a
new hardware register or packet field.

## 6. Final regressions

The strict build uses `-Wall -Wextra -Werror`. Final serial results:

- default same: `007f00ff` x3, PASS;
- default offset: `007f40ff` x3, PASS;
- default stride: `007f00ff` x3, PASS;
- sensitive same/pipeline/stride: exact table above, all PASS;
- legacy passive count=3: `007f00ff`, PASS, word270=3;
- legacy opt-in active count=2: `663300ff`, word270=2 and sentinel unchanged.
  The old test intentionally exits 1 because its expected count remains 3;
  this expected mismatch is recorded, not hidden as a normal PASS.

`uapi-all-va-public-audit.log` contains the compact machine-checked results.
Raw evidence remains under `/tmp/lg110-ioctl-round32`,
`/tmp/lg110-sensitive-evidence`, `/tmp/lg110-large-full-evidence`,
`/tmp/lg110-all-va-wait` and `/tmp/lg110-final-regression` on the research
host. Large XML/full-hash logs are deliberately not committed as source.

## 7. Reproduce the synchronized all-VA control

Build and enter the generated run directory:

```sh
scripts/research/lg110/build-vertex-state.sh /tmp/lg110-allva
cd /tmp/lg110-allva/run
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export LD_PRELOAD=/tmp/lg110-allva/lg110-draw-count-mutation.so
export LG110_TRACE_MAX=8 LG110_TRACE_AUX_MAX=200000
export LG110_TRACE_IB_DWORDS=1 LG110_TRACE_BO_RESOURCES=1
export LG110_TRACE_BO_DWORDS=4096 LG110_TRACE_BO_MAX_BYTES=0x200000
export LG110_TRACE_BO_HASH=1 LG110_TRACE_BO_HASH_CHUNK=1024
export LG110_TRACE_BO_LARGE_FULL=1 LG110_TRACE_ALL_VA=1
export LG110_TRACE_WAIT_CS=1
unset LG110_MUTATE_DRAW_FIELD LG110_TRACE_VA LG110_ATTRIBUTE_SENSITIVE
```

Run serially with `LG110_FRAME_LIMIT=3 LG110_PAIR_MODE=pipeline
LG110_POSITION_OFFSET=8 LG110_UV_STRIDE=16`, then with
`LG110_PAIR_MODE=stride LG110_UV_STRIDE=8`. Keep the other established draw
arguments at their known non-indexed, non-indirect, count=3 controls. Compare
only after checking every WAIT_CS status and all three pixels. For the
sensitive controls, remove the tracer, set `LG110_ATTRIBUTE_SENSITIVE=1` and
`LG110_FRAGMENT_SHADER=vktest-attribute-color.frag.spv`.

The next work item is to identify how the vendor Gallium draw path derives
vertex fetch behavior outside the captured command/resources. Until that is
observed and causally tested, no native vertex-stride field, native Vulkan
implementation, or performance improvement is claimed.
