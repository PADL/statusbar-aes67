#!/usr/bin/env bash
# Copyright 2026 PADL Software Pty Ltd
# SPDX-License-Identifier: MIT
#
# Cross-compile the statusbar-aes67 Debian package for arm64 inside a
# native amd64 container — no qemu emulation. Produces a .deb stamped
# Architecture: arm64. Tests are not run (cross-built binaries cannot
# execute on the build host); for test runs use the emulated path
# (scripts/container-build.sh).
#
# Dependency .debs must already be cross-built in DEB_OUTPUT — run each
# dep's own container-build-cross.sh first.
set -euo pipefail

PKG="aes67"
DEPS="core"
# STATUSBAR_TOOLCHAIN picks the compiler: clang (default) or gcc. clang
# cross-targets with a single multi-target driver; gcc needs Debian's
# aarch64-linux-gnu cross compiler, which only forky and later ship, so the
# base image default follows the toolchain.
STATUSBAR_TOOLCHAIN="${STATUSBAR_TOOLCHAIN:-clang}"
case "$STATUSBAR_TOOLCHAIN" in
  clang) _default_debian="trixie" ;;
  gcc)   _default_debian="forky" ;;
  *)
    echo "error: STATUSBAR_TOOLCHAIN must be 'clang' or 'gcc'" >&2
    exit 1
    ;;
esac

# Statically link the C++ runtime for gcc cross builds by default, for the same
# reason as the native path: a C++26 GCC 16 binary needs a newer libstdc++ than
# the deployment target carries. See scripts/container-build.sh.
case "$STATUSBAR_TOOLCHAIN" in
  gcc) STATUSBAR_STATIC_CXX="${STATUSBAR_STATIC_CXX:-ON}" ;;
  *)   STATUSBAR_STATIC_CXX="${STATUSBAR_STATIC_CXX:-OFF}" ;;
esac

DEBIAN_VERSION="${DEBIAN_VERSION:-$_default_debian}"
TARGET_ARCH="${TARGET_ARCH:-arm64}"
ENGINE="${CONTAINER_ENGINE:-podman}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE_DIR="$(dirname "$SCRIPT_DIR")"
EXPORT_ROOT="$(dirname "$TREE_DIR")"
DEB_OUTPUT="${DEB_OUTPUT:-$EXPORT_ROOT/deb-output}"
IMAGE="localhost/statusbar-deb-builder-cross:$DEBIAN_VERSION-$TARGET_ARCH-$STATUSBAR_TOOLCHAIN"

MOUNT_OPT=""
if [ "$(uname -s)" = "Linux" ]; then
  MOUNT_OPT=",z"
fi

# Debian package revision: a monotonic build id appended as the package's
# Debian revision (version becomes x.y.z-<rev>), so every rebuild produces a
# strictly-newer package and apt always upgrades instead of refusing an
# equal-or-older version. The umbrella container-build-cross.sh exports one
# shared STATUSBAR_DEB_REVISION per run so a multi-package build gets a
# consistent revision.
DEB_REVISION="${STATUSBAR_DEB_REVISION:-$(date -u +%Y%m%d%H%M%S)}"


mkdir -p "$DEB_OUTPUT"

# Rebuild the cross builder image when it is missing or its Containerfile
# changed — the image records the Containerfile checksum in a label at build
# time. The tag is shared by all statusbar packages, whose Containerfiles are
# kept byte-identical so any package can (re)build the image for the others.
CF_SUM="$(cksum "$TREE_DIR/Containerfile.cross" | cut -d' ' -f1)"
if [ "$("$ENGINE" image inspect \
         --format '{{index .Config.Labels "statusbar.containerfile"}}' \
         "$IMAGE" 2>/dev/null)" != "$CF_SUM" ]; then
  echo "=== building cross builder image $IMAGE ==="
  "$ENGINE" build -t "$IMAGE" \
    --platform linux/amd64 \
    --build-arg "DEBIAN_VERSION=$DEBIAN_VERSION" \
    --build-arg "TARGET_ARCH=$TARGET_ARCH" \
    --build-arg "STATUSBAR_TOOLCHAIN=$STATUSBAR_TOOLCHAIN" \
    --label "statusbar.containerfile=$CF_SUM" \
    -f "$TREE_DIR/Containerfile.cross" "$TREE_DIR"
