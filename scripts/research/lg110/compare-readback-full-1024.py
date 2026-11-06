#!/usr/bin/env python3
"""Compare complete A/B readback GFX IBs from lg110-iotrace-1024."""
import hashlib
import re
import sys


def parse(path):
    cs = {}
    ib_bytes = {}
    dumps = {}
    with open(path, encoding="utf-8", errors="strict") as f:
        for line in f:
            m = re.search(r"LG110CS seq=(\d+).* ret=(-?\d+)", line)
            if m:
                cs[int(m.group(1))] = int(m.group(2))
            m = re.search(r"LG110IB seq=(\d+).* ib_bytes=(\d+) ip_type=(\d+)", line)
            if m and int(m.group(3)) == 0:
                ib_bytes[int(m.group(1))] = int(m.group(2))
            if "LG110IBDUMP" not in line:
                continue
            m = re.search(r"seq=(\d+).* status=(\w+).* dwords=(\d+).* data=(.*)$", line)
            if not m:
                continue
            seq = int(m.group(1))
            if seq not in ib_bytes:
                continue
            words = [int(x, 16) for x in m.group(4).split()]
            if m.group(2) != "ok" or len(words) != int(m.group(3)):
                raise ValueError(f"{path}: invalid dump at seq {seq}")
            dumps[seq] = words
    return cs, ib_bytes, dumps


a = parse(sys.argv[1])
b = parse(sys.argv[2])
for name, trace in zip(("A", "B"), (a, b)):
    cs, sizes, dumps = trace
    if len(cs) != 121 or any(cs.values()):
        raise SystemExit(f"{name}: expected 121 successful CS, got {len(cs)}")
    if sorted(dumps) != list(range(2, 122)):
        raise SystemExit(f"{name}: missing GFX dumps")

pairs = equal = different = truncated = 0
ha, hb = hashlib.sha256(), hashlib.sha256()
for seq in range(2, 122):
    wa, wb = a[2][seq], b[2][seq]
    expected = 672 if seq == 2 else 400
    if len(wa) != expected or len(wb) != expected:
        raise SystemExit(f"seq {seq}: expected {expected} dwords, got {len(wa)}/{len(wb)}")
    if a[1][seq] != expected * 4 or b[1][seq] != expected * 4:
        raise SystemExit(f"seq {seq}: inconsistent IB byte counts")
    if len(wa) * 4 < a[1][seq] or len(wb) * 4 < b[1][seq]:
        truncated += 1
    pairs += 1
    for x, y in zip(wa, wb):
        if x == y:
            equal += 1
        else:
            different += 1
    for words, h in ((wa, ha), (wb, hb)):
        h.update(b"".join(x.to_bytes(4, "little") for x in words))

print(f"complete_gfx_pairs={pairs} dwords_per_seq2=672 dwords_per_steady=400")
print(f"equal={equal} different={different} truncated={truncated}")
print(f"A_sha256={ha.hexdigest()}")
print(f"B_sha256={hb.hexdigest()}")
if different or truncated or ha.digest() != hb.digest():
    raise SystemExit(1)
print("PASS complete A/B GFX IB comparison")
