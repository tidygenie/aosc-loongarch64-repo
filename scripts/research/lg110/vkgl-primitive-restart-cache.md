# LG110 primitive-restart segment cache candidate (2026-11-11)

This is a rejected performance candidate on top of installed/published vkgl
0.1.33. It is not 0.1.34, not installed, and not an AOSC deb. The public patch
[`vkgl-primitive-restart-cache.patch`](vkgl-primitive-restart-cache.patch)
reproduces the final candidate source from the 0.1.33 baseline. From a directory
containing that baseline `vkgl.cpp`, apply it with `patch -p1 < PATCH`.

## What was measured

0.1.33 scans restart-enabled index ranges on every submitted draw. The candidate
caches nonempty segments by buffer, memory, index offset, first/count, index type,
and memory revision. Mapped memory additionally requires the current submit
serial. Buffer destruction and rebinding erase the cache, including rebinds to
the same memory at a different offset.

The final external-map design leaves the 0.1.33 `Buffer` layout unchanged. Only
restart-enabled draws allocate/query a cache; restart-disabled draws do not.

With `VKGL_DEBUG=1`, the real UINT16 sentinel strip over 120 frames produced:

```text
final static: 1 scan, 119 cache hits, raw 663300ff, PASS
final mapped: 120 scans, 0 cache hits, raw 007f00ff after frame-1 update, PASS
```

Thus 119 of 120 static scans (99.17%) were eliminated. This is a **scan-count
reduction, not an FPS improvement**. The mapped harness keeps the index buffer
mapped, replaces the sentinel with ordinary index 3 on frame 1, and performs its
only readback on that frame. Both installed 0.1.33 and the candidate produce the
new `007f00ff` result; the candidate correctly rescans every submit. UINT32
mapped update also passed with 120 scans and zero hits. Fan and indirect
UINT16/32 restart cases passed, and no-sentinel native IBs remained identical to
0.1.33.

Final candidate library SHA-256:
`80e73b801590744e7b7606e06442a8e56c6c584d720de7ad4af5c58526b0a856`.
Installed 0.1.33 remains
`2924be31e12c097d430c79b678b0761e095c8884561ebc40afdb0240e8cd22b3`.

## Performance rejection

Two independent six-round AB/BA vkmark matrices were run serially with hardware
LG110, Khronos validation, 640x480 FIFO, and four seconds per scene.

The first revision embedded cache state in every `Buffer` (candidate `b5ac28fe`):

| Scene | Median paired change |
| --- | ---: |
| vertex | -4.16% |
| texture | +0.75% |
| shading | -0.29% |

The second revision moved cache state to an external map and restored the Buffer
layout (candidate `807492c2`):

| Scene | Median paired change |
| --- | ---: |
| vertex | -3.38% |
| texture | -2.33% |
| shading | -3.65% |

Individual pairs are noisy, but the second matrix is not an acceptable default
path regression control. The final rebind-invalidation fix was then rebuilt and
functionally reverified; it does not justify ignoring these negative results.
No cache version was installed, packaged, or published. A future attempt needs
an isolated CPU benchmark and a convincing paired default-path control rather
than repeating either candidate blindly.

Evidence is outside Git under
`/home/houge/infra/gpu/evidence/lg110-primitive-restart-cache-20261111/`,
`/home/houge/infra/gpu/evidence/vkgl-primitive-restart-cache-ab-20261111/`, and
`/home/houge/infra/gpu/evidence/vkgl-primitive-restart-cache-v2-ab-20261111/`.
