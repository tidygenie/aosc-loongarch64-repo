# LG110 vkgl hot-path experiments (2026-11-06)

These were **experimental candidates, not released performance improvements**.
The control was the installed 0.1.19 library with SHA-256
`04e51457f62e56b4a3443c3cd99e6a0d7a0cffb74e9b72fff765d7c327da38e3`.
Both experiments passed the hardware dmabuf smoke suite (5/5, zero validation
errors, no llvmpipe); the mapped-only candidate also passed the wl_shm suite.
No native-Vulkan claim is involved.

## Method

`clear-ab.py` alternated control and candidate, reversing order every round.
Six rounds used 640x480 FIFO vkmark, four seconds per scene, with
`vertex,texture,shading,effect2d,desktop`. Each experiment completed 60/60
runs successfully. The script now accepts `--scenes`; its default remains the
original three scenes. Raw data are `mapped-upload-ab-0.1.20.jsonl` and
`draw-hotpath-ab-0.1.20.jsonl`. `summarize-ab.py <jsonl>` independently checks
that every run succeeded and every scene/round has exactly two variants, then
reproduces the medians, changes, wins, and ties below.

## Experiment A: one mapped-memory invalidation per QueueSubmit

Candidate SHA-256:
`aea84ab9cc8b85c631026f754bbf7e1f46e3ab8bb1a47a366f9dbbaed6d5329e`.
It avoided treating a persistent mapping as dirty on every draw, advancing its
revision once per submission instead.

| Scene | Base median FPS | Candidate median FPS | Change | Candidate wins/6 |
|---|---:|---:|---:|---:|
| vertex | 231 | 228.5 | -1.08% | 3 |
| texture | 160 | 152.5 | -4.69% | 2 |
| shading | 190.5 | 188 | -1.31% | 1 |
| effect2d | 59.5 | 59.5 | 0.00% | 1 (4 ties) |
| desktop | 120 | 122.5 | +2.08% | 5 (1 tie) |

The isolated desktop result does not offset the mixed texture/shading results;
no general improvement is claimed.

## Experiment B: mapped submission plus draw hot-path guards

Candidate SHA-256:
`f42202faf9094eeeda5c5a4d0a8e5ecbd37787fb025759053d78687a486afbe4`.
This additionally avoided redundant `eglMakeCurrent` calls and made the
post-draw `glGetError` conditional on `VKGL_DEBUG`.

| Scene | Base median FPS | Candidate median FPS | Change | Candidate wins/6 |
|---|---:|---:|---:|---:|
| vertex | 231 | 235.5 | +1.95% | 3 (1 tie) |
| texture | 164.5 | 158.5 | -3.65% | 1 |
| shading | 187.5 | 185.5 | -1.07% | 2 |
| effect2d | 59 | 59 | 0.00% | 2 (1 tie) |
| desktop | 120 | 119.5 | -0.42% | 0 (3 ties) |

This candidate was rejected as a performance change and was not packaged.
The development source was restored from the unchanged public 0.1.19 package
source; rebuilding reproduced the installed control SHA-256 exactly.
