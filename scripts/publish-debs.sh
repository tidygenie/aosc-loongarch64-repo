#!/usr/bin/env bash
# publish-debs.sh —— dists 布局的第三方 AOSC DEB 发布器
#
# 功能：
#   1. 把 .deb 收进 pool/main/<首字母>/<包名>/（原子落盘）
#   2. 重建 dists/stable/main/binary-loongarch64/Packages(.gz)
#   3. 重新生成 Release，并用既有仓库签名身份签发 InRelease / Release.gpg
#   4. （可选 --deploy）按 pool → Packages → Release/InRelease 的顺序 rsync 到服务器，
#      使索引切换尽量原子，避免客户端抓到不一致的索引
#   5. 验证：索引内部一致性、本地 vs 服务器 md5、HTTPS 可达、隔离 apt 可见性
#
# 用法：
#   publish-debs.sh [选项] [file.deb ...]
#
# 选项：
#   --repo DIR        仓库根               （默认 ~/infra/srv/aosc）
#   --keyid FPR       签名密钥指纹         （默认读取 ~/infra/keys/keyid）
#   --gnupghome DIR   签名用 GNUPGHOME     （默认 ~/infra/gnupg）
#   --host USER@HOST  部署目标             （默认 root@124.156.133.143）
#   --remote DIR      远端仓库根           （默认 /var/www/srv/aosc）
#   --site URL        站点仓库根 URL       （默认 https://homebrewloongarch64.site/aosc）
#   --deploy          同步到服务器
#   --index-only      不接收 deb，仅重建索引与签名
#   --skip-verify     跳过最终验证阶段
#   --dry-run         只打印将要执行的动作，不改动任何内容
#   -h|--help         显示帮助
#
# 示例：
#   # 发布新构建的 deb 并部署到服务器
#   ci/publish-debs.sh --deploy ~/build/v2raya_2.5.14-1_loongarch64.deb
#   # 只重建索引（比如手动增删了 pool 里的文件之后）
#   ci/publish-debs.sh --index-only
#   # 在临时副本上演练（不影响线上）
#   ci/publish-debs.sh --repo /tmp/aosc-repo-test --dry-run /tmp/x_1.0-1_loongarch64.deb

set -euo pipefail

# ---------- 默认值 ----------
INFRA="${INFRA_ROOT:-$HOME/infra}"
REPO="$INFRA/srv/aosc"
KEYID=""
GNUPGHOME="$INFRA/gnupg"
DEPLOY_HOST="root@124.156.133.143"
REMOTE_REPO="/var/www/srv/aosc"
SITE_URL="https://homebrewloongarch64.site/aosc"
CODENAME="stable"
COMPONENT="main"
ARCH="loongarch64"
ORIGIN="AOSC Third-party"
LABEL="aosc-thirdparty"
DESC="Local AOSC third-party package repository"

DO_DEPLOY=0
INDEX_ONLY=0
SKIP_VERIFY=0
DRY_RUN=0
DEBS=()

log()  { echo "[publish] $*"; }
warn() { echo "[publish][warn] $*" >&2; }
die()  { echo "[publish][error] $*" >&2; exit 1; }
run()  { if [[ $DRY_RUN -eq 1 ]]; then echo "[dry-run] $*"; else "$@"; fi; }

usage() { sed -n '2,40p' "$0"; exit "${1:-0}"; }

# ---------- 参数解析 ----------
while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo)       REPO="$2"; shift 2;;
    --keyid)      KEYID="$2"; shift 2;;
    --gnupghome)  GNUPGHOME="$2"; shift 2;;
    --host)       DEPLOY_HOST="$2"; shift 2;;
    --remote)     REMOTE_REPO="$2"; shift 2;;
    --site)       SITE_URL="$2"; shift 2;;
    --deploy)     DO_DEPLOY=1; shift;;
    --index-only) INDEX_ONLY=1; shift;;
    --skip-verify) SKIP_VERIFY=1; shift;;
    --dry-run)    DRY_RUN=1; shift;;
    -h|--help)    usage 0;;
    -*)           die "未知选项: $1";;
    *)            DEBS+=("$1"); shift;;
  esac
