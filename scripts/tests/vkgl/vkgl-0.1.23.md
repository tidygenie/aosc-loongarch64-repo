# vkgl-loonggpu 0.1.23-1 release evidence (2026-11-06)

## Sampler correctness fix

Before 0.1.23, `CreateSampler` allocated a generic object and discarded the
entire `VkSamplerCreateInfo`. Textures remained at their creation-time NEAREST /
CLAMP_TO_EDGE state regardless of the descriptor's sampler.

0.1.23 stores and applies the supported single-level 2D sampler state:

- `VK_FILTER_NEAREST` / `VK_FILTER_LINEAR` for minification and magnification;
- REPEAT, MIRRORED_REPEAT and CLAMP_TO_EDGE for U/V;
- per-Image GL state-value caching, so repeated draws with the same effective
  state do not repeat four `glTexParameteri` calls. State values, not pointer
  identity, are compared, so sampler destruction/address reuse cannot leave a
  stale cache entry.

Compare enable, unnormalized coordinates, nonzero LOD bias, CLAMP_TO_BORDER,
MIRROR_CLAMP_TO_EDGE and cubic filtering return `VK_ERROR_FEATURE_NOT_PRESENT`
instead of being silently accepted. Multilevel LOD selection is not claimed.

## Exact pixel and state proof

The public test creates a 64x64 texture split at texel 16 and a shader sampling
at UV=0.2505. The shader contributes 0.25 of sampled red:

| ICD / sampler | Raw BGRA | Result |
|---|---|---|
| 0.1.22 requested LINEAR | `007f40ff` | expected `007f22ff`, negative control fails |
| 0.1.23 NEAREST | `007f40ff` | pass |
| 0.1.23 LINEAR | `007f22ff` | pass |

Installed-driver GL readback also confirmed LINEAR plus CLAMP/REPEAT/MIRRORED
as `0x812f/0x2901/0x8370`. A three-draw run recorded three texture bindings but
only one actual sampler-state submission, with the same correct pixel.

Evidence: `sampler-create-*`, `sampler-pixel-*`, `sampler-wrap-*-0.1.23.log`,
`sampler-cache-0.1.23.log`, and the two raw `sampler-*-0.1.23.rgba` files.

## Anisotropy deliberately remains unsupported

LG110 advertises `GL_EXT_texture_filter_anisotropic` and maximum 16, but an
isolated prototype changed the LINEAR magnification pixel from 34 to the
NEAREST value 64 when anisotropy16 was applied. The release therefore keeps
`samplerAnisotropy=0` and `maxSamplerAnisotropy=1`; it does not claim working
16x filtering. See [the anisotropy prototype report](../../research/lg110/lg110-sampler-anisotropy.md).

## Validation and other regressions

Both candidate and installed smoke paths still report **28 events in four VUID
classes**, exactly matching 0.1.22's per-class counters. This release does not
claim another validation-count reduction. The anisotropy diagnostic remains
visible because that invalid application request is outside the advertised
capabilities.

The following passed on the final candidate; package and installed bytes are
identical:

- uniform A/B gradient, padded readback, CopyImage and ClearAttachments;
- six tight/padded upload cases, real EGL submission fence and both acquire paths;
- vkcube and five vkmark scenes on dmabuf and wl_shm, GPU-only;
- six deterministic unsupported-sampler rejection cases and the public
  negative controls above.

Logs: `pixel-regressions-0.1.23-candidate.log` and
`smoke-{candidate,installed}-0.1.23-{dmabuf,shm}.log`.

## Performance data — no improvement claim

Five paired three-second rounds ran while Homebrew compilation was active
(observed load average 9.35). Median FPS was:

| Scene | 0.1.22 | 0.1.23 | Change |
|---|---:|---:|---:|
| vertex | 207 | 182 | -12.08% |
| texture | 144 | 122 | -15.28% |
| shading | 159 | 158 | -0.63% |

Vertex debug tracing showed zero texture bindings and zero sampler applications,
so these numbers cannot all be attributed to sampler execution. A byte-identical
control also moved in the opposite direction (median 154 versus 163 vertex FPS).
The data is published without claiming improvement or stable regression
attribution; an idle-system follow-up remains necessary. Real linear filtering
can have a hardware cost even with redundant GL calls removed.

Raw data: `clear-ab-0.1.23.jsonl` and `clear-ab-same-bytes-0.1.23.jsonl`.

## Artifacts and publication

- `vkgl-loonggpu 0.1.23-1 loongarch64`
- Candidate/package/installed library SHA-256:
  `1a2ee24ee1add7a39c78b1d52ea8615bd76d1c49b336f58eadc9e4d11e11e0be`
- Deb SHA-256:
  `444d76daf4b4fd7229d1410285b18ad192f175828fda8623eeace73a163b76f7`
- Signed 22-package AOSC index, MD5, InRelease, HTTPS and isolated apt checks
  passed. Server/HTTPS bytes match and 0.1.23-1 is the highest indexed version.

Logs: `package-0.1.23.log`, `publish-0.1.23.log`.

This is a sampler-state correctness release, not a native-Vulkan or performance
release.
