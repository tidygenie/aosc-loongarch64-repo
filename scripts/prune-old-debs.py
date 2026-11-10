#!/usr/bin/env python3
"""Plan and apply bounded deb retention without wildcard or recursive deletion."""

import argparse
import functools
import hashlib
import json
from pathlib import Path
import subprocess

FIELDS = ("Package", "Version", "Architecture", "Filename", "SHA256")


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_packages(path: Path) -> list[dict[str, str]]:
    rows = []
    current: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines() + [""]:
        if not line.strip():
            if current:
                missing = [field for field in FIELDS if field not in current]
                if missing:
                    raise ValueError(f"incomplete package stanza: {missing}")
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
    return rows


def compare_versions(left: str, right: str) -> int:
    if left == right:
        return 0
    result = subprocess.run(["dpkg", "--compare-versions", left, "gt", right], check=False)
    if result.returncode not in (0, 1):
        raise ValueError(f"cannot compare {left!r} and {right!r}")
    return -1 if result.returncode == 0 else 1


def entry(root: Path, row: dict[str, str]) -> dict[str, object]:
    path = root / row["Filename"]
    if not path.is_file() or path.is_symlink():
        raise ValueError(f"indexed deb is not a regular non-symlink file: {path}")
    return {
        "path": row["Filename"],
        "package": row["Package"],
        "version": row["Version"],
        "architecture": row["Architecture"],
        "sha256": row["SHA256"],
        "size": path.stat().st_size,
    }


def make_plan(packages: Path, repo: Path, keep_old: int, prune_attic: bool) -> dict[str, object]:
    root = repo.resolve(strict=True)
    groups: dict[tuple[str, str], list[dict[str, str]]] = {}
    for row in read_packages(packages):
        groups.setdefault((row["Package"], row["Architecture"]), []).append(row)
    keep = []
    delete = []
    for group in groups.values():
        ordered = sorted(group, key=functools.cmp_to_key(lambda a, b: compare_versions(a["Version"], b["Version"])))
        keep.extend(entry(root, row) for row in ordered[: keep_old + 1])
        delete.extend(entry(root, row) for row in ordered[keep_old + 1 :])
    attic = []
    if prune_attic:
        attic_root = root / "pool/.attic"
        if attic_root.is_dir():
            for path in sorted(attic_root.rglob("*.deb")):
                if not path.is_file() or path.is_symlink():
                    raise ValueError(f"attic deb is not a regular non-symlink file: {path}")
                attic.append({"path": str(path.relative_to(root)), "sha256": sha256(path), "size": path.stat().st_size})
    return {
        "format": 1,
        "packages_sha256": sha256(packages),
        "keep_old": keep_old,
        "keep": sorted(keep, key=lambda row: row["path"]),
        "delete": sorted(delete, key=lambda row: row["path"]),
        "attic_delete": attic,
    }


def checked_path(root: Path, item: dict[str, object], area: str) -> Path:
    relative = Path(str(item["path"]))
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError(f"unsafe path in plan: {relative}")
    expected_parent = (root / area).resolve(strict=True)
    path = (root / relative).resolve(strict=True)
    if path == expected_parent or expected_parent not in path.parents:
        raise ValueError(f"path escapes {area}: {path}")
    if path.suffix != ".deb" or path.is_symlink() or not path.is_file():
        raise ValueError(f"not a regular non-symlink deb: {path}")
    return path


def verify_item(root: Path, item: dict[str, object], area: str) -> Path:
    path = checked_path(root, item, area)
    actual = sha256(path)
    if actual != item["sha256"]:
        raise ValueError(f"SHA256 changed since planning: {path}: {actual}")
    if path.stat().st_size != item["size"]:
        raise ValueError(f"size changed since planning: {path}")
    return path


def apply_plan(plan: dict[str, object], repo: Path) -> None:
    if plan.get("format") != 1 or plan.get("keep_old") != 1:
        raise ValueError("unsupported plan or retention policy")
    root = repo.resolve(strict=True)
    keep = plan["keep"]
    delete = plan["delete"]
    attic = plan["attic_delete"]
    if not isinstance(keep, list) or not isinstance(delete, list) or not isinstance(attic, list):
        raise ValueError("invalid plan arrays")
    for item in keep:
        print(f"KEEP_VERIFIED {verify_item(root, item, 'pool/main')}")
    targets = [(item, "pool/main") for item in delete] + [(item, "pool/.attic") for item in attic]
    resolved = [(verify_item(root, item, area), item) for item, area in targets]
    for path, item in resolved:
        print(f"DELETE_VERIFIED {path} sha256={item['sha256']}")
    for path, _ in resolved:
        path.unlink()
    for item in keep:
        verify_item(root, item, "pool/main")
    print(f"RETENTION_APPLIED kept={len(keep)} deleted_indexed={len(delete)} deleted_attic={len(attic)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("packages", nargs="?", type=Path)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--keep-old", type=int, default=1)
    parser.add_argument("--prune-attic", action="store_true")
    parser.add_argument("--plan-out", type=Path)
    parser.add_argument("--apply", type=Path, metavar="PLAN")
    args = parser.parse_args()
    if args.apply:
        if args.packages or args.plan_out or args.prune_attic or args.keep_old != 1:
            parser.error("--apply cannot be combined with planning options")
        apply_plan(json.loads(args.apply.read_text(encoding="utf-8")), args.repo)
        return 0
    if not args.packages or args.keep_old != 1:
        parser.error("planning requires Packages and exactly --keep-old=1")
    plan = make_plan(args.packages, args.repo, args.keep_old, args.prune_attic)
    payload = json.dumps(plan, indent=2, sort_keys=True) + "\n"
    if args.plan_out:
        args.plan_out.write_text(payload, encoding="utf-8")
    reclaimed = sum(int(row["size"]) for row in plan["delete"] + plan["attic_delete"])
    print(
        f"RETENTION_PLAN kept={len(plan['keep'])} delete_indexed={len(plan['delete'])} "
        f"delete_attic={len(plan['attic_delete'])} reclaim_bytes={reclaimed}"
    )
    for row in plan["delete"] + plan["attic_delete"]:
        print(f"PLAN_DELETE {row['path']} sha256={row['sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
