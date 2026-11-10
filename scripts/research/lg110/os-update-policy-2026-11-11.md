# AOSC OS update audit — 2026-11-11

## Method

The host was audited with `oma upgrade --dry-run` only. No package install,
remove, upgrade, `autoremove`, initramfs regeneration, boot-entry change, or
reboot was performed. The SHA-256 of `/var/lib/dpkg/status` was identical
before and after the final simulation:
`d6891e5a8666ffcbb489fb1c28175c792449e5edd1f440ec388e2198f429c4a9`.

The complete command output and package-policy baseline are retained outside
Git in `infra/gpu/evidence/os-update-audit-20261111/`.

## Available updates and decisions

| Package | Installed | Candidate | Decision |
|---|---|---|---|
| Mesa | 1:26.1.5-1 | 1:26.2.6 | Keep installed pending a controlled graphics candidate and full LG110/vkgl regression. The vendor `mesa-vulkan-loonggpu` remains 26.1.5-1; do not split this stack blindly. |
| Qt 5 | 1:5.15.18+webengine5.15.19+webkit5.212.0+kde20251209-5 | 1:5.15.19+webengine5.15.19+kde20260521 | Defer to a controlled Wayland/Plasma candidate; this is a desktop-stack change, not a compiler or kernel update. |
| PyQt5 | 5.15.11-1 | 5.15.11-2 | Evaluate together with Qt, not as an isolated graphics test. |
| fcitx5-qt | 1:5.1.16 | 1:5.1.16-1 | Evaluate together with Qt and input-method integration. |
| libcamera | 0.7.2-1 | 0.8.0 | No current LG110/Vulkan development dependency justifies a host upgrade. |
| PipeWire | 1.6.10 | 1.6.11 | No measured development benefit in this audit; leave for normal desktop maintenance rather than mixing with GPU tests. |
| WirePlumber | 0.5.18 | 0.5.19 | Same controlled-maintenance decision as PipeWire. |

## Rationale

The running kernel remains `7.3.1-mainline-16k-rt-gcc-o3`, vkgl remains
0.1.33-1, and liblol remains 0.1.12. None of the seven candidates is a
compiler, kernel, DKMS, or vkgl update. Installing every available update would
add uncontrolled variables to LG110 hardware-rendering measurements.

Mesa 26.2.6 remains specifically unsuitable for an uncontrolled host upgrade:
the installed vendor driver and vkgl depend on the existing graphics stack.
Any future candidate must preserve the vendor GSPKT/gsgpu path and rerun native
hardware renderer identification, vkgl correctness tests, Wayland/Plasma
checks, and paired performance tests. Software rasterizers are not an accepted
fallback.

The next useful update gate is therefore a controlled graphics/desktop
candidate, not a blanket host upgrade. PipeWire, WirePlumber, and libcamera can
be reviewed independently when there is a concrete desktop or hardware need.
