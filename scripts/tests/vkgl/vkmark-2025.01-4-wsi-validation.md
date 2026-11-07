# vkmark 2025.01-4 WSI and validation cleanup (2026-11-07)

## Responsibility: benchmark, not vkgl

Exact vkgl 0.1.26 produced 28 validation events in the five-scene vkmark smoke.
The same count reproduced with an unmodified upstream vkmark build at commit
[`83a128d65b1adddfacb4fa2e555a7e90817a7266`](https://github.com/vkmark/vkmark/commit/83a128d65b1adddfacb4fa2e555a7e90817a7266).
The causes were in vkmark:

1. It waited an acquire fence before reusing an acquire semaphore, but the later
   QueueSubmit that waited that semaphore had no submission fence. Acquire
   completion does not prove completion of a subsequent queue wait.
2. Each scene reused one render-finished semaphore across all swapchain images;
   a new signal could overlap a previous present wait.
3. OPTIMAL texture images used PREINITIALIZED rather than UNDEFINED.
4. The anisotropy=16 scene did not clamp to `maxSamplerAnisotropy` (LG110/vkgl
   advertises 1), and enabled anisotropy even when the limit could not support it.

The public `vktest-wsi-semaphore.c` isolates the first issue. With a real submit
fence wait before each acquire-semaphore reuse, both dmabuf and wl_shm complete
30 cycles with zero acquire-pending and signal-in-use events. Its empty submits
intentionally produce ten PRESENT_SRC layout events because they perform no
image transition; it is a synchronization test, not a pixel-valid render test.

## Patch and measured reduction

The package patch adds separate initially-signaled per-frame submit fences,
waits them before semaphore reuse, passes them through `VulkanImage`, and
allocates render-finished semaphores per swapchain image for all seven scenes.
It also changes the texture transition to UNDEFINED and clamps/disables
anisotropy according to the physical-device limit.

| vkmark build | dmabuf events | wl_shm events |
|---|---:|---:|
| upstream baseline + exact vkgl 0.1.26 | 28 | 28 |
| WSI semaphore fixes only | 8 | 8 |
| complete 2025.01-4 patch, staged | 0 | 0 |
| complete 2025.01-4 patch, installed | 0 | 0 |
| installed after 2026-11-07 OS updates | 0 | 0 |

All runs executed vertex/texture/shading/effect2d/desktop on LG110, plus vkcube
and vulkaninfo. This is validation correctness, not a claimed FPS improvement.
The remaining anisotropy16 behavior is not misrepresented: on a limit-1 device
vkmark now disables it rather than pretending vkgl supports it.

## Package

- `vkmark 2025.01-4 loongarch64`; source commit and tarball SHA-256 are pinned in
  `packages/graphics-drivers/vkmark/spec`.
- All five window-system plugins remain enabled: headless, display, Wayland,
  XCB, KMS.
- Meson unit tests: 1/1 PASS.
- Deb SHA-256:
  `5cbda2cb85b902f590b6fdf0d2e8ad4cd1f84887c8f27020b6596d6a5085356a`.
- Signed 27-entry repository checks, MD5, InRelease, HTTPS and isolated apt
  passed; server/HTTPS bytes match and revision 4 is the highest indexed vkmark.
- vkgl remains 0.1.26 with library SHA-256
  `f44790263dc31c6ceb909afc0c61d7df3369c3cbaa295d52a47ee27c1036a0db`;
  post-update split-texture LINEAR raw remains `007f22ff`.

Evidence: `vkmark-{upstream-base,wsi-fixed,full-fix}-0.1.26-{dmabuf,shm}.log`,
`vkmark-{staged,installed}-2025.01-4-{dmabuf,shm}.log`,
`post-os-2026-11-07-{dmabuf,shm}.log`, and package/unit/install/publish logs.
