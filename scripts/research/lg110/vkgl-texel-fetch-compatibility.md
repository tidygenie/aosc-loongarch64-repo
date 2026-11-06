# vkgl 0.1.15 texelFetch / GLSL120 compatibility investigation

## Scope and conclusion

This investigation used only the installed vkgl 0.1.15 translation path, the existing LG110 hardware context, independent research probes, and the previously created texelFetch SPIR-V. No IB comparison or performance test was performed. The installed driver and public repositories were not modified.

**No hardware-supported minimal repair via `GL_EXT_gpu_shader4` or GLSL 130 exists on the tested context.** The hardware context advertises neither capability. A forced extension directive can be emitted by SPIRV-Cross, but that does not create the missing hardware/compiler capability, so no candidate vkgl library was built or presented as a fix.

## Relevant source path

Read-only inspection of `/home/houge/infra/vkgl/vkgl.cpp` established:

- Lines 82–105: selects an existing GSGPU/LoongGPU EGL vendor JSON, sets `GBM_BACKEND=gsgpu`, opens `/dev/dri/renderD128`, creates a GBM device and EGL platform display.
- Lines 107–120: initializes EGL, binds `EGL_OPENGL_API`, uses the first EGL config, creates a 16×16 GBM window surface, and explicitly requests `EGL_CONTEXT_MAJOR_VERSION=2`, `EGL_CONTEXT_MINOR_VERSION=1`.
- Lines 121–124: creates and makes that OpenGL context current.
- Lines 2506–2512: creates `spirv_cross::CompilerGLSL`, sets `opts.version = 120`, `opts.es = false`, disables the 420pack extension and push-constant UBO emission, and applies those options.
- Line 2574: calls `c.compile()`; lines 2587–2599 pass the resulting GLSL directly to `glShaderSource`/`glCompileShader` and print the compiler log and GLSL on failure.

Thus the original texelFetch failure is in the GLSL compilation stage, after SPIR-V translation and before any Vulkan command submission.

## Actual hardware capabilities

The independent `gl-capability-probe.c` reproduces the same vendor selection, GBM render node, EGL OpenGL API, first-config, surface, and OpenGL 2.1 context requests. It rejects renderer/version strings containing llvmpipe, swrast, softpipe, or software and exits nonzero if found. The probe exited 0.

Observed in `gl-capability-probe.log`:

| Property | Observed value |
| --- | --- |
| EGL vendor | `/usr/share/glvnd/egl_vendor.d/40_gsgpu.json` |
| EGL version | 1.4 |
| GL vendor | X.Org |
| GL renderer | `LoongGPU(TM) LG110 (DRM 1.0.0 / 7.3.1-mainline-16k-rt-clang-lto, LLVM 8.0.1)` |
| GL version | `2.1 Mesa 18.3.6` |
| GLSL version | `1.20` |
| `GL_EXT_gpu_shader4` exact extension token | **absent** |
| GLSL >= 1.30 | **no** |
| Software-renderer guard | accepted; no forbidden software renderer name |

The renderer string mentions LLVM 8.0.1, but it is the hardware LoongGPU LG110 renderer, not llvmpipe/swrast. The full GL extension string is preserved in the probe log.

## Local SPIRV-Cross extension behavior

An independent `spirv-cross-extension-probe.cpp` links the same local static SPIRV-Cross libraries and uses the exact vkgl GLSL120 options on `vktest-gradient-texel-fetch.frag.spv`.

Relevant implementation details in `/home/houge/infra/vkgl/spirv-cross`:

- `spirv_glsl.cpp` lines 8790–8817 choose the `texelFetch` function name and route legacy GLSL through `legacy_tex_op`.
- `spirv_glsl.cpp` lines 7945–8054 implement legacy texture names. The `texelFetch` branch rejects legacy ES at lines 8028–8029, but for legacy desktop line 8054 simply returns `texelFetch` plus the dimension suffix; it does not call `require_extension_internal("GL_EXT_gpu_shader4")`.
- `spirv_glsl.hpp` lines 253–267 expose `add_header_line` and public `require_extension`. Their implementations are at `spirv_glsl.cpp` lines 17950–17978.

Observed translation outputs:

- Default `spirv-cross-texel-fetch-glsl120.glsl`: `#version 120`, `texelFetch2D(...)`, **no `#extension` line**. Its metadata log reports `REQUIRED_EXTENSIONS_COUNT 0`.
- Explicit `require_extension("GL_EXT_gpu_shader4")` probe: `spirv-cross-texel-fetch-glsl120-forced-ext.glsl` adds `#extension GL_EXT_gpu_shader4 : require` immediately after `#version 120`; metadata reports that one required extension.

This separates translator behavior from target support: vkgl does not request the extension and local SPIRV-Cross does not infer it for legacy-desktop texelFetch; manually forcing the directive is possible, but the actual GL 2.1/GLSL 1.20 target does not advertise the required extension or GLSL 130.

## Candidate and test status

No `GL_EXT_gpu_shader4` or GLSL130 vkgl candidate was built because both prerequisites are absent on the actual hardware context. Injecting an unsupported `#extension ... : require` or raising `#version` to 130 would not be a hardware-supported fix.

No new pipeline run was claimed. The unmodified installed-vkgl evidence remains the prior `vktest-gradient-texel-fetch-failed.log`: translated `texelFetch2D` is rejected, `vkCreateGraphicsPipelines` returns -3, the process exits 1, and there are 0 CS/IB submissions. Whether texelFetch could produce any particular IB layout remains untested and unresolved.

A software emulation of texel fetch or a hardware/driver capability upgrade would be a different, larger change and was not attempted in this bounded investigation.

## Evidence artifacts

All research artifacts are under `/home/houge/infra/research/lg110`:

- Hardware probe: `gl-capability-probe.c`, `gl-capability-probe`, `gl-capability-probe-build.log`, `gl-capability-probe.log`, `gl-capability-probe.exit`.
- SPIRV-Cross probe: `spirv-cross-extension-probe.cpp`, `spirv-cross-extension-probe`, `spirv-cross-extension-probe-build.log`.
- Default translation: `spirv-cross-texel-fetch-glsl120.glsl`, `spirv-cross-texel-fetch-glsl120.log`.
- Forced-extension translation only: `spirv-cross-texel-fetch-glsl120-forced-ext.glsl`, `spirv-cross-texel-fetch-glsl120-forced-ext.log`.
- Original failed variant evidence: `vktest-gradient-texel-fetch-failed.log`, `vktest-gradient-texel-fetch-failed.stdout.log`, `vktest-gradient-texel-fetch-failed.exit`.
- This report: `vkgl-texel-fetch-compatibility.md`.
