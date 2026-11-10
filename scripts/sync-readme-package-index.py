#!/usr/bin/env python3
"""List only the latest indexed deb per package/architecture in README."""

import argparse
import hashlib
import subprocess
from pathlib import Path
from urllib.parse import quote

BEGIN = "<!-- BEGIN GENERATED DEB PACKAGE INDEX -->"
END = "<!-- END GENERATED DEB PACKAGE INDEX -->"
BASE_URL = "https://homebrewloongarch64.site/aosc/"
FIELDS = ("Package", "Version", "Architecture", "Filename", "SHA256")


def read_packages(path: Path) -> list[dict[str, str]]:
    rows = []
    current: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines() + [""]:
        if not line.strip():
            if current:
                missing = [field for field in FIELDS if field not in current]
                if missing:
                    raise ValueError(f"incomplete package stanza: missing {missing}")
                rows.append({field: current[field] for field in FIELDS})
                current = {}
            continue
        if line[0].isspace():
            continue
        key, separator, value = line.partition(":")
        if separator and key in FIELDS:
            current[key] = value.lstrip()
    if not rows:
        raise ValueError("empty Packages index")
    identities = [(row["Package"], row["Version"], row["Architecture"], row["Filename"]) for row in rows]
    if len(set(identities)) != len(identities):
        raise ValueError("duplicate package identity in Packages index")
    return sorted(rows, key=lambda row: (row["Package"], row["Version"], row["Architecture"], row["Filename"]))


def version_greater(candidate: str, current: str) -> bool:
    result = subprocess.run(
        ["dpkg", "--compare-versions", candidate, "gt", current],
        check=False,
    )
    if result.returncode not in (0, 1):
        raise ValueError(f"cannot compare deb versions {candidate!r} and {current!r}")
    return result.returncode == 0


def latest_per_package(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    latest: dict[tuple[str, str], dict[str, str]] = {}
    for row in rows:
        key = (row["Package"], row["Architecture"])
        current = latest.get(key)
        if current is None or version_greater(row["Version"], current["Version"]):
            latest[key] = row
    return sorted(latest.values(), key=lambda row: (row["Package"], row["Architecture"]))


def verify_pool(rows: list[dict[str, str]], pool_root: Path) -> None:
    for row in rows:
        payload = pool_root / row["Filename"]
        if not payload.is_file():
            raise FileNotFoundError(payload)
        with payload.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if digest != row["SHA256"]:
            raise ValueError(f"SHA256 mismatch for {payload}: {digest} != {row['SHA256']}")


def render(rows: list[dict[str, str]], indexed_total: int) -> str:
    names = len({row["Package"] for row in rows})
    lines = [
        BEGIN,
        "",
        "以下清单由 `scripts/sync-readme-package-index.py` 从实际发布的",
        "`dists/stable/main/binary-loongarch64/Packages` 生成，**只列每个包的最新版本**。",
        f"当前列出 **{len(rows)} 个最新 deb 条目、{names} 个不同包名**；索引共保留 {indexed_total} 个 deb。",
        "旧版本只保留有限的回滚副本，不在 README 罗列。APT 默认选择可安装的最新版本。",
        "`mesa-vulkan-loonggpu` 仍只是已废弃的 RADV 存档，不代表 LG110 可用原生 Vulkan。",
        "",
        "| 软件包 | 版本 | 架构 | SHA-256 | deb |",
        "|---|---|---|---|---|",
    ]
    for row in rows:
        url = BASE_URL + quote(row["Filename"], safe="/")
        lines.append(
            f"| `{row['Package']}` | `{row['Version']}` | `{row['Architecture']}` | "
            f"`{row['SHA256']}` | [下载]({url}) |"
        )
    lines.extend(["", END])
    return "\n".join(lines)


def replace_section(readme: str, section: str) -> str:
    start = readme.find(BEGIN)
    end = readme.find(END)
    if start < 0 or end < 0 or end < start:
        raise ValueError("README must contain ordered generated-package-index markers")
    end += len(END)
    return readme[:start] + section + readme[end:]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("packages", type=Path)
    parser.add_argument("readme", type=Path)
    parser.add_argument("--pool-root", type=Path, help="also verify every indexed local deb SHA256")
    parser.add_argument("--check", action="store_true", help="fail instead of writing when README differs")
    args = parser.parse_args()

    all_rows = read_packages(args.packages)
    if args.pool_root:
        verify_pool(all_rows, args.pool_root)
    rows = latest_per_package(all_rows)
    updated = replace_section(args.readme.read_text(encoding="utf-8"), render(rows, len(all_rows)))
    original = args.readme.read_text(encoding="utf-8")
    if args.check:
        if updated != original:
            raise SystemExit("README deb package index is stale; rerun without --check")
        status = "ok"
    else:
        if updated != original:
            args.readme.write_text(updated, encoding="utf-8")
            status = "updated"
        else:
            status = "unchanged"
    print(
        f"DEB_INDEX entries={len(rows)} names={len({row['Package'] for row in rows})} "
        f"indexed={len(all_rows)} status={status}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
