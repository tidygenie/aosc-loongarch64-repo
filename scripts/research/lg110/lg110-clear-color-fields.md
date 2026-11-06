# LG110 clear RGBA field causality (2026-11-06)

This extends the earlier [single red-field proof](lg110-native-clear-mutation.md).
It establishes four independent single-dword/single-pixel-channel results on
vendor GSPKT streams. It is not a native Vulkan driver, a complete packet
specification, shader ISA research, or a performance claim.

## Method and guards

`lg110-clear-color-mutation.c` accepts exactly one
`LG110_MUTATE_CLEAR_FIELD=red|green|blue|alpha` per process. The old
`LG110_MUTATE_CLEAR_RED=1` spelling still selects red. Each selected word has
an exact required old value; every nonselected signature word, including
word91=`0x7f800000`, must match. The selected word is excluded from the fixed
signature and checked separately.

The write occurs only for a uniquely resolved, completely writable GFX ring0
IB, immediately before the original CS ioctl. No ioctl is added or replaced.
The store is read back and only `patched=1` with `observed==new` is accepted.
A process mutates at most one IB.

Two stream-specific exact lengths and signatures are used:

- 640x480 swapchain clear/readback: **1600 bytes / 400 dwords**, RGB fields.
- 64x64 headless optimal RGBA `ClearColorImage`: **1088 bytes / 272 dwords**,
  alpha only. The offscreen signature includes its dimensions and state words;
  selecting red on this stream must not patch anything.

The installed vkgl was 0.1.20-1, library SHA-256
`d9aad6dea8cb6b39e538722cb8b1fccb8f3bc9aa44380dc5944725d0eea0067d`.
All GPU runs were serial, used the same binaries and fresh controls, dumped
the complete IB, saved raw pixels before validation, and checked sentinel
overwrite and CS success.

## Results

All active runs below changed exactly one IB dword. “Pixel diff” is a direct
four-byte comparison against the corresponding fresh passive baseline; no
channel tolerance or normalization was used for this claim.

| Stream / field | Word and mutation | Raw result | Pixel diff |
|---|---|---|---|
| swapchain red | 87: `00000000→3f800000` | `663300ff→6633ffff` | byte2 `00→ff` |
| swapchain green | 88: `3e4ccccd→3f800000` | `663300ff→66ff00ff` | byte1 `33→ff` |
| swapchain blue | 89: `3ecccccd→3f800000` | `663300ff→ff3300ff` | byte0 `66→ff` |
| swapchain alpha | 90: `3f800000→3f000000` | `663300ff→663300ff` | none; inconclusive on this path |
| offscreen alpha | 90: `3f800000→3f000000` | `003366ff→00336680` | byte3 `ff→80` |

Thus words 87, 88, and 89 causally control clear red, green, and blue in the
swapchain stream. Word90 causally controls clear alpha in the independent
offscreen RGBA stream. The observed 0.5 encoding was 0x80 here; 0x7f was also
accepted by the test, so this does not claim a general UNORM midpoint rule.

## Why alpha required a second workload

A passive swapchain clear with alpha=0.5 also changed only word90, yet raw
readback remained `663300ff`, exactly like the active word90 experiment. This
shows that the swapchain/XR24 readback path cannot validate alpha causality;
the initial no-pixel-change result was not used to patch neighboring words.

The headless optimal R8G8B8A8 offscreen control preserves alpha:

- passive alpha=1.0: `003366ff`;
- passive alpha=0.5: `00336680`;
- the two complete 272-dword IBs differ only at word90.

The active offscreen experiment then started from the alpha=1.0 stream,
changed only word90 to 0.5, and produced exactly the passive alpha=0.5 pixel.

## Public rebuild and negative guard

`build-clear-color-mutation.sh` builds all three sources with strict warnings
and runs no GPU test. A fresh public-relative-path build repeated:

- swapchain green: only word88 changed, raw `663300ff→66ff00ff`;
- offscreen alpha: only word90 changed, raw `003366ff→00336680`;
- offscreen red request: **zero mutation records, zero IB differences**, raw
  stayed `003366ff`, and the test passed its alpha=1.0 control.

## Evidence and reproduction

Final evidence is named
`clear-color-swapchain-{base,red,green,blue,alpha,alpha-passive}.*` and
`clear-color-offscreen-{base,mutated,alpha-passive}.*`. Public-rebuild and
guard evidence uses the `clear-color-public-*` prefix. Each case includes a
four-byte `.rgba`, `.stdout.log`, and full `.trace.log`.

Build:

```sh
scripts/research/lg110/build-clear-color-mutation.sh /tmp/lg110-clear-color
```

Example active green run in the serial hardware Wayland session:

```sh
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vkgl_loonggpu.json
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export LD_PRELOAD=/tmp/lg110-clear-color/lg110-clear-color-mutation.so
export LG110_TRACE_IB_DWORDS=512 LG110_TRACE_MAX=16
LG110_MUTATE_CLEAR_FIELD=green \
LG110_EXPECT_CLEAR_FIELD=green \
LG110_READBACK_PIXEL=/tmp/green.rgba \
  /tmp/lg110-clear-color/vktest-clear-color-readback
```

Offscreen alpha starts with input alpha=1.0 and a separate expected value:

```sh
LG110_MUTATE_CLEAR_FIELD=alpha \
LG110_OFFSCREEN_ALPHA=1.0 LG110_EXPECT_OFFSCREEN_ALPHA=0.5 \
LG110_READBACK_PIXEL=/tmp/offscreen-alpha.rgba \
  /tmp/lg110-clear-color/vktest-offscreen-clear-alpha
```

Do not run multiple GPU experiments concurrently, bypass a signature/old-value
failure, or infer unmeasured fields from color adjacency. Draw, shader, tile,
address, and replay semantics remain open.
