#!/usr/bin/env bash
# Verify a freshly installed mainline-16k-rt kernel: ABI, RT, 16K pages,
# compiler used, DKMS loonggpu, and the graphical stack (Wayland + Vulkan).
set -uo pipefail
pass=0; fail=0
ok()  { echo "  ✅ $*"; pass=$((pass+1)); }
bad() { echo "  ❌ $*"; fail=$((fail+1)); }

echo "== 1. running kernel"
REL=$(uname -r); echo "  uname -r: $REL"
case "$REL" in
  *-mainline-16k-rt*) ok "mainline 16K RT kernel" ;;
  *) bad "unexpected kernel release" ;;
esac
if grep -q 'PREEMPT_RT' /proc/version; then ok "PREEMPT_RT"; else bad "not PREEMPT_RT"; fi
if [[ "$(getconf PAGESIZE)" == 16384 ]]; then ok "16K pages"; else bad "page size $(getconf PAGESIZE)"; fi
echo "  /proc/version: $(cut -c1-160 /proc/version)"

case "$REL" in
  *gcc-o3)  grep -qE "gcc \(GCC\)|gcc version" /proc/version && ok "built with gcc -O3" || bad "gcc build expected" ;;
  *clang-lto) grep -qE "clang version|Clang" /proc/version && ok "built with clang (full LTO)" || bad "clang build expected" ;;
esac

echo "== 2. modules / DKMS"
if lsmod | grep -q '^loonggpu'; then ok "loonggpu loaded"; else bad "loonggpu not loaded"; fi
if dkms status 2>/dev/null | grep -F "$REL" | grep -q installed; then ok "DKMS entry for $REL"; else bad "no DKMS entry for $REL"; fi

echo "== 3. graphics"
if command -v qdbus >/dev/null && [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
  r=$(qdbus org.kde.KWin /KWin supportInformation 2>/dev/null | grep -m1 "OpenGL renderer")
  case "$r" in
    *LG110*) ok "$r" ;;
    *) bad "KWin renderer: ${r:-unavailable}" ;;
  esac
fi
if command -v vulkaninfo >/dev/null && vulkaninfo --summary 2>/dev/null | grep -q "LG110 (vkgl"; then
  ok "Vulkan: vkgl ICD active"
else
  bad "Vulkan device not found"
fi

echo
echo "== result: $pass passed, $fail failed"
exit $(( fail > 0 ))
