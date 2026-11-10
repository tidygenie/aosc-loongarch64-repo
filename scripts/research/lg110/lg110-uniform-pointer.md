# LG110 fragment-uniform GPU VA pointer: causal separation

Date: 2026-11-11

## Result

For the tested fragment-uniform stream, GFX IB words 240/241 are a little-endian
64-bit GPU VA pointing at the exact uniform vec4 whose contents were previously
proven causal. Changing only this pointer to a second, controlled vec4 changes
the output color; writing that second vec4 without changing the pointer does
not.

This establishes pointer semantics for this workload and binary layout. It is
not a complete packet disassembler, a general descriptor format, a stable UAPI,
or a performance optimization.

## Identical baseline and tools

- vkgl 0.1.35 library SHA-256:
  `78c963855189836046520f1ee9f327057bd61d7a75c2b8e40b8988f3ea473668`.
- Vendor `gsgpu_dri.so` SHA-256:
  `aff7101ab877db0026fa0f8274e03a579f1b4fd2c16fabe2478d08f224d0da0a`.
- Workload SHA-256:
  `8fe10d351674a8639b540df0fce9511c8836f40f4ca10609fe30882acafbf120`,
  unchanged from [`lg110-uniform-state.md`](lg110-uniform-state.md).
- New isolated pointer-mutation tracer SHA-256:
  `21d997e44dbc42376b298daf0a9755832817ac55b137fa7696efc826be7a8233`.

The earlier passive and direct-mutation sources remain unchanged. The new
[`lg110-uniform-pointer-mutation.c`](lg110-uniform-pointer-mutation.c) is
explicitly active only under its environment opt-ins.

## Locate the VA in the complete IB

A passive red run with full VA metadata reported:

```text
handle 8: va=0x00000001010c0000 bo_offset=0 size=0x1000000
uniform vec4: handle 8 + 0x3cb0 = VA 0x00000001010c3cb0
```

The complete 544-dword GFX draw IB contains this exact pair:

```text
word240 = 0x010c3cb0
word241 = 0x00000001
```

The readback IB has no pointer to offset `0x3cb0`. Other handle-8 VAs occur in
the GFX IB, but they are not named here merely because their addresses fit the
same mapping.

Reports:

- [`uniform-pointer-ib-address-search.txt`](uniform-pointer-ib-address-search.txt),
  SHA-256 `df351e7291b3e1da31d75db70232d45f1dd9af36fbe795e4b0f3fc123bf4932c`.
- [`uniform-pointer-handle8-va-pairs.txt`](uniform-pointer-handle8-va-pairs.txt),
  SHA-256 `93e20a42277183602b95e43ab6ef35151d1eb1fcf44eeed8ce28ba4e99070441`.

## Auxiliary-slot and pointer controls

The next 16-byte slot, handle 8 + `0x3cc0`, is zero in the red baseline. The new
tracer supports two mutually exclusive opt-ins before submission 2:

- `LG110_MUTATE_AUX=1`: require four zero dwords at `0x3cc0`, then write green
  `(0,1,0,1)` there. Do not change the IB.
- `LG110_MUTATE_POINTER=1`: perform the same guarded auxiliary write and require
  IB words 240/241 to equal `0x010c3cb0,0x00000001`. Then change only word240 to
  `0x010c3cc0`, leaving the high dword and every other IB dword unchanged.

The IB guard also requires a valid IB chunk, at least 242 dwords, a resolved VA,
and a live user mapping covering both words. A missing mapping or unexpected
old value rejects the mutation. The original ioctl payload is forwarded
unchanged; only the separately tracked userspace BO/IB memory is changed.

Exact serial results:

| Run | Data at `0x3cc0` | IB word240 | Raw top/bottom BGRA | Test result |
|---|---|---|---|---|
| passive | unchanged zero | unchanged | `0000ffff000000ff` | red PASS |
| auxiliary only | green | unchanged | `0000ffff000000ff` | red PASS |
| pointer | green | `0x010c3cc0` | `00ff00ff000000ff` | expected red-test failure, exit 1 |

The passive run has no mutation records. The auxiliary run has exactly one
`status=aux_written` and no pointer record. The pointer run has exactly one
`LG110POINTER ... status=mutated` and one `status=aux_and_pointer_mutated`, with
no guard rejection or missing mapping.

## Full-stream differential

Comparing complete captured IBs:

| Submission | Dwords | Passive vs auxiliary | Auxiliary vs pointer |
|---|---:|---:|---:|
| 1 XDMA | 16 | 0 | 0 |
| 2 GFX draw | 544 | 0 | 1: word240 only |
| 3 GFX readback | 272 | 0 | 0 |

Thus the red-to-green transition between the last two runs is associated with a
single changed IB dword after both runs received the same auxiliary green data.
Together with the unchanged red auxiliary control, this is causal evidence that
the tested pointer selects the fragment-uniform vector.

Report: [`uniform-pointer-ib-differential.txt`](uniform-pointer-ib-differential.txt),
SHA-256 `c826c02c2811023bf4a205ee0f790b3f1f63e168205eaaa3a53b2c932d3bd7be`.

## Boundaries

Run these experiments only in an active hardware-rendered Wayland session,
serially, with the same one-frame workload and bounded trace limits. Do not add
the active preload to a desktop, installed ICD, or system environment. Handles,
VAs, offsets, and word indices must be re-derived for another shader, driver, or
stream. No production driver was modified, no deb was produced, and no latency,
FPS, native-Vulkan, or zero-copy claim follows from this result.

Complete local evidence is in
`/home/houge/infra/gpu/evidence/lg110-uniform-descriptor-20261111/`. Public raw
text traces use the `uniform-pointer-` prefix; compiled libraries and SPIR-V are
not committed.
