#!/usr/bin/env python3
import sys
from PIL import Image

if len(sys.argv) != 2:
    raise SystemExit(f"usage: {sys.argv[0]} SCREENSHOT.png")

image = Image.open(sys.argv[1]).convert("RGB")
red = blue = 0
for r, g, b in image.get_flattened_data():
    red += r > 200 and g < 60 and b < 60
    blue += b > 200 and r < 60 and g < 60
print(f"red={red} blue={blue}")
if red != 153600 or blue != 153600:
    raise SystemExit("FAIL: expected 153600 red and 153600 blue pixels")
print("PASS: copied linear texture and padded rows are correct")
