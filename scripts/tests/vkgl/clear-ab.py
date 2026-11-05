#!/usr/bin/env python3
"""Alternating paired vkmark benchmark for two vkgl ICD manifests."""
import argparse
import json
import os
import re
import subprocess
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--base", required=True, help="ICD JSON for the control")
parser.add_argument("--candidate", required=True, help="ICD JSON for the candidate")
parser.add_argument("--rounds", type=int, default=6)
parser.add_argument("--duration", type=int, default=5)
parser.add_argument("--size", default="640x480", help="vkmark window size, for example 1920x1080")
parser.add_argument("--output", type=Path)
args = parser.parse_args()
if args.rounds < 1 or args.duration < 1:
    parser.error("rounds and duration must be positive")
if not re.fullmatch(r"[1-9][0-9]{0,3}x[1-9][0-9]{0,3}", args.size):
    parser.error("size must be WIDTHxHEIGHT")

rows = []
for round_no in range(1, args.rounds + 1):
    for scene in ("vertex", "texture", "shading"):
        order = ("base", "candidate") if round_no % 2 else ("candidate", "base")
        for variant in order:
            env = os.environ.copy()
            env.update({
                "XDG_RUNTIME_DIR": f"/run/user/{os.getuid()}",
                "WAYLAND_DISPLAY": "wayland-0",
                "VK_ICD_FILENAMES": args.base if variant == "base" else args.candidate,
            })
            env.pop("VKGL_DEBUG", None)
            env.pop("VKGL_PROF", None)
            command = [
                "vkmark", "--winsys", "wayland", "-p", "fifo", "-s", args.size,
                "-b", f"{scene}:duration={args.duration}",
            ]
            started = time.monotonic()
            result = subprocess.run(
                command, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, timeout=args.duration + 20, check=False,
            )
            match = re.search(r"FPS:\s*([0-9]+)\s+FrameTime:\s*([0-9.]+)\s*ms", result.stdout)
            row = {
                "round": round_no, "scene": scene, "variant": variant,
                "rc": result.returncode, "elapsed_s": round(time.monotonic() - started, 3),
                "fps": int(match.group(1)) if match else None,
                "frame_ms": float(match.group(2)) if match else None,
            }
            rows.append(row)
            print(json.dumps(row), flush=True)
            if result.returncode or not match:
                raise SystemExit(f"benchmark failed; output follows:\n{result.stdout}")

if args.output:
    args.output.write_text("".join(json.dumps(row) + "\n" for row in rows))
