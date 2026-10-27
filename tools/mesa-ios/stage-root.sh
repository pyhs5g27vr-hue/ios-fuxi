#!/bin/bash
# Stage the Mesa GLES stack into the iOS root image.
#
#   ./stage-root.sh <root.img> <artifacts-dir>
#
# <root.img>       the raw HFS+ image that goes to userdata (16 GiB)
# <artifacts-dir>  the directory holding libOSMesa.8.dylib and libredwood-gl.dylib
#                  (download the 'mesa-ios' artifact and point here)
#
# What it installs, following hackios-xnu-userland's build.sh GL=1 layout:
#   /redwood/lib/libOSMesa.8.dylib
#   /redwood/lib/libredwood-gl.dylib
# and prints what still has to be wired by hand (the loader that makes backboardd
#  take libredwood-gl; see notes/next.md).
#
# The volume header fix at the end is the one the author's README demands: an image
# last mounted by Linux (lastMountedVersion "H+Lx") makes iOS daemons crash (ns2 0x5).
set -euo pipefail

IMG=${1:?usage: stage-root.sh <root.img> <artifacts-dir>}
ART=${2:?usage: stage-root.sh <root.img> <artifacts-dir>}
MNT=${MNT:-/root/fuxi/mnt/stage-root}

OSMESA=$ART/libOSMesa.8.dylib
BRIDGE=$ART/libredwood-gl.dylib
for f in "$OSMESA" "$BRIDGE"; do
  [ -f "$f" ] || { echo "missing $f"; exit 1; }
done

echo "=== 1) fsck ==="
fsck.hfsplus -f -y "$IMG" 2>&1 | tail -2

echo "=== 2) mount ==="
mkdir -p "$MNT"
for m in $(mount | awk '/hfsplus/{print $3}'); do umount -l "$m" 2>/dev/null || true; done
mount -t hfsplus -o loop,force,rw "$IMG" "$MNT"

echo "=== 3) install the libraries ==="
mkdir -p "$MNT/redwood/lib" "$MNT/usr/local/lib"
cp "$OSMESA" "$MNT/redwood/lib/libOSMesa.8.dylib"
cp "$BRIDGE" "$MNT/redwood/lib/libredwood-gl.dylib"
# the bridge's dependency is /redwood/lib/libOSMesa.8.dylib; a second copy in /usr/local/lib
# lets tools that look there find it too
cp "$OSMESA" "$MNT/usr/local/lib/libOSMesa.8.dylib"
chown 0:0 "$MNT"/redwood/lib/*.dylib "$MNT"/usr/local/lib/libOSMesa.8.dylib
chmod 755 "$MNT"/redwood/lib/*.dylib "$MNT"/usr/local/lib/libOSMesa.8.dylib
ls -l "$MNT/redwood/lib/"

echo "=== 4) unmount and fix the volume header for iOS ==="
sync
umount "$MNT"
python3 - "$IMG" <<'PY'
import struct, sys
p = sys.argv[1]
with open(p, 'r+b') as f:
    f.seek(1024 + 8)
    cur = f.read(4)
    if cur != b'HFSJ':
        f.seek(1024 + 8)
        f.write(b'HFSJ')
        print('lastMountedVersion %r -> HFSJ' % cur)
    else:
        print('lastMountedVersion already HFSJ')
    f.seek(1024 + 4)
    attr = struct.unpack('>I', f.read(4))[0]
    if not attr & 0x100:
        f.seek(1024 + 4)
        f.write(struct.pack('>I', attr | 0x100))
        print('set the unmounted bit')
PY
fsck.hfsplus -f -y "$IMG" 2>&1 | tail -2

echo "=== 5) what remains (by hand, see notes/next.md) ==="
cat <<'EOF'
  a) 让 backboardd 加载 libredwood-gl.dylib（K90 是 build.sh GL=1 + libredwood.dylib）
  b) fuxi-drm 目前是存根：Mesa 找不到 GPU 会退回 softpipe（纯 CPU），不会崩但也没有加速
  c) 上机后先看 backboardd 的日志里有没有 "eagl" 与 "libredwood-gl" 字样
EOF
echo STAGE_ROOT_OK
