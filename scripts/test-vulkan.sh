#!/usr/bin/env bash
# Smoke-test the vkgl ICD inside the current Wayland session.
#   ./scripts/test-vulkan.sh [--soak]
set -uo pipefail
pass=0; fail=0
ok()   { echo "  ✅ $*"; pass=$((pass+1)); }
bad()  { echo "  ❌ $*"; fail=$((fail+1)); }

echo "== environment"
: "${XDG_RUNTIME_DIR:=/run/user/$(id -u)}"; export XDG_RUNTIME_DIR
[[ -n "${WAYLAND_DISPLAY:-}" ]] || export WAYLAND_DISPLAY=wayland-0

echo "== 1. device enumeration"
if vulkaninfo --summary 2>/dev/null | grep -q "LoongGPU LG110 (vkgl"; then
  ok "vulkaninfo sees LoongGPU LG110 (vkgl GLES2)"
else
  bad "vulkaninfo does not list the vkgl device"
fi

echo "== 2. vkcube renders"
if timeout 20 vkcube --c 60 >/tmp/vkgl-vkcube.log 2>&1; then
  ok "vkcube completed 60 frames"
else
  bad "vkcube failed: $(tail -2 /tmp/vkgl-vkcube.log | tr '\n' ' ')"
fi

echo "== 3. vkmark scenes"
if command -v vkmark >/dev/null; then
  out=$(timeout 120 vkmark --winsys wayland -p fifo -s 640x480 \
        -b vertex:duration=2 -b texture:duration=2 -b shading:duration=2 \
        -b effect2d:duration=2 -b desktop:duration=2 2>&1)
  n=$(grep -c "FPS:" <<<"$out")
  if [[ $n -ge 5 ]]; then ok "vkmark: $n/5 scenes rendered"; else bad "vkmark: only $n/5 scenes"; fi
  grep "FPS:" <<<"$out" | sed 's/^/     /'
else
  echo "  .. vkmark not installed, skipped"
fi

echo "== 4. validation layers clean"
if VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation timeout 30 vkcube --c 10 >/tmp/vkgl-val.log 2>&1; then
  n=$(grep -c "Validation Error" /tmp/vkgl-val.log)
  if [[ $n -eq 0 ]]; then ok "0 validation errors"; else bad "$n validation errors"; fi
else
  bad "vkcube crashed under validation layers"
fi

echo "== 5. no llvmpipe in the渲染 process"
timeout 15 vkcube --c 2000 >/dev/null 2>&1 & pid=$!
sleep 3
if pid=$(pgrep -x vkcube | head -1); then
  if grep -q llvmpipe "/proc/$pid/maps" 2>/dev/null; then bad "llvmpipe mapped"; else ok "GPU-only (no llvmpipe)"; fi
else
  bad "vkcube process not found"
fi
kill $pid 2>/dev/null || true

echo
echo "== result: $pass passed, $fail failed"
exit $(( fail > 0 ))
