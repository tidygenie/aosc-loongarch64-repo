# vkgl-loonggpu 0.1.26-1 coherent-upload evidence (2026-11-06)

## Change

The 0.1.25 draw path used `mem->mapped || uploaded_revision != revision` for
vertex/index buffers and LINEAR textures. A persistent mapping therefore caused
a full upload on every draw, even when all draws belonged to one submission.

0.1.26 adds a QueueSubmit serial. A persistent mapped resource uploads at most
once per submission; a revision change can still force an upload within that
submission. This preserves HOST_COHERENT writes made before QueueSubmit while
removing redundant same-submit uploads. The change does not claim zero copy and
does not alter loader dispatchable destruction or sampler state.

## Public coherence test and negative control

`vktest-coherent-upload.c` keeps its texture mapped. With
`LG110_UPDATE_SECOND_FRAME=1`, it waits for the first submission, changes every
red byte without Flush/Unmap, then submits and reads the second frame.

- Exact 0.1.25: second-frame raw `007f40ff`, PASS.
- 0.1.26 candidate and installed package: the same `007f40ff`, PASS.
- Deliberately broken revision-only negative control: stale `007f00ff`,
  `pixels_valid=0`, exit 1.

The test therefore distinguishes a stale optimization from the correct serial
implementation. Build it with `build-0.1.26-tests.sh`; building executes no GPU.

## Controlled scaling result

`coherent-upload-ab.py` alternates exact 0.1.25 and 0.1.26. The workload uses a
persistent mapped 256x256 RGBA texture, 120 frames, no frame sleep, and
vertexCount=2 so triangle rasterization does not hide upload cost.

| Draws per submit | 0.1.25 median FPS | 0.1.26 median FPS | Change | Paired wins |
|---|---:|---:|---:|---:|
| 1 | 125.259 | 127.227 | +1.57% | 3/5 |
| 64 | 21.683 | 114.972 | **+430.24%** | **5/5** |

For the 64-draw case, median 120-frame elapsed time fell from 5534.197ms to
1043.735ms. After installation, a further three-round comparison against saved
exact 0.1.25 measured 20.443 to 123.326 FPS, **+503.27% (3/3)**. Run-to-run
absolute FPS varies; the large draw-count scaling effect repeats.

This claim is strictly for the controlled persistent-mapping, multi-draw upload
workload. It is not a universal 5x Vulkan or desktop performance claim.

Ordinary vkmark remained near its prior noise range: the two earlier paired
sets for this candidate were (+0.82%, +1.89%, +2.02%) and
(-1.21%, +0.62%, 0.00%) for vertex/texture/shading. No ordinary-scene speedup is
claimed. Raw files: `draw-opt-submit-upload-{1,2}.jsonl` and
`coherent-ab-*-0.1.26.jsonl`.

## Correctness and release

The candidate passed host format/sampler/fence/dispatch-lifetime tests, five
complete vkmark exits, A/B pixels, padded/CopyImage/ClearAttachments, tight
upload, real submission fence and both acquire paths. Candidate and installed
smoke remain **28 events / four VUID classes** on dmabuf and wl_shm. Installed
split-texture LINEAR raw remains `007f22ff`; no llvmpipe/swrast substitution.

- Candidate/package/installed library SHA-256:
  `f44790263dc31c6ceb909afc0c61d7df3369c3cbaa295d52a47ee27c1036a0db`
- Deb SHA-256:
  `81ffef20abcdf2acd529609378999b6597ae1273329426e76e6f36eace500970`
- Signed 25-package AOSC index, MD5, InRelease, HTTPS and isolated apt checks
  passed. Server/HTTPS bytes match and 0.1.26-1 is the highest indexed version.

Evidence: `coherent-second-frame-*-0.1.26.log`,
`pixel-regressions-0.1.26-candidate.log`,
`smoke-{candidate,installed}-0.1.26-{dmabuf,shm}.log`, `package-0.1.26.log`,
`install-0.1.26.log`, and `publish-0.1.26.log`.
