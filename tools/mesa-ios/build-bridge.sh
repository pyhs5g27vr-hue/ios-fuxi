#!/bin/bash
# Build the EAGL bridge (libredwood-gl.dylib) on top of the Mesa library.
#
# eagl.c is the EAGLContext replacement the window server gets: CoreAnimation's
# GLES renderer (CA::WindowServer::EAGLServer) asks it for an API-3 context and a
# function table. mkeagl.py generates that table from the slot list
# (eagl_slots.txt, read off iOS's OpenGLES.framework) and the gl* functions our
# Mesa exports, so the two halves cannot drift.
#
# Sources and the slot list come from hackios-xnu-userland (iola1999, k90-port);
# the slot list is for iOS 16.5 (20F66) and must be re-derived for 20H392 before
# this can be trusted on the device — see notes/slot-table.md.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KIT=$ROOT/tools/mesa-ios
B=${BUILD_DIR:-$ROOT/build/mesa-ios}
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --find clang)
MIN_IOS=${MIN_IOS:-15.0}

MESA_SRC=$B/mesa-25.0.7
MESA_BUILD=$B/build-ios
EXPORTED=$MESA_BUILD/exported-gl.txt
GLXML=$MESA_SRC/src/mapi/glapi/registry/gl.xml
OSMESA_LIB=$MESA_BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib

for f in "$EXPORTED" "$GLXML" "$OSMESA_LIB"; do
  [ -f "$f" ] || { echo "missing: $f (run build.sh first)"; exit 1; }
done

echo "=== 1) generate the EAGL function table ==="
GEN=$B/eagl_gen.h
python3 "$KIT/userland/mkeagl.py" \
  "$KIT/userland/eagl_slots.txt" \
  "$GLXML" \
  "$EXPORTED" \
  "$KIT/userland/eagl.c" > "$GEN"
wc -l "$GEN"

echo "=== 2) compile the bridge ==="
OUT=$B/libredwood-gl.dylib
"$CC" -arch arm64 -miphoneos-version-min=$MIN_IOS -isysroot "$SDK" \
  -O2 -fPIC -dynamiclib \
  -I"$B" -I"$KIT/userland" -I"$MESA_SRC/include" -I"$MESA_BUILD/include" -I"$MESA_BUILD/src" \
  "$KIT/userland/eagl.c" "$KIT/userland/gl_osmesa.c" "$KIT/userland/fuxi_probe.c" \
  "$OSMESA_LIB" \
  -framework CoreFoundation -framework IOKit -framework IOSurface -framework CoreGraphics \
  -install_name /redwood/lib/libredwood-gl.dylib \
  -o "$OUT"
ls -lh "$OUT"
install_name_tool -id /redwood/lib/libredwood-gl.dylib "$OUT"
echo BRIDGE_OK
