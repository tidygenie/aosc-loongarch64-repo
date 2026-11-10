# LG110 Vulkan primitive restart gap and rejected GL_NV candidate (2026-11-10)

This is a compatibility and native-stream investigation on installed vkgl
0.1.32. It is not a completed primitive-restart implementation, a native Vulkan
driver, or a performance improvement.

## Established gap

The vendor GL 2.1 context is hardware `LoongGPU(TM) LG110`; llvmpipe/swrast are
not used. vkgl reads `VkPipelineInputAssemblyStateCreateInfo::topology` but does
not read `primitiveRestartEnable`.

`vktest-draw-instance.c` now accepts guarded `LG110_PRIMITIVE_RESTART=0|1`
(default 0) and passes the value in the pipeline input-assembly state. With an
indexed UINT16 triangle-strip, three indices, firstInstance=1 and no indirect
draw, restart off/on both produce `007f00ff` and pass the pixel check.

More importantly, the two 16-dword XDMA IBs and complete 672-dword GFX IB are
**byte-for-byte identical**. Word276 remains `0xffffffff` and word281 remains 1
in both streams. Thus this workload confirms that installed vkgl drops the flag;
it does not identify word276 as the restart index.

## Vendor extension is advertised but not functional

`gl-capability-probe.c` now performs a strict, per-call state check on the real
vendor context. It reports:

```text
CAP_GL_NV_primitive_restart yes
PROC_glPrimitiveRestartIndexNV yes
PRIMITIVE_RESTART_STATE enabled=0 index=0xffff enable_error=0x500 set_error=0x0 enabled_error=0x0 get_error=0x0
CAP_GL_NV_primitive_restart_functional no
```

`0x500` is `GL_INVALID_ENUM` from `glEnable(GL_PRIMITIVE_RESTART_NV)`. Setting and
querying the index do not error, but the enable state remains false. The default
probe reports the missing capability and exits successfully;
`LG110_REQUIRE_PRIMITIVE_RESTART=1` deliberately exits nonzero.

## Candidate rejected, not installed

An isolated candidate recorded the pipeline flag, resolved
`glPrimitiveRestartIndexNV`, enabled `GL_PRIMITIVE_RESTART_NV` for indexed draws,
selected `0xffff`/`0xffffffff` by index width, and disabled the state for other
draws. The baseline rebuild was byte-identical to the installed library:

```text
baseline/installed a84c177852cac3f405c1c155d75affcf4ef6d3f7e33dcceed42b6dfb73e041b3
candidate          3a7a627e248ff7296b4e2eaa18c1cb6635917ea775981dde9e07bf342accf05d
```

The candidate was loaded only through a temporary ICD JSON. Restart-off output
and every IB dword matched the installed baseline. Restart-on still produced the
same IBs and pixel; with `VKGL_DEBUG=1`, the candidate itself logged
`vkgl: GL error 0x500 after draw`. Therefore the candidate is rejected: it adds a
failing GL call without implementing restart. No source-driver change, package
version bump, deb, installation, or performance claim resulted.

The default nonindexed triangle regression still passes with raw `007f00ff`, and
invalid `LG110_PRIMITIVE_RESTART=2` is rejected before GPU work.

## Reproduce and evidence

Build the harness with `scripts/tests/vkgl/build-0.1.29-tests.sh`, run from its
`instance` directory, and use the normal indexed settings plus:

```sh
LG110_DRAW_TOPOLOGY=triangle-strip LG110_EXPECT_TOPOLOGY=triangle-strip \
LG110_DRAW_FIRST_INSTANCE=1 LG110_DRAW_INDEXED=1 LG110_DRAW_INDIRECT=0 \
LG110_INDEX_TYPE=16 LG110_PRIMITIVE_RESTART=1 LG110_EXPECT_DRAW_COUNT=3
```

Build the capability probe with strict C11 warnings and `-lEGL -lGL -lgbm`.
External raw logs, complete IB dumps, candidate patch and JSON assertions are in
`/home/houge/infra/gpu/evidence/lg110-primitive-restart-20261110/`.
`candidate-rejection-summary.json` asserts all four restart-off/on IB arrays.
Binaries and full dumps stay outside Git. A future fix needs a demonstrated
functional path; repeating the same NV-enable candidate is not justified.
