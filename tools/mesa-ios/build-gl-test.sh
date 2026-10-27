#!/bin/bash
# Build fuxi_gl_test against the image's libOSMesa.8.dylib.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=${BUILD_DIR:-$ROOT/build/mesa-ios}
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --find clang)
LIB=$B/build-ios/src/gallium/targets/osmesa/libOSMesa.8.dylib
[ -f "$LIB" ] || { echo "missing $LIB"; exit 1; }

"$CC" -arch arm64 -miphoneos-version-min=15.0 -isysroot "$SDK" \
  -O2 -I"$ROOT/tools/mesa-ios/fuxi-drm" \
  "$ROOT/tools/mesa-ios/experimental/fuxi_gl_test.c" "$LIB" \
  -o "$B/fuxi_gl_test"
ls -lh "$B/fuxi_gl_test"
file "$B/fuxi_gl_test" || true
