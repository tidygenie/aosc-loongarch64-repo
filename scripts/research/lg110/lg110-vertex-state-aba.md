# LG110 same-process vertex-state A/B/A differential (2026-11-07)

Installed vkgl remains 0.1.31. This research does not modify the driver or write
GPU BOs. It extends the previous constant-pixel test with same-process A/B/A
sequences so that ASLR, pipeline lazy initialization and per-frame commands can
be distinguished from vertex-state semantics.

## Why cross-process BO hashes were rejected

The scoped tracer first hashed actual CS resources. A large handle11 BO appeared
to change between different processes, but repeating the **same** offset=8,
stride=8 input in two processes also changed its full-BO hash:

- `0x0194faecaebde563`
- `0x8d75cd2e01831283`

A 4K-chunk run likewise changed all 480 chunks for identical input. Therefore
cross-process handle11 differences are noise/uninitialized or process-specific
content, not evidence for offset or stride fields. The `noise-public-*` traces
preserve this negative control.

## In-process A/B/A method

`vktest-vertex-pair.c` renders and reads back every frame with validation clean:

- `same`: offset=8, stride=16 for all three frames;
- `offset`: offset 0 -> 8 -> 0 with stride=8;
- `stride`: UV stride 8 -> 16 -> 8 with offset=8, using two pipelines.

Cyclic positions and constant `{0,0.5}` UV pairs make every center pixel
`007f00ff`. Three validation runs and three traced runs all pass every frame;
nine raw pixels are retained for each set.

The tracer logs BO-list membership and, only when
`LG110_TRACE_BO_RESOURCES=1`, reads resources belonging to the actual CS.
Raw mode is capped at 16K per BO. Hash mode accepts at most 2M per BO, excluding
the 16M framebuffer in this capture. No resource is written.

## Causal results after both pipelines are initialized

For seq4 -> seq5 (B -> A, after the second pipeline has already run):

| Mode | Changed common small BOs | GFX dword differences |
|---|---:|---:|
| same | 0 | 12 |
| offset | 0 | 12 |
| stride | 0 | 14 |

The two stride-only differences are:

- word338: `0x022e4000 -> 0x022dc000`;
- word362: `0x022e8000 -> 0x022e0000`.

These words select different state BOs when returning from the second pipeline
to the first. They are **not** numeric stride=16/8 fields. The selected BO
contents do not explain a stride difference:

- initialized handle23 seq3 == handle25 seq4: all 4096 dwords have FNV-1a
  `0xe785d7ee442e0ad2`;
- initialized handle24 seq3 == handle26 seq4: all 4096 dwords have FNV-1a
  `0x6d4e832bb0e9f7b8`;
- their first 64 dwords are also exactly equal in the raw ABA capture.

The seq3 -> seq4 stride run initially showed handle25/26 changing from zero to
nonzero. A/B/A identifies this as second-pipeline lazy initialization, not a
reversible stride update. Offset produced no extra small-BO change beyond the
same control. No new native vertex field is assigned.

See `aba-public-audit.log` for machine-checked invariants. The extended tracer
also passes the old passive count=3 and active count=2 known-signature controls;
active mutation remains opt-in and was not used in the ABA captures.

## Reproduce

Build and run from the generated `run` directory:

```sh
scripts/research/lg110/build-vertex-state.sh /tmp/lg110-aba
cd /tmp/lg110-aba/run
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export LD_PRELOAD=/tmp/lg110-aba/lg110-draw-count-mutation.so
export LG110_TRACE_MAX=8 LG110_TRACE_AUX_MAX=128 LG110_TRACE_IB_DWORDS=2048
export LG110_TRACE_BO_RESOURCES=1 LG110_TRACE_BO_DWORDS=64
export LG110_TRACE_BO_MAX_BYTES=0x4000
unset LG110_MUTATE_DRAW_FIELD LG110_TRACE_BO_HASH
LG110_TEXTURE_VARIANT=A LG110_DRAW_VERTEX_COUNT=3 LG110_DRAW_INSTANCE_COUNT=1 \
LG110_DRAW_FIRST_INSTANCE=0 LG110_DRAW_INDEXED=0 LG110_DRAW_INDIRECT=0 \
LG110_INDEX_TYPE=16 LG110_FIRST_INDEX=0 LG110_VERTEX_OFFSET=0 \
LG110_EXPECT_DRAW_COUNT=3 LG110_FRAME_LIMIT=3 \
LG110_PAIR_MODE=stride LG110_POSITION_OFFSET=8 LG110_UV_STRIDE=8 \
LG110_READBACK_PIXEL=/tmp/aba.rgba /tmp/lg110-aba/vktest-vertex-pair
```

Repeat with `PAIR_MODE=same`, offset=8, stride=16, and with `PAIR_MODE=offset`,
offset=0, stride=8. Run serially. FNV-1a is used only as a differential digest,
not as a security hash or a substitute for the raw-word checks above.
