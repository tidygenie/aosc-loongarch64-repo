#!/usr/bin/env bash
# Initialize the candidate without changing the syscall table, then unload it.
set -euo pipefail

candidate=${1:-}
if [[ -z $candidate ]]; then
  echo "usage: $0 /absolute/path/to/la_ow_syscall.ko" >&2
  exit 2
fi
candidate=$(readlink -f -- "$candidate")
[[ -f $candidate ]] || { echo "candidate not found: $candidate" >&2; exit 2; }

kernel=$(uname -r)
[[ $kernel == *clang-lto ]] || {
  echo "refusing discovery test on non-clang-lto kernel: $kernel" >&2
  exit 2
}
if grep -q '^la_ow_syscall ' /proc/modules; then
  echo "refusing to replace an already loaded la_ow_syscall module" >&2
  exit 2
fi
modinfo -F vermagic "$candidate" | grep -q "^$kernel " || {
  echo "candidate vermagic does not match $kernel" >&2
  exit 2
}
modinfo -F parm "$candidate" | grep -q '^discovery_only:' || {
  echo "candidate lacks the discovery_only safety parameter" >&2
  exit 2
}

stamp=$(date +%Y%m%d-%H%M%S)
out=${LIBLOL_EVIDENCE_DIR:-/home/houge/infra/gpu/evidence/liblol-discovery-$stamp}
mkdir -p -- "$out"
sha256sum "$candidate" | tee "$out/candidate.sha256"
modinfo "$candidate" > "$out/modinfo.txt"
uname -a > "$out/uname.txt"
sudo -n dmesg > "$out/dmesg-before.log"
loaded=0
cleanup() {
  if ((loaded)); then
    sudo -n rmmod la_ow_syscall || true
  fi
}
trap cleanup EXIT

sudo -n insmod "$candidate" discovery_only=1
loaded=1
grep '^la_ow_syscall ' /proc/modules | tee "$out/module-loaded.txt"
sudo -n dmesg > "$out/dmesg-loaded.log"
diff -u "$out/dmesg-before.log" "$out/dmesg-loaded.log" > "$out/dmesg-load.diff" || true
grep -q 'la_ow_syscall discovery-only initialization succeeded; syscall table unchanged' \
  "$out/dmesg-load.diff"
if grep -q 'la_ow_syscall module successfully initialized' "$out/dmesg-load.diff"; then
  echo "normal syscall-table initialization unexpectedly ran" >&2
  exit 1
fi
python3 -c 'import os; print("native_during_discovery", os.getpid())' | tee "$out/native-loaded.txt"

sudo -n rmmod la_ow_syscall
loaded=0
if grep -q '^la_ow_syscall ' /proc/modules; then
  echo "candidate remained loaded after rmmod" >&2
  exit 1
fi
sudo -n dmesg > "$out/dmesg-after.log"
diff -u "$out/dmesg-loaded.log" "$out/dmesg-after.log" > "$out/dmesg-unload.diff" || true
python3 -c 'print("native_after_unload=PASS")' | tee "$out/native-after.txt"

libc=${LIBLOL_OLDWORLD_LIBC:-/opt/lol/support/lib/loongarch64-aosc-linux-gnuow/libc.so.6}
loader=${LIBLOL_OLDWORLD_LOADER:-/usr/lib/ld.so.1}
if [[ -x $loader && -f $libc ]]; then
  "$loader" --library-path "$(dirname "$libc")" "$libc" > "$out/oldworld-after.log" 2>&1
  grep -q 'LibLoL' "$out/oldworld-after.log"
fi

echo "LIBLOL_DISCOVERY_ONLY=PASS evidence=$out"