done

[[ -n "$KEYID" ]] || KEYID="$(cat "$INFRA/keys/keyid" 2>/dev/null || true)"
[[ -n "$KEYID" ]] || die "未指定 --keyid 且读不到 $INFRA/keys/keyid"
export GNUPGHOME
INDEX_DIR="dists/$CODENAME/$COMPONENT/binary-$ARCH"

# ---------- 前置检查 ----------
command -v apt-ftparchive >/dev/null || die "缺少 apt-ftparchive"
command -v dpkg-deb       >/dev/null || die "缺少 dpkg-deb"
[[ -d "$REPO" ]] || die "仓库根不存在: $REPO"
[[ $INDEX_ONLY -eq 1 && ${#DEBS[@]} -gt 0 ]] && die "--index-only 与 deb 参数互斥"
[[ ${#DEBS[@]} -eq 0 && $INDEX_ONLY -eq 0 ]] && INDEX_ONLY=1
gpg --batch --list-secret-keys "$KEYID" >/dev/null 2>&1 \
  || die "GNUPGHOME=$GNUPGHOME 中没有签名私钥 $KEYID"

if [[ $DRY_RUN -eq 1 ]]; then
  log "DRY-RUN 模式：仓库 $REPO（不写入任何内容）"
else
  mkdir -p "$REPO/pool/$COMPONENT" "$REPO/$INDEX_DIR"
fi

# ---------- 第 1 步：收 deb 进 pool ----------
for DEB in ${DEBS[@]+"${DEBS[@]}"}; do
  [[ -f "$DEB" ]] || die "找不到 deb: $DEB"
  PKG="$(dpkg-deb -f "$DEB" Package)"
  VER="$(dpkg-deb -f "$DEB" Version)"
  DARCH="$(dpkg-deb -f "$DEB" Architecture)"
  [[ -n "$PKG" && -n "$VER" ]] || die "$DEB 缺少 Package/Version 字段"
  [[ "$DARCH" == "$ARCH" || "$DARCH" == "all" ]] \
    || die "$PKG $VER 架构 $DARCH 与仓库 $ARCH 不符，拒收"
  DEST="$REPO/pool/$COMPONENT/${PKG:0:1}/$PKG"
  BASE="$(basename "$DEB")"
  [[ "$BASE" == "${PKG}_${VER}_${DARCH}.deb" ]] \
    || warn "文件名 $BASE 与规范 ${PKG}_${VER}_${DARCH}.deb 不一致（仍将按内容入库）"
  log "收编 $PKG $VER ($DARCH) -> pool/$COMPONENT/${PKG:0:1}/$PKG/"
  if [[ $DRY_RUN -eq 0 ]]; then
    mkdir -p "$DEST"
    cp -f "$DEB" "$DEST/.$BASE.tmp"      # 先落临时名
    mv -f "$DEST/.$BASE.tmp" "$DEST/$BASE"  # 再原子改名
  fi
done

# ---------- 第 2 步：重建 Packages(.gz) ----------
log "重建 $INDEX_DIR/Packages(.gz)"
if [[ $DRY_RUN -eq 0 ]]; then
  ( cd "$REPO" && apt-ftparchive packages "pool/$COMPONENT" ) > "$REPO/$INDEX_DIR/Packages.tmp"
  mv -f "$REPO/$INDEX_DIR/Packages.tmp" "$REPO/$INDEX_DIR/Packages"
  gzip -9c "$REPO/$INDEX_DIR/Packages" > "$REPO/$INDEX_DIR/Packages.gz.tmp"
  mv -f "$REPO/$INDEX_DIR/Packages.gz.tmp" "$REPO/$INDEX_DIR/Packages.gz"
fi

# ---------- 第 3 步：Release + 签名 ----------
log "重新生成 Release 并签名（keyid=${KEYID:0:16}…）"
if [[ $DRY_RUN -eq 0 ]]; then
  # apt-ftparchive release 会枚举 dists/$CODENAME 下所有文件并计算校验和，
  # 因此必须先移除旧的 Release/InRelease/Release.gpg，防止陈旧的签名单元被编入
  rm -f "$REPO/dists/$CODENAME/InRelease" \
        "$REPO/dists/$CODENAME/Release.gpg" \
        "$REPO/dists/$CODENAME/Release"
  RELCONF="$(mktemp)"
  cat > "$RELCONF" <<EOF
APT::FTPArchive::Release {
  Origin "$ORIGIN";
  Label "$LABEL";
  Suite "$CODENAME";
  Codename "$CODENAME";
  Architectures "$ARCH";
  Components "$COMPONENT";
  Description "$DESC";
};
EOF
  ( cd "$REPO" && apt-ftparchive -c "$RELCONF" release "dists/$CODENAME" ) \
    > "$REPO/dists/$CODENAME/Release.tmp"
  mv -f "$REPO/dists/$CODENAME/Release.tmp" "$REPO/dists/$CODENAME/Release"
  rm -f "$RELCONF"
  gpg --batch --yes --default-key "$KEYID" --clearsign \
    -o "$REPO/dists/$CODENAME/InRelease" "$REPO/dists/$CODENAME/Release"
  gpg --batch --yes --default-key "$KEYID" -abs \
    -o "$REPO/dists/$CODENAME/Release.gpg" "$REPO/dists/$CODENAME/Release"
fi

COUNT="?"
[[ $DRY_RUN -eq 0 ]] && COUNT="$(grep -c '^Package: ' "$REPO/$INDEX_DIR/Packages" || true)"
log "本地索引重建完成：$COUNT 个包"

# ---------- 第 4 步：部署到服务器（分阶段，索引最后上线） ----------
if [[ $DO_DEPLOY -eq 1 ]]; then
  log "部署到 $DEPLOY_HOST:$REMOTE_REPO（顺序：pool → Packages → Release/InRelease）"
  RSYNC=(rsync -az --chmod=Du=rwx,Dgo=rx,Fu=rw,Fgo=r)
  run ssh -o BatchMode=yes "$DEPLOY_HOST" \
      "mkdir -p '$REMOTE_REPO/pool' '$REMOTE_REPO/$INDEX_DIR' '$REMOTE_REPO/dists/$CODENAME'"
  run "${RSYNC[@]}" "$REPO/pool/" "$DEPLOY_HOST:$REMOTE_REPO/pool/"
  run "${RSYNC[@]}" "$REPO/$INDEX_DIR/Packages" "$REPO/$INDEX_DIR/Packages.gz" \
      "$DEPLOY_HOST:$REMOTE_REPO/$INDEX_DIR/"
  run "${RSYNC[@]}" "$REPO/dists/$CODENAME/Release" \
      "$REPO/dists/$CODENAME/Release.gpg" \
      "$REPO/dists/$CODENAME/InRelease" \
      "$DEPLOY_HOST:$REMOTE_REPO/dists/$CODENAME/"
else
  log "未指定 --deploy，跳过服务器同步"
fi

# ---------- 第 5 步：验证 ----------
[[ $SKIP_VERIFY -eq 1 || $DRY_RUN -eq 1 ]] && { log "结束（验证跳过）。"; exit 0; }

FAIL=0
ok()  { echo "  ✅ $*"; }
bad() { echo "  ❌ $*"; FAIL=1; }

log "验证：本地索引一致性"
# Packages 中每个 Filename 存在且 MD5 吻合
IDIR="$INDEX_DIR" python3 - "$REPO" <<'EOF' && ok "Packages 引用与 MD5 全部吻合" || bad "Packages 引用/MD5 不一致"
import sys, os, hashlib
base = sys.argv[1]
text = open(os.path.join(base, os.environ['IDIR'], 'Packages')).read()
for stanza in text.strip().split('\n\n'):
    d = dict(l.split(': ', 1) for l in stanza.split('\n') if ': ' in l)
    fn = os.path.join(base, d['Filename'])
    assert os.path.exists(fn), 'missing ' + d['Filename']
    assert hashlib.md5(open(fn,'rb').read()).hexdigest() == d['MD5sum'], 'md5 ' + d['Filename']
EOF

# Release 中的 Packages(.gz) 条目与实际文件吻合
REL_OK=1
for f in "$COMPONENT/binary-$ARCH/Packages" "$COMPONENT/binary-$ARCH/Packages.gz"; do
  want="$(md5sum "$REPO/dists/$CODENAME/$f" | cut -d' ' -f1)"
  grep -q "^ $want " "$REPO/dists/$CODENAME/Release" || REL_OK=0
done
[[ $REL_OK -eq 1 ]] && ok "Release 中 Packages(.gz) 校验和正确" || bad "Release 校验和滞后"

gpg --batch --verify "$REPO/dists/$CODENAME/InRelease" >/dev/null 2>&1 \
  && ok "InRelease 签名有效" || bad "InRelease 签名无效"

if [[ $DO_DEPLOY -eq 1 ]]; then
  log "验证：本地 vs 服务器 md5"
  LOCAL_MD5="$(cd "$REPO" && md5sum "$INDEX_DIR/Packages" "dists/$CODENAME/Release" "dists/$CODENAME/InRelease")"
  REMOTE_MD5="$(ssh -o BatchMode=yes "$DEPLOY_HOST" "cd '$REMOTE_REPO' && md5sum '$INDEX_DIR/Packages' 'dists/$CODENAME/Release' 'dists/$CODENAME/InRelease'")"
  [[ "$LOCAL_MD5" == "$REMOTE_MD5" ]] && ok "三个索引文件 md5 两端一致" || { bad "两端 md5 不一致"; diff <(echo "$LOCAL_MD5") <(echo "$REMOTE_MD5") || true; }

  log "验证：HTTPS 可达性（$SITE_URL）"
  curl -fsS -o /dev/null --max-time 20 "$SITE_URL/dists/$CODENAME/InRelease" \
    && ok "HTTPS InRelease 200" || bad "HTTPS InRelease 不可达"
  HTTPS_COUNT="$(curl -fsS --max-time 20 "$SITE_URL/$INDEX_DIR/Packages.gz" | zcat | grep -c '^Package: ' || true)"
  [[ "$HTTPS_COUNT" == "$COUNT" ]] && ok "HTTPS Packages.gz 包数一致（$COUNT）" \
    || bad "HTTPS Packages.gz 包数 $HTTPS_COUNT != 本地 $COUNT"
fi

log "验证：隔离 apt 可见性（不影响系统状态）"
TMPD="$(mktemp -d)"; trap 'rm -rf "$TMPD"' EXIT
mkdir -p "$TMPD/lists/partial" "$TMPD/cache/archives/partial"
APT_BASE="$SITE_URL"
[[ $DO_DEPLOY -eq 0 ]] && APT_BASE="file://$REPO"
cat > "$TMPD/sources.list" <<EOF
deb [signed-by=$REPO/thirdparty.gpg] $APT_BASE $CODENAME $COMPONENT
EOF
if apt-get \
    -o "Dir::Etc::sourcelist=$TMPD/sources.list" \
    -o "Dir::Etc::sourceparts=/dev/null" \
    -o "Dir::State::lists=$TMPD/lists" \
    -o "Dir::Cache=$TMPD/cache" \
    -o "Debug::NoLocking=1" \
    -o "Acquire::Languages=none" \
    update >"$TMPD/apt.log" 2>&1; then
  ok "apt-get update 通过"
else
  bad "apt-get update 失败"; tail -15 "$TMPD/apt.log"
fi

if [[ $FAIL -eq 0 ]]; then
  log "全部验证通过 ✅（包数 $COUNT）"
else
  die "存在验证失败项 ❌"
fi
