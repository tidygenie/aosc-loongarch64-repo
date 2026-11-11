#!/usr/bin/env bash
# Package the mainline 16K RT kernels as AOSC loongarch64 DEBs.
# Usage: package-kernel-v3.sh <gcc-o3|clang-lto>; DEBREL=N selects the deb revision.
set -euo pipefail
DEBREL="${DEBREL:-1}"
[[ "$DEBREL" =~ ^[0-9]+$ ]]

VARIANT="${1:?usage: package-kernel-v3.sh <gcc-o3|clang-lto>}"
case "$VARIANT" in
  gcc-o3)    SRC="${KSRC:-/home/houge/infra/kernel/build-src/mainline-gcc-o3}";    OTHER=clang-lto ;;
  clang-lto) SRC="${KSRC:-/home/houge/infra/kernel/build-src/mainline-clang-lto}"; OTHER=gcc-o3 ;;
  *) echo "unknown variant: $VARIANT" >&2; exit 2 ;;
esac
[[ -d "$SRC" ]] || { echo "missing build tree: $SRC" >&2; exit 1; }
OUT=/home/houge/infra/kernel/debs
[[ -d "$SRC" ]] || { echo "missing build tree: $SRC" >&2; exit 1; }
VER=$(make -s -C "$SRC" kernelversion)
REL=$(make -s -C "$SRC" kernelrelease)
PKG="linux-kernel-mainline-16k-rt-${VER}-${VARIANT}"
HDR="linux-headers-mainline-16k-rt-${VER}-${VARIANT}"
OTHER_PKG="linux-kernel-mainline-16k-rt-${VER}-${OTHER}"
OTHER_HDR="linux-headers-mainline-16k-rt-${VER}-${OTHER}"
DKMS_DEP=""
if [[ "$VARIANT" == clang-lto ]]; then
  DKMS_DEP=", loonggpu-kernel-dkms (>= 2:1.0.2+lnd25.1-3)"
fi

# Refuse to package a tree that does not satisfy the required ABI/configuration.
grep -q '^CONFIG_PAGE_SIZE_16KB=y$' "$SRC/.config"
grep -q '^CONFIG_PREEMPT_RT=y$' "$SRC/.config"
[[ "$REL" == *-mainline-16k-rt* ]]
[[ -s "$SRC/Module.symvers" ]]

echo "==> $PKG ($REL, $VARIANT)"
mkdir -p "$OUT"

make_maintainer_scripts() {
  local dir=$1
  cat > "$dir/postinst" <<EOF
#!/bin/bash
set -e
if [ -x /usr/bin/depmod ]; then depmod '$REL'; fi
if [ -x /usr/bin/dkms ]; then dkms autoinstall --kernelver '$REL'; fi
# AOSC update-initramfs regenerates every installed kernel; target only this ABI.
if [ -x /usr/bin/dracut ]; then dracut -q --force '/boot/initramfs-$REL.img' '$REL'; fi
if ! lsinitrd '/boot/initramfs-$REL.img' 2>/dev/null | grep -q 'updates/dkms/loonggpu.ko'; then
  echo "LoongGPU is missing from /boot/initramfs-$REL.img" >&2
  exit 1
fi
EOF
  cat > "$dir/prerm" <<EOF
#!/bin/bash
if [ -x /usr/bin/dkms ]; then
  dkms status -k '$REL' | while IFS= read -r line; do
    case "\$line" in
      *': installed'*|*': built'*)
        name=\${line%%,*}
        dkms uninstall "\$name" -k '$REL' || true ;;
    esac
  done
fi
rm -f '/boot/initramfs-$REL.img'
EOF
  cat > "$dir/postrm" <<EOF
#!/bin/bash
rm -rf '/usr/lib/modules/$REL/updates'
EOF
  chmod 755 "$dir/postinst" "$dir/prerm" "$dir/postrm"
}

