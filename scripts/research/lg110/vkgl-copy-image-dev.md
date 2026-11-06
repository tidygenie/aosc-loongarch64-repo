# vkgl direct GPU `Cmd::CopyImage` dev result

## Scope and implementation

Only `/home/houge/infra/vkgl/vkgl.cpp` and independent research tests/artifacts were changed. No system library was installed or replaced and no public repository was changed or pushed. The candidate remains selected through the research dev ICD JSON.

The original entry point was a varargs no-op stub and `Cmd::Kind` had no CopyImage command. The patch therefore adds both recording and execution:

- `Cmd::CopyImage`, source-image and `VkImageCopy` fields.
- A correctly typed `vkgl_CmdCopyImage` that records each region with src/dst image handles.
- Execution restricted to different 2D color images, mip0, one layer, depth1, identical B8G8R8A8_UNORM or R8G8B8A8_UNORM formats, transfer src/dst usage, nonnegative offsets, nonzero extent, and complete source/destination bounds.
- Different images make the supported copy non-overlapping. A destination must have bound, non-mapped memory; unsupported or invalid regions log `CopyImage rejected unsupported or out-of-bounds region` and return before any GL call, write, or generation change.
- `ensure_fbo(src)` and `ensure_fbo(dst)` provide the actual draw FBO and destination texture. The implementation calls `glCopyTexSubImage2D` directly from the bound source FBO into the destination texture; there is no `glReadPixels` or CPU staging in CopyImage.
- GL source and destination Y are independently `image_height - region_y - region_height`, preserving orientation across the direct bottom-up-to-bottom-up copy.
- Previous framebuffer and texture bindings are restored after the copy.
- The host shadow is unchanged. On success `dst->uploaded_revision = dst->mem->revision` without incrementing the shadow revision, preventing a later revision-mismatch upload from overwriting the GPU copy. A mapped dst is rejected because the existing linear sampling path would upload its shadow unconditionally.
- The dmabuf zero-copy present branch is unchanged.

Incremental patch: `vkgl-copy-image-dev.patch` (91 diff lines). Base: `vkgl-copy-image-base.cpp`. The dev build log `vkgl-copy-image-dev-build.log` is empty.

## Independent real src→dst→buffer test

`vktest-gradient-copy-image.c` uses the same original gradient draw and fixed-UV texture sample for A/B. It creates an independent 64×64 optimal-tiled offscreen destination in the swapchain's B8/R8 format with TRANSFER_SRC|TRANSFER_DST usage.

On frame 0, one submitted command sequence:

1. Draws the gradient+sample into the src swapchain image.
2. Transitions and GPU-clears the offscreen dst to `a5` using the existing clear path.
3. Copies src `(320,239)`, extent 2×2, to a different dst offset `(7,9)` with `vkCmdCopyImage`.
4. Reads **dst `(7,9)`**, not src, through the previously validated CopyImageToBuffer path with rowLength4, bufferOffset4, and a 32-byte buffer.

Frames 1–119 retain the identical draw without the one-time copy/readback. A texture is all zero; B changes only source texture red to 255. The fragment shader remains independent `vktest-gradient-copy-image.frag` / `.frag.spv`; strict C11 `-Wall -Wextra -Werror` and shader build logs are empty.

Both A/B completed 120 frames, validated four dst pixels, preserved every buffer guard/padding byte, printed PASS, and exited 0. Actual bytes exactly matched expected values; tolerance was not widened:

```text
A 00000000: a5 a5 a5 a5 00 80 00 ff 00 80 00 ff a5 a5 a5 a5
A 00000010: a5 a5 a5 a5 00 7f 00 ff 00 7f 00 ff a5 a5 a5 a5
B 00000000: a5 a5 a5 a5 00 80 40 ff 00 80 40 ff a5 a5 a5 a5
B 00000010: a5 a5 a5 a5 00 7f 40 ff 00 7f 40 ff a5 a5 a5 a5
```

Only offsets 6, 10, 22, 26 differ, each red byte 0→64. Raw buffers are `vktest-gradient-copy-image-dev-A.bin` and `...-B.bin`; `vktest-gradient-copy-image-dev-hexdump.log` records the exact comparison.

## IB comparison and tracer-line correction

### Initial 1024-dword capture

