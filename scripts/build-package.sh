#!/usr/bin/env bash
# 轻量 abbs 兼容构建器：按 spec/autobuild 构建单个包。
#   ./scripts/build-package.sh packages/app-net/v2raya
set -euo pipefail
PKGDIR_SRC="${1:?usage: build-package.sh <package-dir>}"
PKGDIR_SRC="$(cd "$PKGDIR_SRC" && pwd)"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="$(basename "$PKGDIR_SRC")"

# shellcheck disable=SC1090
source "$PKGDIR_SRC/spec"
# shellcheck disable=SC1090
source "$PKGDIR_SRC/autobuild/defines"

OUT="$ROOT/build"
STAGE="$ROOT/build/stage/$NAME"
SRCDIR="${SRCDIR:-$ROOT/build/src/$NAME}"
PKGDIR="$STAGE"
ABTHREADS="${ABTHREADS:-$(nproc)}"
export SRCDIR PKGDIR ABTHREADS

rm -rf "$STAGE" "$SRCDIR"
mkdir -p "$STAGE" "$SRCDIR"

echo "==> $NAME $VER (REL=$REL)"
echo "==> SRCDIR=$SRCDIR"

# 源码准备：dir:: 表示使用包内 files/，tarball/git 交给 build 脚本自行处理
case "${SRCS:-}" in
  dir::*)
    cp -a "$PKGDIR_SRC/files/." "$SRCDIR/" ;;
  "")
    : ;;
  *)
    echo "==> SRCS=$SRCS（由 autobuild/build 负责获取）" ;;
esac

( cd "$SRCDIR" && bash "$PKGDIR_SRC/autobuild/build" )

mkdir -p "$STAGE/DEBIAN"
SIZE=$(du -sk "$STAGE" | cut -f1)
cat > "$STAGE/DEBIAN/control" <<CTL
Package: ${PKGNAME}
Version: ${VER}-${REL}
Architecture: loongarch64
Maintainer: Local Infra <infra@localhost>
Installed-Size: ${SIZE}
Section: ${PKGSEC}
Priority: optional
Depends: ${PKGDEP// /, }
Description: ${PKGDES}
CTL
DEB="$OUT/${PKGNAME}_${VER}-${REL}_loongarch64.deb"
mkdir -p "$OUT"
dpkg-deb --build --root-owner-group "$STAGE" "$DEB" >/dev/null
echo "==> $DEB"
[[ "${KEEP_STAGE:-0}" == 1 ]] || rm -rf "$STAGE"
