#!/usr/bin/env python3
"""Validate and summarize the LG110 UNORM vertex-format evidence logs."""
import argparse
import hashlib
import json
import re
from pathlib import Path

FORMATS = {
    "rg8unorm": ("VK_FORMAT_R8G8_UNORM", 16, True),
    "rgba8unorm": ("VK_FORMAT_R8G8B8A8_UNORM", 37, True),
    "rg16unorm": ("VK_FORMAT_R16G16_UNORM", 77, True),
    "rgba16unorm": ("VK_FORMAT_R16G16B16A16_UNORM", 91, True),
}
A_RAW = 0x000005F22C00000000
A_WORD3 = 0x0005F22C


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pixels(log, pixel_prefix):
    found = re.findall(r"readback frame=(\d+) actual_bytes=([0-9a-f]{8}) "
                       r"pixels_valid=1 sentinel_unchanged=0", log)
    assert [int(frame) for frame, _ in found] == [0, 1, 2]
    values = [value for _, value in found]
    for frame, expected in enumerate(values):
        actual = Path(f"{pixel_prefix}.frame{frame}").read_bytes().hex()
        assert actual == expected, (pixel_prefix, frame, actual, expected)
    return values


def descriptors(log):
    lines = log.splitlines()
    pending = []
    pairs = []
    for index, line in enumerate(lines):
        match = re.search(r"VBO_FORMAT_META index=1 raw=0x([0-9a-f]+)", line)
        if match:
            pending.append(int(match.group(1), 16))
        if "VBO_DESC index=2 stride=16 " in line:
            assert pending, "descriptor without preceding raw metadata"
            words = re.findall(r"0x([0-9a-f]{8})", lines[index + 1])[1:]
            assert len(words) == 4, lines[index + 1]
            values = [int(word, 16) for word in words]
            pairs.append((pending.pop(0), values))
    assert not pending
    assert len(pairs) == 11, len(pairs)
    for raw, words in pairs:
        assert words[:3] == [0x022C4000, 0x00100001, 0x00000040]
        assert words[3] & 0x7FFFF == (raw >> 32) & 0x7FFFF
    variants = {(raw, words[3]) for raw, words in pairs if raw != A_RAW}
    assert len(variants) == 1, variants
    raw, word3 = variants.pop()
    return raw, word3


def subfields(word3):
    return {
        "bits_2_0": word3 & 7,
        "bits_5_3": (word3 >> 3) & 7,
        "bits_8_6": (word3 >> 6) & 7,
        "bits_11_9": (word3 >> 9) & 7,
        "bits_14_12": (word3 >> 12) & 7,
        "bits_18_15": (word3 >> 15) & 15,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--vkgl", type=Path, required=True)
    args = parser.parse_args()
    result = {
        "schema": 1,
        "date": "2026-11-10",
        "driver_sha256": "aff7101ab877db0026fa0f8274e03a579f1b4fd2c16fabe2478d08f224d0da0a",
        "vkgl_candidate_sha256": sha256(args.vkgl),
        "candidate_installed": False,
        "test_binary_sha256": sha256(args.binary),
        "mapping": "word3[18:0] = raw_metadata[50:32]",
        "uv_entries_per_gdb_run": 11,
        "all_entries_mapping_valid": True,
        "validation_clean_claimed": False,
        "variants": [],
    }
    for name, (vk_format, number, distinct) in FORMATS.items():
        plain_path = args.evidence / f"{name}-plain.log"
        gdb_path = args.evidence / f"{name}-gdb.log"
        plain = plain_path.read_text()
        gdb = gdb_path.read_text()
        assert f"pair_uv_format={name} vk_format={number}" in plain
        assert "attribute_sensitive pattern_valid=1" in plain
        assert "PASS vktest-vertex-state" in plain
        raw, word3 = descriptors(gdb)
        plain_pixels = pixels(plain, args.evidence / f"{name}-plain.bin")
        assert (plain_pixels[0] == plain_pixels[2])
        assert (plain_pixels[0] != plain_pixels[1]) == distinct
        numeric_path = args.evidence / f"{name}-numeric.log"
        numeric = numeric_path.read_text()
        assert "pair=numeric expected_pattern=A-A-A" in numeric
        assert "attribute_sensitive pattern_valid=1" in numeric
        numeric_pixels = pixels(numeric, args.evidence / f"{name}-numeric.bin")
        assert numeric_pixels == ["004030ff"] * 3
        entry = {
            "name": name,
            "vk_format": vk_format,
            "vk_format_number": number,
            "raw_metadata": f"0x{raw:016x}",
            "word3": f"0x{word3:08x}",
            "plain_pixel_hex": plain_pixels,
            "word3_subfields": subfields(word3),
            "plain_log_sha256": sha256(plain_path),
            "gdb_log_sha256": sha256(gdb_path),
            "numeric_equivalence": {
                "samples_8bit": [[0, 0], [128, 64], [64, 192], [26, 230]],
                "pixel_hex": numeric_pixels,
                "log_sha256": sha256(numeric_path),
            },
            "causal_mutation": None,
        }
        if distinct:
            mutation_path = args.evidence / f"{name}-mutated.log"
            mutation = mutation_path.read_text()
            assert "MUTATIONS=1 UNEXPECTED=0" in mutation
            assert "UNEXPECTED_DESCRIPTOR" not in mutation
            assert "attribute_sensitive pattern_valid=1" in mutation
            assert re.search(rf"MUTATE_UV_FORMAT off=0x8f90 old=0x{word3:x} "
                             r"new=0x5f22c", mutation)
            mutated_pixels = pixels(mutation, args.evidence / f"{name}-mutated.bin")
            assert mutated_pixels == [plain_pixels[0]] * 3
            entry["causal_mutation"] = {
                "source_word3": f"0x{word3:08x}",
                "destination_word3": f"0x{A_WORD3:08x}",
                "mutations": 1,
                "unexpected": 0,
                "mutated_pixel_hex": mutated_pixels,
                "log_sha256": sha256(mutation_path),
            }
        if name == "rgba16unorm":
            baseline_path = args.evidence / f"{name}-numeric-baseline.log"
            baseline = baseline_path.read_text()
            baseline_pixels = pixels(baseline, args.evidence / f"{name}-numeric-baseline.bin")
            assert baseline_pixels == ["004030ff", "000000ff", "004030ff"]
            assert "attribute_sensitive pattern_valid=0" in baseline
            entry["baseline_numeric_failure"] = {
                "pixel_hex": baseline_pixels,
                "log_sha256": sha256(baseline_path),
            }
        result["variants"].append(entry)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
