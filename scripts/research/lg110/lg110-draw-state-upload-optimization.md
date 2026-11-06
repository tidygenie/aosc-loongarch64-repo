# LG110 draw-state and upload optimization experiments (2026-11-06)

## Result: no candidate released

Starting from installed vkgl 0.1.25, this round tested smaller GL error handling,
EGL current-context checks, vendor VAO caching, fixed-state caching, and
coherent upload coalescing. None produced repeatable balanced FPS improvement.
The installed driver and public source therefore remain 0.1.25; there is no
0.1.26 package and no performance-improvement claim.

Every percentage below is the candidate/control median from alternating
three-second 640x480 FIFO runs. Raw JSONL and load records are in
`scripts/tests/vkgl/draw-opt-*`. Rows with `-1`/`-2` are independent repeats.

| Candidate | Run | Vertex | Texture | Shading |
|---|---:|---:|---:|---:|
| glGetError only in VKGL_DEBUG | 1 | +5.22% | -5.10% | +3.76% |
| glGetError only in VKGL_DEBUG | 2 | +1.24% | 0.00% | +0.52% |
| EGL current fast path + GL error | 1 | +2.60% | -2.65% | +1.09% |
| ARB VAO + current + GL error | 1 | +0.84% | +0.65% | +2.12% |
| fixed state + VAO + current + GL error | 1 | 0.00% | +0.65% | +13.21% |
| fixed state + VAO + current + GL error | 2 | -3.36% | +1.39% | 0.00% |
| fixed state + GL error only | 1 | -3.35% | -2.56% | -1.56% |
| per-submit coherent uploads | 1 | +0.82% | +1.89% | +2.02% |
| per-submit coherent uploads | 2 | -1.21% | +0.62% | 0.00% |
| per-submit uploads, vertex device-local=false | 1 | -0.41% | n/a | n/a |

The isolated +13.21% shading result did not reproduce. The isolated fixed-state
candidate then regressed all three scenes. Selecting the first favorable table
would have been misleading.

## VAO capability and actual cache hits

`es2_info` on LG110 advertises `GL_OES_vertex_array_object`, but vkgl creates a
desktop GL 2.1 context, whose extension string instead advertises
`GL_ARB_vertex_array_object`. The candidate loaded the corresponding ARB core
entry points and reported `VAO=1`; no GL draw error was observed.

A debug vertex run recorded 179 draws, one VAO creation and therefore 178 cache
hits. The cache key was Pipeline/Buffer/vertex offset, with invalidation on
Pipeline or Buffer destruction. Exact split-texture LINEAR bytes remained
`007f22ff` and three full exits passed. Correct cache operation still did not
produce repeatable FPS gains.

See `draw-opt-vao-capabilities.log` and `draw-opt-vao.jsonl`.

## Coherent upload design

The original draw path uploads a persistent mapped vertex/index buffer or
LINEAR texture on every draw. The experimental candidate added a QueueSubmit
serial: a mapped resource uploaded at most once per submission, while a memory
revision change could still force another upload in the same submission.

This matches the intended HOST_COHERENT boundary: host writes before a submit
must become visible, but they do not require redundant uploads between draws of
that submit. The candidate passed exact LINEAR pixels and full exits. However,
both ordinary scenes and `vertex:device-local=false` failed to show a repeatable
benefit, so this extra state was not merged without a workload that demonstrates
its value. A future promotion also requires a public persistent-mapping test
that changes data between submissions and validates the second frame.

## Next measurement target

The present vkmark scenes are too small/noisy to justify further cache or flag
churn. Future work should use a controlled multi-draw/large-upload benchmark,
record actual GL upload/state call counts, and then inspect vendor command
submission. A theoretical reduction in API calls is not itself evidence of
higher frame rate or lower latency.