The first runs used `lg110-iotrace-1024.so`, cap 1024. Sequence 2's actual GFX length is **800 dwords**. Its header reported 800, but only **733 data words** were serialized because that tracer's fixed 8192-byte log line truncated the text; this was not missing GPU work. The first IB was therefore not claimed complete.

The complete steady-state sequences 3–121 were nevertheless compared directly:

- 119 pairs, each 400/400 dwords, 0 truncated.
- **47,600 equal / 0 different**.
- A/B steady SHA-256 both `0c7b618d024ad536245bc1b8c0e8cd74592a4981d579260bcceab08aa48fdace`.
- Log: `vktest-gradient-copy-image-dev-ib-diff.log`.

### Full-line capture

After all other dev/public GPU tests ended, CopyImage A/B were rerun serially with `lg110-iotrace-fullline.so` (1024 cap and enlarged `1024*11+1024` line buffer). Both again passed pixel/guard validation and exited 0. The supplied comparator was invoked with `first_dwords=800`.

Final result in `vktest-gradient-copy-image-fullline-ib-diff.log`:

- Complete GFX pairs: **120**.
- Sequence 2: **800/800**; sequences 3–121: 400/400.
- **48,400 equal / 0 different / 0 truncated**.
- A/B SHA-256 both `c1b0cb49f9d2cb2d024bb65204f7436466c4fddcd7bacc8c99d974f70be7a95e`.
- `PASS complete A/B GFX IB comparison`.

Raw full-line pixels are `vktest-gradient-copy-image-fullline-A/B.bin`; logs use the same prefix with `-full.log`, `.stdout`, and `.exit`. No unknown opcode semantics are inferred from these byte comparisons.

## Dev smoke

Using the CopyImage dev ICD, all requested existing smoke paths passed with exit 0:

- `vktest-clear`: `PASS vktest-clear`.
- `vktest-copy-texture`: `PASS vktest-copy-texture`.
- Original tight readback A: PASS, raw `00 7f 00 ff`.
- Padded readback A: PASS, exact 32-byte A result and all guards/padding preserved.

Logs/statuses are `vktest-clear-copy-image-dev-smoke.*`, `vktest-copy-texture-copy-image-dev-smoke.*`, `vktest-tight-copy-image-dev-smoke.*`, and `vktest-padded-copy-image-dev-smoke.*`; tight/padded raw smoke files are also retained.

## Public readback portability control on installed 0.1.17

After all dev GPU testing, the already-built `/tmp/public-vktest-readback` and `/tmp/public-vktest-readback-padded` were run from the public research script directory so their relative shader paths resolve naturally. Installed package was `vkgl-loonggpu 0.1.17-1`; `VK_ICD_FILENAMES`, `VK_DRIVER_FILES`, and `LD_PRELOAD` were explicitly unset. No public file was modified.

All four serial 120-frame runs passed and exited 0:

| Public run | Raw result | Matches prior evidence |
| --- | --- | --- |
| tight A | `00 7f 00 ff` | installed full1024 tight A |
| tight B | `00 7f 40 ff` | installed full1024 tight B |
| padded A | exact 32-byte A above | dev padded A |
| padded B | exact 32-byte B above | dev padded B |

Logs are `/tmp/public-tight-A/B.log` and `/tmp/public-padded-A/B.log`; raw files are corresponding `.rgba` for tight and `.bin` for padded, with `.exit` statuses.

## Limitations and artifact index

The result is bounded to the implemented single-mip/single-layer 2D UNORM copy and tested 2×2 B8G8R8A8 workload. Mapped destinations, other formats/aspects, depth, array/mip regions, same-image/overlap, and general synchronization remain unsupported or unclaimed. This is functional evidence, not performance or full Vulkan-conformance certification.

Under `/home/houge/infra/research/lg110`:

- Driver: `vkgl-copy-image-dev.patch`, `vkgl-copy-image-base.cpp`, `vkgl-copy-image-dev-build.log`, dev shared library/JSON.
- Test: `vktest-gradient-copy-image.c`, binary, `.frag`, `.frag.spv`, C/shader build logs.
- Initial A/B: `vktest-gradient-copy-image-dev-A/B` raw, stdout, exit, full traces; hexdump and steady IB diff logs.
- Full-line A/B: `vktest-gradient-copy-image-fullline-A/B` raw, stdout, exit, full traces; full 120-pair comparison log.
- Smoke files and installed public `/tmp` evidence named above.
- This report: `vkgl-copy-image-dev.md`.
