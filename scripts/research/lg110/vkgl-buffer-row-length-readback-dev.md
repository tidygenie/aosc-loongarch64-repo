# vkgl padded `bufferRowLength` readback dev result

## Scope

This extends the current CopyImageToBuffer implementation with padded destination rows. Only `/home/houge/infra/vkgl/vkgl.cpp` and independent research tests/artifacts were changed. The candidate remains `/home/houge/infra/research/lg110/libvkgl-readback-dev.so`, selected by the existing research dev JSON. No system library was installed or replaced and no public repository was modified or pushed.

The incremental patch against the saved tightly-packed implementation is `vkgl-buffer-row-length-dev.patch` (one functional hunk set, 58 diff lines). `vkgl-readback-unpadded.cpp` preserves its immediate base. The padded dev build log is empty.

## Implementation and overflow/bounds rules

For the existing supported 2D color, mip0, single-layer RGBA8/BGRA8 UNORM path:

- `row_texels = bufferRowLength ? bufferRowLength : imageExtent.width`.
- `bufferRowLength=0` therefore retains the old tightly packed behavior. A nonzero value must make `row_texels >= width`.
- `row_bytes = width*4`; destination `stride = row_texels*4`.
- `bufferImageHeight` remains 0 or at least `height`; depth remains 1.
- Overflow checks cover `width*4`, `row_texels*4`, temporary `height*row_bytes`, and `(height-1)*stride+row_bytes` before allocation or address calculation.
- Destination requirement is exactly `(height-1)*stride+row_bytes`, not `height*stride`; trailing padding after the last row is not required.
- Existing short-circuit checks protect `bufferOffset`, buffer size, `mem_offset`, and shadow-size subtraction before any write.
- `glReadPixels` still fills a tightly packed temporary buffer from the actual draw FBO. Row `y` is written at `dst + y*stride`; GL bottom-up source rows are reversed into Vulkan top-down order.

No support is claimed for other formats, depth, multiple mips/layers, or arbitrary padding conventions.

## Independent valid padded A/B test

Test source/binary: `vktest-gradient-sample-readback-padded.c` / `vktest-gradient-sample-readback-padded`; independent fragment/SPIR-V use the same original draw and one fixed-UV sample with weight 0.25. C builds with strict C11 `-Wall -Wextra -Werror`; its build log is empty.

`VkBufferImageCopy` uses:

- image offset `(320,239)`, extent 2×2×1;
- `bufferRowLength=4`, `bufferImageHeight=0`;
- **valid 4-byte-aligned `bufferOffset=4`**, buffer size 32;
- destination stride 16 bytes, row_bytes 8 bytes, required size 24 bytes.

The entire 32-byte buffer starts as `a5`. Data occupies offsets 4–11 and 20–27. Front guard 0–3, eight-byte inter-row padding 12–19, and tail guard 28–31 must remain exactly `a5`. Pixel tolerance remains ±2; guards/padding allow no difference. The two rows ideally have green 128 and 127, respectively.

A is all-zero texture; B changes only texture red to 255. Both dev runs completed 120 frames, validated four pixels, printed PASS, and exited 0. Actual bytes exactly equal the expected bytes, so no tolerance was needed or widened:

```text
A 00000000: a5 a5 a5 a5 00 80 00 ff 00 80 00 ff a5 a5 a5 a5
A 00000010: a5 a5 a5 a5 00 7f 00 ff 00 7f 00 ff a5 a5 a5 a5
B 00000000: a5 a5 a5 a5 00 80 40 ff 00 80 40 ff a5 a5 a5 a5
B 00000010: a5 a5 a5 a5 00 7f 40 ff 00 7f 40 ff a5 a5 a5 a5
```

A/B differ only at offsets **6, 10, 22, 26**, each red byte changing 0→64. Every guard and padding byte remains `a5`. Raw buffers are `vktest-gradient-sample-readback-padded-dev-A.bin` and `...-B.bin`; `vktest-gradient-sample-readback-padded-dev-hexdump.log` contains the verified dump and difference list.

## Padded dev steady-state IB comparison

Each padded dev trace contains 121 successful CS calls. Sequence 2, which contains readback, is 672 dwords and captured 512 under the fixed 512-dword tracer; it is excluded from the requested steady-state comparison.

For sequences 3–121:

- 119 frame-matched pairs, each 400/400 dwords; complete pairs 119, truncated pairs 0.
- Equal dwords **47,600**, different dwords **0**.
- A/B SHA-256 both `04ed5b028b951ea6da35e8945c8963005c712f59e4116bc3e1412f210803dc1d`.
- Evidence: `vktest-gradient-sample-readback-padded-dev-ib-diff.log`.

The first readback IB truncation is not hidden or included in the claim of complete steady-state IBs.

## Additional installed-0.1.16 full tight-IB control

After all dev GPU tests finished, the original tight `vktest-gradient-sample-readback` binary was run serially with installed `vkgl-loonggpu 0.1.16-1`, explicitly unsetting `VK_ICD_FILENAMES`/`VK_DRIVER_FILES`, using `lg110-iotrace-1024.so` with `LG110_TRACE_MAX=512` and `LG110_TRACE_IB_DWORDS=1024`.

Both A/B passed center-pixel validation and exited 0; actual bytes remain A=`00 7f 00 ff`, B=`00 7f 40 ff`. The supplied `compare-readback-full-1024.py` then verified all 120 GFX pairs, including the previously truncated first readback frame:

- Sequence 2: 672/672 dwords; sequences 3–121: 400/400 dwords.
- Complete pairs 120, truncated 0.
- Equal dwords **48,272**, different dwords **0**.
- A/B SHA-256 both `7430660bf066aa62c359ea8ed343d54a6ea0234e2e3d60da079d10f76ffff535`.
- Comparator result: `PASS complete A/B GFX IB comparison`.

Evidence files use prefix `vktest-gradient-sample-readback-full1024-A/B` and the comparison log is `vktest-gradient-sample-readback-full1024-ib-diff.log`.

## Smoke and limitations

Using the padded dev ICD, existing `vktest-clear` and the unchanged existing `vktest-copy-texture` research binary both printed PASS and exited 0. These are functional smoke tests only.

The padded result is bounded to the tested 2×2 B8G8R8A8 readback and existing format path. No performance or general Vulkan-compliance claim is made.

## Artifact index

Under `/home/houge/infra/research/lg110`:

- Incremental driver patch/build: `vkgl-buffer-row-length-dev.patch`, `vkgl-readback-unpadded.cpp`, `vkgl-readback-padded-dev-build.log`, `libvkgl-readback-dev.so`.
- Test C/shader/build: `vktest-gradient-sample-readback-padded.c`, binary, `.frag`, `.frag.spv`, `vktest-gradient-sample-readback-padded-build.log`, `vktest-gradient-sample-readback-padded-shader-build.log`.
- Padded A/B: per-run `.bin`, `.stdout.log`, `.exit`, and `-full.log` with prefix `vktest-gradient-sample-readback-padded-dev-A/B`; combined hexdump and IB diff logs named above.
- Smoke: `vktest-clear-padded-dev-smoke.log/.exit`, `vktest-copy-texture-padded-dev-smoke.log/.exit`.
- Installed full tight control: 1024 tracer/comparator and the full1024 A/B logs, exit statuses, raw pixels, and IB comparison log named above.
- This report: `vkgl-buffer-row-length-readback-dev.md`.