# ---------- kernel image and modules ----------
STAGE=$(mktemp -d "/tmp/loong-kernel-${VARIANT}-stage.XXXXXX")
[[ "$STAGE" == /tmp/loong-kernel-*-stage.* ]]
mkdir -p "$STAGE/DEBIAN" "$STAGE/boot" "$STAGE/usr/lib/modules/$REL"
make -C "$SRC" INSTALL_MOD_PATH="$STAGE/usr" INSTALL_MOD_STRIP=1 modules_install >/dev/null

IMG="$SRC/arch/loongarch/boot/vmlinuz.efi"
[[ -f "$IMG" ]] || IMG="$SRC/arch/loongarch/boot/vmlinuz"
[[ -s "$IMG" ]]
cp "$IMG" "$STAGE/boot/vmlinuz-$REL"
cp "$SRC/System.map" "$STAGE/boot/System.map-$REL"
cp "$SRC/.config" "$STAGE/boot/config-$REL"
rm -f "$STAGE/usr/lib/modules/$REL/build" "$STAGE/usr/lib/modules/$REL/source"
ln -s "/usr/src/linux-headers-$REL" "$STAGE/usr/lib/modules/$REL/build"
ln -s "/usr/src/linux-headers-$REL" "$STAGE/usr/lib/modules/$REL/source"
depmod -b "$STAGE/usr" "$REL"
make_maintainer_scripts "$STAGE/DEBIAN"

SIZE=$(du -sk "$STAGE" | cut -f1)
cat > "$STAGE/DEBIAN/control" <<CTL
Package: ${PKG}
Version: ${VER}-${DEBREL}
Architecture: loongarch64
Maintainer: Local Infra <infra@localhost>
Installed-Size: ${SIZE}
Section: kernel
Priority: optional
Depends: kmod, dracut-ng${DKMS_DEP}
Recommends: dkms
Description: Mainline Linux ${VER} for LoongArch (16K pages, PREEMPT_RT, ${VARIANT})
 Built from kernel.org ${VER} with the AOSC OS configuration as a baseline,
 16KB pages, PREEMPT_RT scheduling, and single-threaded ${VARIANT} build.
CTL
dpkg-deb --build --root-owner-group "$STAGE" "$OUT/${PKG}_${VER}-${DEBREL}_loongarch64.deb" >/dev/null
rm -rf -- "$STAGE"

# ---------- headers / external-module build tree ----------
STAGE2=$(mktemp -d /tmp/loong-kernel-headers-stage.XXXXXX)
[[ "$STAGE2" == /tmp/loong-kernel-headers-stage.* ]]
HDRROOT="$STAGE2/usr/src/linux-headers-$REL"
mkdir -p "$STAGE2/DEBIAN" "$HDRROOT"
(
  cd "$SRC"
  tar cf - --exclude='*.o' --exclude='*.ko' --exclude='*.a' --exclude='*.cmd' \
    Makefile .config Module.symvers System.map \
    arch/loongarch/Makefile arch/loongarch/include \
    include scripts tools/build tools/objtool tools/bpf
) | tar xf - -C "$HDRROOT"
# Only the kernel package owns DKMS and initramfs lifecycle; headers have no scripts.
SIZE2=$(du -sk "$STAGE2" | cut -f1)
cat > "$STAGE2/DEBIAN/control" <<CTL
Package: ${HDR}
Version: ${VER}-${DEBREL}
Architecture: loongarch64
Maintainer: Local Infra <infra@localhost>
Installed-Size: ${SIZE2}
Section: devel
Priority: optional
Depends: ${PKG}, kmod
Recommends: dkms
Description: Headers for mainline Linux ${VER} (16K pages, PREEMPT_RT, ${VARIANT})
 Build tree for out-of-tree modules and DKMS against ${REL}.
CTL
dpkg-deb --build --root-owner-group "$STAGE2" "$OUT/${HDR}_${VER}-${DEBREL}_loongarch64.deb" >/dev/null
rm -rf -- "$STAGE2"

echo "==> products:"
ls -lh "$OUT/${PKG}_${VER}-${DEBREL}_loongarch64.deb" "$OUT/${HDR}_${VER}-${DEBREL}_loongarch64.deb"
