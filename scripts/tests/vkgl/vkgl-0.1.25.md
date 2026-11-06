# vkgl-loonggpu 0.1.25-1 dispatchable-lifetime evidence (2026-11-06)

## Profiling led to a correctness fix, not a performance claim

A delayed user-space `perf` sample of installed 0.1.24 vertex rendering excluded
three seconds of startup. The 130 runtime samples showed substantial time in
`gsgpu_dri.so`; libc `getenv` accounted for 7.13% through `vkgl_execute_draw`.
See `profile-vertex-0.1.24.txt`. The sample count is small and is used to locate
investigation targets, not to claim a precise total-time breakdown.

Caching `VKGL_DEBUG` removed those lookups but exposed a deterministic vkmark
teardown crash. That candidate was not released. The final 0.1.25 retains the
original debug lookup behavior and contains only the lifetime fix below.

## Root cause: loader dispatch table versus C++ vtable

`Obj` has a virtual destructor, so its first word is normally a C++ vtable.
Vulkan loaders use the first word of dispatchable objects for their dispatch
table. At `vkFreeCommandBuffers`, gdb showed:

```text
command_buffer=0x555555a3d9e0
first_word=0x5555556ba1d0
0x5555556ba1d0: 0x10aded040410aded  0x00007ffff7ea9410
```

The first word points to a heap dispatch table containing the loader magic and a
libvulkan entry, not to a vkgl vtable. A normal C++ `delete` therefore dispatched
through the loader table. Backtraces entered libvulkan/`strcmp` from
`vkFreeCommandBuffers` or `vkDestroyDevice`; ASan reproduced the failure in
`vkgl_FreeCommandBuffers` during a real rendered workload.

Evidence: `dispatch-first-word-gdb.log`, `dispatch-crash-gdb.log`, and
`dispatch-crash-asan.log`.

## Fix

A `delete_dispatchable<T>` helper now performs a qualified, non-virtual
`T` destructor call followed by the matching global `::operator delete`. It is
used for Instance, Device, Queue and CommandBuffer. Object layout and loader ABI
remain unchanged; non-dispatchable objects retain their existing deletion.

After the fix, the same ASan hardware vertex run passed. The default candidate
and installed package each completed five consecutive rendered vkmark exits;
the previous cached-debug candidate crashed consistently at exit. Evidence:
`dispatch-fixed-asan.log` and the installed smoke/lifecycle logs.

## Public lifecycle regression and its limit

`vktest-dispatch-lifetime.c` performs 64 CommandPool/CommandBuffer allocate,
free and destroy cycles, then destroys Device and Instance. The public binary
passes on installed 0.1.25. Exact 0.1.23 and 0.1.24 also pass this minimal test,
so it is **not** presented as a standalone negative control. The actual rendered
crash, loader first-word proof and ASan differential remain the causal evidence.

Build with `build-0.1.25-tests.sh`; the script also builds earlier strict tests
and does not execute a GPU.

## Rejected performance combinations

Raw paired JSONL is retained for every combination:

- cached debug + lifetime fix: vertex -12.81%, texture +2.44%, shading +9.52%;
- the same plus 64-byte function alignment: vertex -7.92%, texture -1.64%,
  shading +10.20%;
- cold-separated lifetime functions: vertex -8.54%, texture -4.62%, shading
  +3.23%.

None was selected. The final dispatch-only candidate versus installed 0.1.24,
using five alternating three-second rounds, was:

| Scene | 0.1.24 median FPS | 0.1.25 median FPS | Change | Paired wins |
|---|---:|---:|---:|---:|
| vertex | 187 | 181 | -3.21% | 1/5 |
| texture | 124 | 115 | -7.26% | 0/5 |
| shading | 149 | 150 | +0.67% | 1/5 |

This is a correctness release with **no performance improvement claim** and
with unfavorable texture/vertex data explicitly retained. Environment/load logs
accompany `clear-ab-*-0.1.25.jsonl`.

## Validation and artifacts

The final candidate passed complete A/B pixels, padded/CopyImage/
ClearAttachments, tight upload, real submission fence and both acquire paths.
Candidate and installed dmabuf/wl_shm smoke remain at **28 events / four VUID
classes**, with no llvmpipe substitution.

- `vkgl-loonggpu 0.1.25-1 loongarch64`
- Candidate/package/installed library SHA-256:
  `00fb9eb672cd9c42a05d94ab4eae6a2637c3cb1fb3555cd746f9dba579747a50`
- Deb SHA-256:
  `a630b3b3e6bcefb90c9cd59703d9a0df49327cc824cd1c74b02b9315e322e57e`
- Signed 24-package AOSC index, MD5, InRelease, HTTPS and isolated apt checks
  passed. Server/HTTPS bytes match and 0.1.25-1 is the highest indexed version.

Logs: `pixel-regressions-0.1.25-candidate.log`,
`smoke-{candidate,installed}-0.1.25-{dmabuf,shm}.log`, `package-0.1.25.log`,
`install-0.1.25.log`, and `publish-0.1.25.log`.
