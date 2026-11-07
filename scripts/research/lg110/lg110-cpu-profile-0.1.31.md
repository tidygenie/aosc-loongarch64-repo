# LG110 vkgl 0.1.31 CPU profile (2026-11-07)

Exact installed vkgl 0.1.31, hardware vendor GLES/GSPKT path, Wayland FIFO,
640x480. `perf_event_paranoid=2` initially rejected both cycles and software
cpu-clock events. For only the two recording commands it was temporarily set to
1, then restored to 2 by an EXIT trap; no persistent sysctl configuration was
changed. `profile-0.1.31-environment.log` records the restored value and driver
hash. Binary perf.data files are intentionally excluded.

## Samples

| Scene | Duration | Samples | Approximate sampled CPU time | FPS during profiling |
|---|---:|---:|---:|---:|
| vertex | 30 s | 287 | 1.442210875 s | 240 |
| shading | 30 s | 258 | 1.296482250 s | 188 |

The small CPU-time totals show that these FIFO runs spend most wall time outside
measured user CPU execution. Profiling overhead and shared Homebrew/system load
mean these FPS values are not release benchmark results.

Aggregated DSO shares:

| Component | vertex | shading |
|---|---:|---:|
| unresolved kernel/unknown | 42.51% | 36.82% |
| `gsgpu_dri.so` | 23.00% | 28.29% |
| `libvkgl.so` | 5.57% | 5.43% |
| `libc.so.6` | 11.15% | 8.91% |
| `libloong-gpucomp.so` | 2.44% | 4.65% |
| `libwayland-client.so` | 3.14% | 2.33% |

`vkgl_execute_draw` itself has 1.74% vertex and 1.16% shading samples;
`execute_cmds` has 1.05% and 1.55%. LASX memmove has 1.39% and 1.16%. No dominant
vkgl CPU function justifies another micro-optimization release. Unknown samples
were not assigned to gsgpu or the kernel without symbols.

Evidence: `profile-0.1.31-{vertex,shading}.{long.flat.txt,dso.txt,long.call.txt,long.log}`.
The call graphs identify the Vulkan draw/QueueSubmit paths, but the sample counts
are too low to claim a reliable small-function regression or optimization win.

Result: retain 0.1.31 unchanged. Future performance work needs controlled native
state/upload evidence or a workload with a demonstrated vkgl hotspot, not another
getenv/state-cache experiment based on one-percent samples.
