# vkgl-loonggpu 0.1.22-1 release evidence (2026-11-06)

## Fix and scope

0.1.21 exposed RGBA8 sampled/color/transfer support, but omitted linear-filter
and color-blend feature bits. Its default format case also advertised sampled
support for float vertex formats while returning `bufferFeatures=0`. Validation
therefore reported missing capabilities for work the actual GL backend already
performed.

0.1.22 makes only these capability corrections:

- B8/R8 RGBA8 UNORM and SRGB, linear and optimal tiling:
  add `VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT` and
  `VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT` to the existing sampled,
  color-attachment and transfer bits.
- R32/R32G32/R32G32B32/R32G32B32A32 SFLOAT:
  report `VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT` in `bufferFeatures`, matching
  `gl_type_for_format()` and `glVertexAttribPointer()` support.
- The core `GetPhysicalDeviceFormatProperties2` wrapper returns the same values.

No new rendering fast path, sampler feature, semaphore model, or FPS claim is
introduced.

## Exact validation reduction

The same public `run-vkgl-smoke.sh` ran 60 vkcube frames and five two-second
FIFO vkmark scenes with validation on both presentation paths.

| Version/path | Events | VUID classes |
|---|---:|---:|
| 0.1.21 dmabuf / wl_shm | 59 each | 8 |
| 0.1.22 candidate and installed, dmabuf / wl_shm | 28 each | 4 |

Exactly 31 events and these four classes were removed on each path:

- `VUID-VkVertexInputAttributeDescription-format-00623`
- `VUID-VkGraphicsPipelineCreateInfo-renderPass-06041`
- `VUID-vkCmdDraw-magFilter-04553`
- `VUID-vkCmdDraw-blendEnable-04727`

The remaining per-VUID counters are identical to 0.1.21; no new class appeared:

- optimal-tiling image with PREINITIALIZED layout (invalid application input);
- anisotropy 16 requested while the currently exposed maximum is 1 (requires a
  real feature/backend decision, not an arbitrary limit increase);
- queue/acquire binary-semaphore reuse (requires actual semaphore/present state).

The release is **not validation-clean**. These remaining 28 events are explicit
follow-up work, not filtered out.

Evidence: `smoke-{candidate,installed}-0.1.22-{dmabuf,shm}.log` and the previous
`smoke-installed-0.1.21-{dmabuf,shm}.log` controls.

## Tests

`build-0.1.22-tests.sh` builds the new strict format test and existing
upload/fence tests without running a GPU. The same format binary fails on exact
0.1.21 with missing color/vertex/FormatProperties2 capabilities and passes on
installed 0.1.22: four color formats, four vertex formats, and FormatProperties2.

Candidate and installed checks also passed:

- A/B gradient, padded readback, CopyImage and ClearAttachments with exact
  expected pixels, guards and sentinels;
- six tight/padded upload cases and real EGL submission fence;
- dmabuf and wl_shm acquire;
- six basic draw topologies plus adjacency/PATCH rejection;
- eleven deterministic submission-fence host cases.

Logs: `format-baseline-0.1.21.log`, `format-0.1.22-installed.log`,
`pixel-regressions-0.1.22-candidate.log`, `topology-0.1.22-candidate.log`,
`pixel-fence-acquire-0.1.22-installed.log`, and
`submit-fence-host-0.1.22.log`.

## Artifacts and publication

- `vkgl-loonggpu 0.1.22-1 loongarch64`
- Candidate/package/installed library SHA-256:
  `759dadbb7de2b44ed7cd01fefb9ed2c092e26e84b779bc247afa5d8f393195d2`
- Deb SHA-256:
  `74316cc6d4574cef6e62584cee567ee4bdbf8aef0078c27f5e4d57eb3ef5d72f`
- Signed 21-package AOSC index, MD5, InRelease, HTTPS and isolated apt checks
  passed. Server and HTTPS deb bytes match; 0.1.22-1 is the highest version.

Logs: `package-0.1.22.log`, `publish-0.1.22.log`.

This is a capability-reporting correctness release. No performance improvement
or native-Vulkan claim is made.
