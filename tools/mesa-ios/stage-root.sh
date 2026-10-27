#!/bin/bash
# Stage the Mesa GLES stack into the iOS root image and (optionally) make the
# window server load the bridge.
#
#   ./stage-root.sh <root.img> <artifacts-dir> [--inject]
#
# <root.img>       the raw HFS+ image that goes to userdata (16 GiB)
# <artifacts-dir>  the directory holding libOSMesa.8.dylib and libredwood-gl.dylib
#                  (E:\DSH-Fuxi\gles-stack)
# --inject         add DYLD_INSERT_LIBRARIES to backboardd's launchd job so the
#                  bridge's constructor runs inside the window server. Without it the
#                  libraries are installed but nothing loads them.
#
# The volume header fix at the end is the one the author's README demands: an image
# last mounted by Linux (lastMountedVersion "H+Lx") makes iOS daemons crash (ns2 0x5).
set -euo pipefail

IMG=${1:?usage: stage-root.sh <root.img> <artifacts-dir> [--inject]}
ART=${2:?usage: stage-root.sh <root.img> <artifacts-dir> [--inject]}
INJECT=${3:-}
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
cp "$OSMESA" "$MNT/usr/local/lib/libOSMesa.8.dylib"
chown 0:0 "$MNT"/redwood/lib/*.dylib "$MNT"/usr/local/lib/libOSMesa.8.dylib
chmod 755 "$MNT"/redwood/lib/*.dylib "$MNT"/usr/local/lib/libOSMesa.8.dylib
ls -l "$MNT/redwood/lib/"

if [ "$INJECT" = "--inject" ]; then
  echo "=== 4) inject into the window server ==="
  PLIST="$MNT/System/Library/LaunchDaemons/com.apple.backboardd.plist"
  if [ ! -f "$PLIST" ]; then
    echo "  no backboardd plist at $PLIST — check the path before relying on --inject"
  else
    cp "$PLIST" "$PLIST.orig"
    python3 - "$PLIST" <<'PY'
import plistlib, sys, pathlib
p = pathlib.Path(sys.argv[1])
d = plistlib.loads(p.read_bytes())
env = d.setdefault('EnvironmentVariables', {})
libs = env.get('DYLD_INSERT_LIBRARIES', '')
if '/redwood/lib/libredwood-gl.dylib' not in libs:
    env['DYLD_INSERT_LIBRARIES'] = (libs + ':' if libs else '') + '/redwood/lib/libredwood-gl.dylib'
    p.write_bytes(plistlib.dumps(d))
    print('  DYLD_INSERT_LIBRARIES =', env['DYLD_INSERT_LIBRARIES'])
else:
    print('  already injected')
PY
    ls -l "$PLIST"
  fi
else
  echo "=== 4) skip injection (pass --inject to wire backboardd) ==="
fi

echo "=== 5) unmount and fix the volume header for iOS ==="
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

echo "=== 6) what to watch on the device ==="
cat <<'EOF'
  a) backboardd 的 stderr 里应出现桥的日志（"eagl"/"libredwood-gl"）
  b) Mesa 会先试 freedreno（我们的 winsys），失败则退回 softpipe（纯 CPU，不会崩）
  c) 若画面错乱/崩溃：槽位表版本不符（16.5 的 20F66 vs 我们的 20H392）
EOF
echo STAGE_ROOT_OK
