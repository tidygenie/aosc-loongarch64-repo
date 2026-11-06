# vkgl-loonggpu 0.1.21-1 release evidence (2026-11-06)

## Fix

0.1.20 recorded `Pipeline::topology` but executed both indexed and non-indexed
draws with hardcoded `GL_TRIANGLES`. A passive point/triangle experiment produced
identical native IBs and the same gradient pixel, exposing the compatibility bug.

0.1.21 adds an explicit Vulkan-to-GL topology mapping for:

- POINT_LIST → GL_POINTS
- LINE_LIST → GL_LINES
- LINE_STRIP → GL_LINE_STRIP
- TRIANGLE_LIST → GL_TRIANGLES
- TRIANGLE_STRIP → GL_TRIANGLE_STRIP
- TRIANGLE_FAN → GL_TRIANGLE_FAN

The selected mode is used by both `glDrawArrays` and `glDrawElements`.
Adjacency and PATCH pipeline creation returns `VK_ERROR_FEATURE_NOT_PRESENT`
instead of silently rendering triangles. Existing triangle behavior, upload,
submission-fence, acquire, and presentation paths are unchanged.

The public acquire test now uses designated Wayland listener initializers so
new `configure_bounds` protocol fields do not break `-Werror` builds.

## Artifacts

- Package/version/architecture: `vkgl-loonggpu 0.1.21-1 loongarch64`
- Packaged, candidate, and installed library SHA-256:
  `f7452278fcaa6d7bfde600527c523181d87cdd599ee0a7487e135b8f612bb063`
- Deb SHA-256:
  `ec99119b9bb4aaa4ce44dfcc357d3fe14fda0de32049c1ef1c624bf2ad979bed`
- Repository: `pool/main/v/vkgl-loonggpu/`
- The signed 20-package index lists 0.1.21-1 as the highest vkgl version.
  Publisher MD5/InRelease/HTTPS/isolated-apt checks passed; server and HTTPS deb
  bytes match the local SHA-256 above.

Logs: `package-0.1.21.log`, `publish-0.1.21.log`.

## Functional and native verification

- All six basic topologies passed actual center-pixel checks. Full native
  differentials isolated word268; active triangle→point changed exactly that
  dword and reproduced passive point pixels. Count mutation regression and
  already-point rejection passed. Adjacency/PATCH both returned -8 with no
  draw/readback. See `../research/lg110/lg110-draw-topology-field.md` and raw
  `draw-topology-*` evidence.
- Candidate A/B gradient, padded readback, CopyImage, ClearAttachments,
  clear-only, real submission fence, six tight/padded upload cases, and dmabuf
  plus wl_shm acquire all passed with expected pixels/guards/sentinels.
  See `pixel-regressions-0.1.21-candidate.log`.
- Installed 0.1.21 tight upload, real submission fence, both acquire paths,
  public native topology suite, and both presentation smoke paths passed.
  See `pixel-fence-acquire-0.1.21-installed.log`.
- Eleven deterministic submission-fence host cases passed.

This is a correctness release, not a performance release. No FPS improvement
is claimed.

## Validation status — not clean

`run-vkgl-smoke.sh` runs vkcube for 60 frames and five two-second FIFO vkmark
scenes on both presentation paths, rejects llvmpipe/swrast/crashes, and prints
actual validation counts. Setting `REQUIRE_VALIDATION_CLEAN=1` makes any
validation diagnostic fatal.

Using that identical public script, exact 0.1.20 and installed 0.1.21 each
reported **59 validation diagnostic events in 8 VUID classes on each path**.
The per-VUID counters match exactly between versions and paths. Thus these are
existing compatibility debt, not a topology-release regression; the release is
**not** described as validation-clean.

Evidence: `smoke-baseline-0.1.20-{dmabuf,shm}.log` and
`smoke-installed-0.1.21-{dmabuf,shm}.log`.

Known classes include vertex input format capabilities, pipeline renderPass
compatibility, image initialLayout, sampler anisotropy, blend/magFilter state,
and queue/acquire semaphore semantics. They remain follow-up work rather than
being hidden by a permissive grep.
