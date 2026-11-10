#!/usr/bin/env bash
# Upgrade one Homebrew prefix only at a queue and process boundary.
set -euo pipefail

PREFIX=${1:-}
TAG=${2:-}
PATCH=${3:-}
[[ -x ${PREFIX}/bin/brew && -d ${PREFIX}/.git && -f ${PATCH} ]] || {
  echo "usage: $0 /absolute/brew-prefix tag /absolute/loongarch.patch" >&2
  exit 2
}
PREFIX=$(readlink -f -- "$PREFIX")
PATCH=$(readlink -f -- "$PATCH")
QUEUE_LOCK=/home/houge/infra/ci/state/brew-queue.lock
mkdir -p /home/houge/infra/ci/state/homebrew-upgrades
exec 9>"$QUEUE_LOCK"
echo "Waiting for shared queue lock before upgrading $PREFIX"
flock 9

python3 - "$PREFIX" <<'PY'
import pathlib, sys, time
prefix = sys.argv[1]
target = f"{prefix}/Library/Homebrew/brew.rb".encode()
deadline = time.monotonic() + 900
while True:
    running = []
    for entry in pathlib.Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            argv = (entry / 'cmdline').read_bytes().split(b'\0')
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        if target in argv:
            running.append(int(entry.name))
    if not running:
        print(f"SAFE_HOMEBREW_BOUNDARY prefix={prefix}")
        break
    if time.monotonic() >= deadline:
        raise SystemExit(f"timed out waiting for Homebrew PIDs: {running}")
    time.sleep(1)
PY

repo=$PREFIX
old_head=$(git -C "$repo" rev-parse HEAD)
stamp=$(date +%Y%m%d-%H%M%S)
backup=/home/houge/infra/ci/state/homebrew-upgrades/$(basename "$PREFIX")-$stamp.diff
git -C "$repo" diff > "$backup"
stashed=0
updated=0
rollback() {
  rc=$?
  if ((updated)); then
    git -C "$repo" checkout --detach --force "$old_head" || true
  fi
  if ((stashed)); then
    git -C "$repo" stash pop || true
  fi
  exit "$rc"
}
trap rollback ERR

if ! git -C "$repo" diff --quiet || ! git -C "$repo" diff --cached --quiet; then
  git -C "$repo" stash push -m "pre-$TAG LoongArch patches"
  stashed=1
fi
if ! git -C "$repo" rev-parse --verify "refs/tags/$TAG^{commit}" >/dev/null 2>&1; then
  git -C "$repo" fetch --depth=1 origin tag "$TAG" --no-tags
fi
git -C "$repo" checkout --detach "refs/tags/$TAG"
updated=1
git -C "$repo" apply --check "$PATCH"
git -C "$repo" apply "$PATCH"
git -C "$repo" diff --check

export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_FROM_API=1 HOMEBREW_NO_ANALYTICS=1
version_output=$($PREFIX/bin/brew --version)
version=${version_output%%$'\n'*}
[[ $version == "Homebrew $TAG-dirty" ]]
$PREFIX/bin/brew --config > "$backup.config"
! grep -q 'dunno' "$backup.config"
if [[ -f /home/houge/infra/brew-tap/Formula/b2sum.rb ]]; then
  $PREFIX/bin/brew ruby -e 'require "formulary"; require "simulate_system"; f=Formulary::FromPathLoader.new("/home/houge/infra/brew-tap/Formula/b2sum.rb").get_formula(:stable); abort unless f.name=="b2sum" && Homebrew::SimulateSystem.current_tag.to_sym==:loongarch64_linux; puts "HOMEBREW_UPGRADE_FORMULA=PASS #{f.version}"'
fi
$PREFIX/bin/brew info --json=v2 --installed > "$backup.installed.json"
python3 - "$backup.installed.json" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    data = json.load(f)
assert isinstance(data.get('formulae'), list)
print(f"HOMEBREW_UPGRADE_INVENTORY=PASS count={len(data['formulae'])}")
PY

trap - ERR
printf 'HOMEBREW_UPGRADE=PASS prefix=%s tag=%s old=%s new=%s backup=%s\n' \
  "$PREFIX" "$TAG" "$old_head" "$(git -C "$repo" rev-parse HEAD)" "$backup"
