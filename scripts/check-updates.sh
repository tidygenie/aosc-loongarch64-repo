#!/usr/bin/env bash
# check-deb-updates.sh —— 检查第三方源里的 deb 包是否有上游新版本
#
# 输出：需要更新的包清单（写入 state/deb-updates.txt）
# 上游来源：
#   - GitHub releases（spec 的 CHKUPDATE=github::repo=owner/name）
#   - ruby-lang 目录列表（spec 里 CHKUPDATE=ruby）
set -uo pipefail
CI="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INFRA="$(dirname "$CI")"
OUT="$CI/state/deb-updates.txt"
: > "$OUT"

TOKEN_FILE="$INFRA/.secrets/github-token"
AUTH=()
[[ -f "$TOKEN_FILE" ]] && AUTH=(-H "Authorization: Bearer $(cat "$TOKEN_FILE")")

latest_ruby() {
  curl -fsSL --max-time 20 https://cache.ruby-lang.org/pub/ruby/4.0/ 2>/dev/null \
    | grep -oE "ruby-4\.0\.[0-9]+\.tar\.gz" | sed 's/ruby-//;s/\.tar\.gz//' | sort -uV | tail -1
}

latest_github() {
  local repo="$1"
  curl -fsSL --max-time 20 "${AUTH[@]}" "https://api.github.com/repos/$repo/releases/latest" 2>/dev/null \
    | python3 -c "import json,sys;print(json.load(sys.stdin).get('tag_name','').lstrip('v'))" 2>/dev/null
}

updated=0
for d in "$INFRA"/pkg/*/; do
  [[ -f "$d/spec" ]] || continue
  name="$(basename "$d")"
  ver="$(grep -oP '^VER=\K.*' "$d/spec" 2>/dev/null)"
  upd="$(grep -oP '^CHKUPDATE="?\K[^"]+' "$d/spec" 2>/dev/null)"
  [[ -n "$ver" ]] || continue

  latest=""
  case "$upd" in
    github::repo=*) latest="$(latest_github "${upd#github::repo=}")" ;;
    ruby*|anitya*)  latest="$(latest_ruby)" ;;
  esac

  if [[ -z "$latest" ]]; then
    echo "??  $name: 当前 $ver（无法获取上游版本）" >> "$OUT"
    continue
  fi

  if [[ "$latest" != "$ver" ]]; then
    echo "⬆   $name: $ver -> $latest" >> "$OUT"
    updated=$((updated+1))
  else
    echo "✓   $name: $ver（最新）" >> "$OUT"
  fi
done

echo "需要更新: $updated 个"
cat "$OUT"
