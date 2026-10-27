#!/bin/bash
# Build Mesa for the iOS userland (arm64-apple-ios) — the first brick of the
# CoreAnimation-on-GLES route.
#
# Modelled on hackios-xnu-userland's tools/mk-mesa.sh (iola1999/hackios-redmi-k90-annibale),
# which is the only port that renders CoreAnimation through Mesa on a phone.
# Differences here:
#   * the cross compiler is Xcode's clang on a macOS runner, not Theos' xnu-clang,
#   * the DRM substitute starts as a stub (fuxi-drm) because fuxi's GPU service
#     ABI (NativeAdreno) is not the RedwoodGPU one yet — the stub proves the
#     build; wiring it to the kernel is the next milestone.
set -euo pipefail

VERSION=${MESA_VERSION:-25.0.7}
SHA256=${MESA_SHA256:-592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6}
MESA_URL=https://archive.mesa3d.org/mesa-$VERSION.tar.xz

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KIT=$ROOT/tools/mesa-ios
B=${BUILD_DIR:-$ROOT/build/mesa-ios}
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --find clang)
AR=$(xcrun --find llvm-ar || true)
RANLIB=$(xcrun --find llvm-ranlib || true)
STRIP=$(xcrun --find llvm-strip || true)
MIN_IOS=${MIN_IOS:-15.0}

echo "=== toolchain ==="
echo "SDK   = $SDK"
echo "CC    = $CC"
$CC --version | head -1 || true

mkdir -p "$B"
cd "$B"

if [ ! -d mesa-$VERSION ]; then
  [ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz "$MESA_URL"
  echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
  tar xJf mesa-$VERSION.tar.xz
  for p in "$KIT"/patches/*.patch; do
    echo "  patch $(basename "$p")"
    patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p" || echo "  (hunk rejected, continuing)"
  done
fi

# the libdrm stand-in lives inside the tree, named as the freedreno-darwin patch expects
rm -rf mesa-$VERSION/src/redwood-drm
cp -r "$KIT"/fuxi-drm mesa-$VERSION/src/redwood-drm

if [ ! -x venv/bin/python3 ]; then
  python3 -m venv venv
  venv/bin/pip -q install mako pyyaml packaging "meson==1.5.1" ninja
fi
export PATH=$B/venv/bin:$PATH

cat > cross-ios.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-miphoneos-version-min=$MIN_IOS', '-isysroot', '$SDK']
cpp = ['$CC++', '-arch', 'arm64', '-miphoneos-version-min=$MIN_IOS', '-isysroot', '$SDK', '-fuse-cxa-atexit']
ar = '${AR:-llvm-ar}'
ranlib = '${RANLIB:-llvm-ranlib}'
strip = '${STRIP:-llvm-strip}'
pkg-config = 'pkg-config'

[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
INI

if [ ! -d build-ios ]; then
  meson setup build-ios \
    --cross-file cross-ios.ini \
    --buildtype release \
    -Dgallium-drivers=softpipe,freedreno \
    -Dvulkan-drivers= \
    -Dplatforms= \
    -Dosmesa=true \
    -Dglx=disabled \
    -Degl=disabled \
    -Dgbm=disabled \
    -Dllvm=disabled \
    -Dopengl=true \
    -Dgles1=disabled \
    -Dgles2=disabled \
    -Dglvnd=disabled \
    -Dzstd=disabled \
    -Dlz4=disabled \
    -Dvalgrind=disabled \
    -Dlibunwind=disabled \
    -Dbuild-tests=false \
    -Dandroid-libbacktrace=disabled
fi

ninja -C build-ios

echo "=== outputs ==="
find build-ios -name 'libOSMesa*.dylib' -o -name 'libgallium*.dylib' | head -10
ls -lh build-ios/src/gallium/targets/osmesa/libOSMesa*.dylib 2>/dev/null || true
echo MESA_IOS_OK