fi

for d in $DEPS; do
  if ! ls "$DEB_OUTPUT"/statusbar-"$d"-dev_*_"$TARGET_ARCH".deb >/dev/null 2>&1; then
    echo "error: dependency statusbar-$d ($TARGET_ARCH) is not cross-built" >&2
    echo "       run each dep's container-build-cross.sh first" >&2
    exit 1
  fi
done

echo "=== cross-building statusbar-$PKG .deb (target $TARGET_ARCH) ==="
"$ENGINE" run --rm \
  --platform linux/amd64 \
  -v "$TREE_DIR:/src:ro$MOUNT_OPT" \
  -v "$DEB_OUTPUT:/debs:rw$MOUNT_OPT" \
  -v "statusbar-deb-ccache-cross-$TARGET_ARCH-$STATUSBAR_TOOLCHAIN:/root/.ccache" \
  -e "DEPS=$DEPS" \
  -e "PKG=$PKG" \
  -e "TARGET_ARCH=$TARGET_ARCH" \
  -e "STATUSBAR_TOOLCHAIN=$STATUSBAR_TOOLCHAIN" \
  -e "STATUSBAR_STATIC_CXX=$STATUSBAR_STATIC_CXX" \
  -e "DEB_REVISION=$DEB_REVISION" \
  "$IMAGE" bash -euo pipefail -c '
    debs=()
    for d in $DEPS; do
      debs+=(/debs/statusbar-"$d"_*_"$TARGET_ARCH".deb \
             /debs/statusbar-"$d"-dev_*_"$TARGET_ARCH".deb)
    done
    if [ "${#debs[@]}" -gt 0 ]; then
      apt-get update -qq
      apt-get install -y --no-install-recommends "${debs[@]}"
      # Restore clang-scan-deps if apt evicted the host clang-tools. The
      # destination is whatever path the image recorded at build time -- it is
      # version-specific (/usr/lib/llvm-19/... on trixie, /usr/bin/... on
      # forky), so hardcoding one breaks on the other base.
      SCAN_DEPS="$(cat /opt/cross-tools/clang-scan-deps.path 2>/dev/null)"
      if [ -n "$SCAN_DEPS" ] && [ ! -e "$SCAN_DEPS" ] \
         && [ -e /opt/cross-tools/clang-scan-deps ]; then
        mkdir -p "$(dirname "$SCAN_DEPS")"
        cp /opt/cross-tools/clang-scan-deps "$SCAN_DEPS"
      fi
    fi
    cmake -Wno-dev -S /src -B /build -G Ninja \
      --toolchain /usr/local/lib/cmake/statusbar-core/toolchain-"$STATUSBAR_TOOLCHAIN"-aarch64.cmake \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DENABLE_FUZZING=OFF \
      -DENABLE_STATIC_CXX_RUNTIME="$STATUSBAR_STATIC_CXX" \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      -DCPACK_DEBIAN_PACKAGE_ARCHITECTURE="$TARGET_ARCH"
    cmake --build /build
    ( cd /build && cpack -G DEB -D CPACK_DEBIAN_PACKAGE_RELEASE="$DEB_REVISION" )
    rm -f /debs/statusbar-"$PKG"_*_"$TARGET_ARCH".deb \
          /debs/statusbar-"$PKG"-dev_*_"$TARGET_ARCH".deb
    cp -v /build/*.deb /debs/
  '

echo "=== statusbar-$PKG packages in $DEB_OUTPUT ==="
ls -1 "$DEB_OUTPUT"/statusbar-"$PKG"_*_"$TARGET_ARCH".deb \
      "$DEB_OUTPUT"/statusbar-"$PKG"-dev_*_"$TARGET_ARCH".deb 2>/dev/null || true
