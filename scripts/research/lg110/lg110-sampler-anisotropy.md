# LG110 sampler anisotropy prototype — not enabled (2026-11-06)

This records why vkgl 0.1.23 does **not** advertise `samplerAnisotropy`, despite
the vendor extension string. It is a correctness boundary, not a performance
claim.

## Hardware capability

`eglinfo -p wayland -a gles -l -s` under the active LG110 Wayland session reports:

- renderer: `LoongGPU(TM) LG110` on the 7.3.1 clang-LTO kernel;
- `GL_EXT_texture_filter_anisotropic` present;
- `GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT = 16`.

Evidence: `sampler-gles-wayland-info.log`. This query used the Wayland hardware
platform, not a software Vulkan replacement.

## Prototype and negative pixel result

An isolated prototype (`fd4056cc…`, not the 0.1.23 release) exposed feature=1,
max=16 and called `glTexParameterf(GL_TEXTURE_MAX_ANISOTROPY_EXT, 16)`. GL state
readback returned LINEAR (`0x2601`), CLAMP_TO_EDGE (`0x812f`) and anisotropy 16,
with no GL error.

The sampler test uses a 64x64 single-level texture split at texel 16 and samples
at UV=0.2505 while magnifying it to 640x480. The fragment shader contributes
0.25 of the sampled red channel:

- NEAREST: red 64, raw BGRA `007f40ff`;
- LINEAR without anisotropy: red 34, raw `007f22ff`;
- LINEAR plus prototype anisotropy16: red 64, raw `007f40ff`, unexpectedly
  matching NEAREST instead of preserving the LINEAR magnification result.

Anisotropy should not turn this magnification sample into nearest filtering.
State acceptance alone is therefore insufficient evidence of working sampler
semantics.

Evidence: `sampler-aniso-prototype.log`, `sampler-aniso-prototype.rgba`, and
`sampler-linear-reference.rgba`.

## Release decision and future gate

vkgl 0.1.23 keeps `samplerAnisotropy=VK_FALSE` and `maxSamplerAnisotropy=1.0`.
It implements and tests filter/wrap state only. vkmark's anisotropy16 request
remains outside the advertised capabilities and its VUID remains visible.

Before enabling the feature, a future candidate must pass both:

1. magnification invariance against the LINEAR reference;
2. a controlled minification/mip-chain test with an expected-pixel oracle and
   actual anisotropy state readback.

No working 16x anisotropy, performance improvement, or native-Vulkan support is
claimed from this prototype.
