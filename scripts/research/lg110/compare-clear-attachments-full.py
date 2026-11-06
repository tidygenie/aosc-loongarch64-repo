#!/usr/bin/env python3
"""Strictly compare the recorded ClearAttachments A/B full GFX stream.

Expected physical layout per run: one XDMA CS, then 121 successful GFX CS:
seq2=800 dwords, seq3=272 dwords, seq4..122=400 dwords each. The 121 GFX
submissions correspond to the test's 120 application frames; no semantics are
inferred for the additional physical submission.
"""
import hashlib
import re
import struct
import sys

if len(sys.argv) != 3:
    raise SystemExit(f"usage: {sys.argv[0]} A-full.log B-full.log")


def parse(path):
    text = open(path, encoding="utf-8", errors="strict").read()
    cs = [int(x) for x in re.findall(r"^LG110CS seq=\d+.* ret=(-?\d+)", text, re.M)]
    if len(cs) != 122 or any(cs):
        raise SystemExit(f"{path}: expected 122 successful CS, got {len(cs)}")
    if "max_ib_dwords=2048" not in text:
        raise SystemExit(f"{path}: expected the 2048 full-line tracer")
    sizes = {}
    xdma = {}
    for m in re.finditer(r"^LG110IB seq=(\d+).* ib_bytes=(\d+) ip_type=(\d+)", text, re.M):
        seq, size, ip_type = map(int, m.groups())
        if ip_type == 0:
            if seq in sizes:
                raise SystemExit(f"{path}: duplicate GFX header seq {seq}")
            sizes[seq] = size
        elif ip_type == 2:
            xdma[seq] = size
    if xdma != {1: 64}:
        raise SystemExit(f"{path}: unexpected XDMA layout {xdma}")
    dumps = {}
    declared = {}
    for line in text.splitlines():
        if not line.startswith("LG110IBDUMP "):
            continue
        seq = int(re.search(r"seq=(\d+)", line)[1])
        if seq not in sizes:
            continue
        if seq in dumps:
            raise SystemExit(f"{path}: duplicate GFX dump seq {seq}")
        if " status=ok " not in line:
            raise SystemExit(f"{path}: unresolved dump seq {seq}")
        declared[seq] = int(re.search(r" dwords=(\d+)", line)[1])
        dumps[seq] = [int(x, 16) for x in re.search(r" data=(.*)$", line)[1].split()]
    return sizes, dumps, declared


a = parse(sys.argv[1])
b = parse(sys.argv[2])
expected = {2: 800, 3: 272, **{seq: 400 for seq in range(4, 123)}}
for name, trace in zip(("A", "B"), (a, b)):
    sizes, dumps, declared = trace
    if sorted(dumps) != list(range(2, 123)):
        raise SystemExit(f"{name}: missing or extra GFX dumps")
    for seq, words_expected in expected.items():
        if sizes.get(seq) != words_expected * 4:
            raise SystemExit(f"{name}: seq {seq} unexpected IB bytes {sizes.get(seq)}")
        if declared.get(seq) != words_expected or len(dumps[seq]) != words_expected:
            raise SystemExit(
                f"{name}: seq {seq} incomplete: declared={declared.get(seq)} "
                f"serialized={len(dumps[seq])} expected={words_expected}"
            )
        if len(dumps[seq]) * 4 < sizes[seq]:
            raise SystemExit(f"{name}: truncated seq {seq}")

different = []
for seq in range(2, 123):
    for index, (x, y) in enumerate(zip(a[1][seq], b[1][seq])):
        if x != y:
            different.append((seq, index, x, y))
raw_a = b"".join(struct.pack("<I", x) for seq in range(2, 123) for x in a[1][seq])
raw_b = b"".join(struct.pack("<I", x) for seq in range(2, 123) for x in b[1][seq])
total = sum(expected.values())
print("logical_frames=120 physical_gfx_pairs=121 successful_CS_per_run=122")
print("seq2=800/800 seq3=272/272 seq4-122=400/400")
print("complete_gfx_pairs=121 truncated=0")
print(f"equal={total - len(different)} different={len(different)} total_dwords={total}")
print(f"A_sha256={hashlib.sha256(raw_a).hexdigest()}")
print(f"B_sha256={hashlib.sha256(raw_b).hexdigest()}")
if different or raw_a != raw_b:
    print("result=DIFFERENT")
    raise SystemExit(1)
print("result=IDENTICAL including split first-frame seq2+seq3")
