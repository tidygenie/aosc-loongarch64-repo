#!/usr/bin/env python3
"""Validate and summarize paired vkmark JSONL produced by clear-ab.py."""
import argparse
import json
import statistics
from collections import defaultdict
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("jsonl", type=Path)
args = parser.parse_args()

rows = [json.loads(line) for line in args.jsonl.read_text().splitlines() if line.strip()]
if not rows:
    parser.error("empty input")
for row in rows:
    if row.get("rc") != 0 or not isinstance(row.get("fps"), int):
        parser.error("all runs must have rc=0 and an integer FPS result")

data = defaultdict(lambda: defaultdict(list))
rounds = set()
for row in rows:
    data[row["scene"]][row["variant"]].append(row["fps"])
    rounds.add((row["scene"], row["round"]))
for key in rounds:
    if sum(1 for row in rows if (row["scene"], row["round"]) == key) != 2:
        parser.error(f"incomplete pair: {key}")

print(f"runs={len(rows)} pairs={len(rounds)}")
for scene, variants in data.items():
    base = variants["base"]
    candidate = variants["candidate"]
    if len(base) != len(candidate):
        parser.error(f"unpaired sample counts for {scene}")
    base_median = statistics.median(base)
    candidate_median = statistics.median(candidate)
    change = (candidate_median / base_median - 1) * 100
    wins = sum(c > b for b, c in zip(base, candidate))
    ties = sum(c == b for b, c in zip(base, candidate))
    print(
        f"{scene}: median_fps={base_median:g}->{candidate_median:g} "
        f"change={change:+.2f}% wins={wins}/{len(base)} ties={ties}"
    )
