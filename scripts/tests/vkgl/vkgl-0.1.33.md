# vkgl-loonggpu 0.1.33-1 primitive restart release

Date: 2026-11-10

## Compatibility correction

0.1.32 read only the pipeline topology and ignored
`VkPipelineInputAssemblyStateCreateInfo::primitiveRestartEnable`. The vendor's
advertised `GL_NV_primitive_restart` path cannot be used: its enable returns
`GL_INVALID_ENUM`; core and fixed-index enables fail identically. See
[`../../research/lg110/lg110-primitive-restart.md`](../../research/lg110/lg110-primitive-restart.md).

0.1.33 implements restart with the existing index-buffer CPU shadow. For an
indexed line-strip, triangle-strip, or triangle-fan pipeline with restart enabled,
it scans the requested index range once per submitted draw, splits at `0xffff`
or `0xffffffff`, and issues one ordinary `glDrawElements` per nonempty segment.
The segments are reused across software instances. `firstIndex`, signed
`vertexOffset`, direct/indirect commands, and index offsets retain their existing
translation. Empty segments are skipped; no-sentinel input remains one draw.

Restart-disabled and list-topology draws do not scan or allocate segments. The
final source keeps the original single-draw branch for the default path. This is
a compatibility implementation, not native Vulkan or a new zero-copy path.

## Real sentinel correctness

The new `vktest-primitive-restart.vert` uses six in-bounds vertices. Both restart
sentinels safely map to vertex 3 when restart is disabled. The resulting
connecting primitive covers the center, while restarted segments 0/1/2 and 3/4/5
do not. This gives an exact observable distinction without out-of-bounds access.

| Path | Restart off | Restart on |
| --- | --- | --- |
| Installed 0.1.32, UINT16 strip | `007f00ff` | `007f00ff` (wrong) |
| Final 0.1.33, UINT16 strip | `007f00ff` | `663300ff` clear |
| Final 0.1.33, UINT32 strip | tested unchanged | `663300ff` clear |
| Installed 0.1.32, UINT16 fan | `007f00ff` | `007f00ff` (wrong) |
| Final 0.1.33, UINT16 fan | tested unchanged | `663300ff` clear |
| Final 0.1.33, indirect UINT16/UINT32 | tested unchanged | both `663300ff` clear |

The final candidate passed six key installed/candidate sentinel cases after the
last source revision, plus 16 specification-valid direct/indirect UINT16/32
firstIndex/baseVertex cases and all 12 two/first/zero-instance instance-rate
regressions. Installed 0.1.33 repeated six strip/fan/UINT32/indirect cases with
the same exact pixels and no GL errors. A two-instance restart case also passed.

With no sentinel, restart-on 0.1.33 remains raw `007f00ff`; both 16-dword XDMA IBs
and the complete 672-dword GFX IB are byte-identical to 0.1.32. This confirms no
native-state or packet change on the ordinary path. Validation-enabled hardware
vkmark and vkcube smoke passed after installation, with no validation errors or
software renderer.

Build the tests with `build-0.1.33-tests.sh`, run from its `restart` directory,
and set `LG110_RESTART_SENTINEL=1`, `LG110_DRAW_VERTEX_COUNT=7`,
`LG110_PRIMITIVE_RESTART=0|1`, and `LG110_EXPECT_CLEAR=0|1` in addition to the
normal indexed topology/type settings. Invalid parameters are rejected before
GPU work. The final harness and shader rebuild byte-identically.

## Build identity and performance control

The pristine baseline rebuild was byte-identical to installed 0.1.32:

```text
0.1.32 baseline a84c177852cac3f405c1c155d75affcf4ef6d3f7e33dcceed42b6dfb73e041b3
0.1.33 fixed    2924be31e12c097d430c79b678b0761e095c8884561ebc40afdb0240e8cd22b3
```

Six serial AB/BA vkmark rounds used the exact final candidate, 640x480 FIFO,
four seconds per scene, and Khronos validation. Wrong-GPU, software-renderer,
validation-error, missing-FPS, and failed-process guards all passed.

| Scene | Baseline median FPS | Fixed median FPS | Median paired change | Pair range |
| --- | ---: | ---: | ---: | ---: |
| vertex | 222.0 | 221.0 | -0.01% | -2.27% to +4.72% |
| texture | 151.5 | 150.0 | -0.99% | -3.23% to +7.14% |
| shading | 193.0 | 192.0 | -0.25% | -2.03% to +4.40% |

These noisy paired results are a default-path regression control, **not a
performance improvement claim**. The raw summary SHA-256 is
`2f4cbdf3346d787aee1204a35f044f3cd42c42af0adb52d77fea02171cf74610`.

## Package, installation, and publication

- Package: `vkgl-loonggpu_0.1.33-1_loongarch64.deb`.
- Deb SHA-256:
  `f49eb88207d44fffb81cfa6026fe0b0fb0b34b3f7491391f3241902e5751aece`.
- Packaged, independently rebuilt, candidate, and installed libraries all match
  the 0.1.33 fixed SHA above.
- The authenticated 0.1.32 rollback was retained and byte-verified before
  installation; 0.1.31 was then removed by the guarded latest-plus-one plan.
- Signed AOSC publication passed dry-run/deploy, 19-entry Packages indexes,
  Release/InRelease and isolated apt checks. Local, VPS, and HTTPS current and
  rollback deb bytes match; each pool contains exactly 19 indexed deb files.
- Root README lists only 0.1.33 as latest while retaining 0.1.32 in the pool.

Evidence remains outside Git under
`/home/houge/infra/gpu/evidence/lg110-primitive-restart-sw-20261110/` and
`/home/houge/infra/gpu/evidence/vkgl-primitive-restart-sw-v2-ab-20261110/`.

This release fixes the tested core strip/fan restart semantics through
Vulkan-over-GL. It is not native Vulkan, generalized packet documentation,
maximum-performance proof, or completion of the larger LoongArch/AOSC objective.
