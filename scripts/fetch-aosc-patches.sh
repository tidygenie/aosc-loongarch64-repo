#!/usr/bin/env bash
# Fetch the AOSC OS kernel package (spec + config + patches) for LoongArch and
# apply the full patch set to a clean kernel tree with 5 lines of fuzz.
#
#   ./scripts/fetch-aosc-patches.sh <kernel-tree> [out-dir]
#
# Results are recorded as "applied|already|conflict" lines in <out-dir>/report.tsv.
set -euo pipefail
TREE="${1:?usage: fetch-aosc-patches.sh <kernel-tree> [out-dir]}"
OUT="${2:-/tmp/aosc-kernel}"
mkdir -p "$OUT"
ABBS="$OUT/aosc-os-abbs"

if [[ ! -d "$ABBS/runtime-kernel/linux-kernel/autobuild/patches" ]]; then
  git clone --depth 1 --filter=blob:none --sparse -b stable \
      https://github.com/AOSC-Dev/aosc-os-abbs.git "$ABBS"
  git -C "$ABBS" sparse-checkout set runtime-kernel/linux-kernel
fi
PKG="$ABBS/runtime-kernel/linux-kernel"
echo "== AOSC kernel package: $PKG"
echo "== config: $PKG/autobuild/loongarch64/config.16k"

: > "$OUT/report.tsv"
# 明确跳过的补丁（见 packages/kernel/.../autobuild/patches/SKIPPED.md）
SKIP_PATCHES="0356-RUYI-drm-amdgpu-disable-dynamic-PCIe-speed-switch-on.patch"

applied=0; already=0; conflict=0
cd "$TREE"
for p in $(ls "$PKG"/autobuild/patches/*.patch | sort); do
  name="$(basename "$p")"
  if [[ " $SKIP_PATCHES " == *" $name "* ]]; then
     echo -e "skipped\t$name" >> "$OUT/report.tsv"; continue
  fi
  if patch -p1 -N --fuzz=5 --dry-run --silent < "$p" >/dev/null 2>&1; then
     patch -p1 -N --fuzz=5 --silent < "$p" >/dev/null 2>&1 && {
        echo -e "applied\t$name" >> "$OUT/report.tsv"; applied=$((applied+1)); }
  elif patch -p1 -R --dry-run --silent < "$p" >/dev/null 2>&1; then
     echo -e "already\t$name" >> "$OUT/report.tsv"; already=$((already+1))
  else
     echo -e "conflict\t$name" >> "$OUT/report.tsv"; conflict=$((conflict+1))
  fi
done
find "$TREE" -name '*.rej' -delete
echo "applied=$applied already=$already conflict=$conflict  (see $OUT/report.tsv)"
