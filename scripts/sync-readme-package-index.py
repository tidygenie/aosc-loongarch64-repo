#!/usr/bin/env python3
"""Synchronize README's complete deb inventory from the actual APT Packages index."""

import argparse
import hashlib
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


def verify_pool(rows: list[dict[str, str]], pool_root: Path) -> None:
    for row in rows:
        payload = pool_root / row["Filename"]
        if not payload.is_file():
            raise FileNotFoundError(payload)
        with payload.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if digest != row["SHA256"]:
            raise ValueError(f"SHA256 mismatch for {payload}: {digest} != {row['SHA256']}")


def render(rows: list[dict[str, str]]) -> str:
    names = len({row["Package"] for row in rows})
    lines = [
        BEGIN,
        "",
        "以下清单由 `scripts/sync-readme-package-index.py` 从实际发布的",
        "`dists/stable/main/binary-loongarch64/Packages` 生成，**不能只列最新版或手工挑选**。",
        f"当前共 **{len(rows)} 个 deb 文件条目、{names} 个不同包名**。仓库保留已发布的多个历史版本；",
        "APT 默认选择可安装的最新版本，表中历史条目不表示它们都是当前推荐版本。",
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

    rows = read_packages(args.packages)
    if args.pool_root:
        verify_pool(rows, args.pool_root)
    updated = replace_section(args.readme.read_text(encoding="utf-8"), render(rows))
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
    print(f"DEB_INDEX entries={len(rows)} names={len({row['Package'] for row in rows})} status={status}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
