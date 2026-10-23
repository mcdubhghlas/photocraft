#!/bin/sh
# Build a Debian .deb from the staged FHS tree (REWRITE_PLAN.md Z1 packaging).
#
#   packaging/linux/deb.sh [--bin DIR] [--dist DIR] [--arch ARCH]
#
# Assembles the .deb by hand with `ar` + `tar` + `gzip` (the .deb container is an `ar` archive of
# debian-binary + control.tar.gz + data.tar.gz), so it needs NO dpkg-deb / fakeroot. The FHS tree is
# staged by `packaging/linux/dist.sh --stage` so the layout lives in one place. The Depends list is
# intentionally minimal and honest (libc6); the distro-accurate windowing/GPU dependency names stay
# in `packaging/linux/nfpm.yaml`, the reference recipe for a signed, dep-resolved build.
#
# No cargo, no network, no signing. Stages whatever is in --bin (default build/default/bin): run
# `make` first. Verified at the end with `ar t` and, if present, `dpkg-deb --info`/`--contents`.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/packaging/linux
APP_ID=ai.storyteller.photocraft

BIN=${PHOTOCRAFT_BIN:-$ROOT/build/default/bin}
DIST=${DIST:-$ROOT/dist/release}
ARCH=${PHOTOCRAFT_ARCH:-$(uname -m)}

while [ $# -gt 0 ]; do
    case $1 in
    --bin) BIN=$2; shift 2 ;;
    --dist) DIST=$2; shift 2 ;;
    --arch) ARCH=$2; shift 2 ;;
    -h | --help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# Debian architecture names.
case $ARCH in
x86_64 | amd64) DEB_ARCH=amd64; FHS_ARCH=x86_64 ;;
aarch64 | arm64) DEB_ARCH=arm64; FHS_ARCH=aarch64 ;;
*) DEB_ARCH=$ARCH; FHS_ARCH=$ARCH ;;
esac

for t in ar tar gzip; do
    command -v "$t" >/dev/null 2>&1 || { echo "deb: '$t' not found; cannot build a .deb" >&2; exit 0; }
done
for b in photocraft photocraft-cli; do
    [ -x "$BIN/$b" ] || { echo "deb: $BIN/$b missing; run 'make' first" >&2; exit 1; }
done

VERSION=${PHOTOCRAFT_VERSION:-$(
    awk '/define +PC_ENGINE_VERSION/ { for (i=1;i<=NF;i++) if ($i ~ /^"/) { v=$i; gsub(/"/,"",v); print v; exit } }' \
        "$ROOT/src/engine/build_info.c"
)}
[ -n "$VERSION" ] || { echo "deb: could not read version from src/engine/build_info.c" >&2; exit 1; }
# semver: 0.2.0-rc.1 -> 0.2.0~rc.1 so pre-releases sort before the release (Debian convention).
DEB_VERSION=$(printf '%s' "$VERSION" | sed 's/-/~/')
MAINTAINER=${PHOTOCRAFT_MAINTAINER:-"PhotoCraft maintainers <noreply@getartcraft.com>"}

echo "==> PhotoCraft $VERSION .deb for $DEB_ARCH"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pc-deb.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM
ROOTDIR="$WORK/root"

# ---- stage the FHS tree (shared with dist.sh) --------------------------------------------------
PHOTOCRAFT_BIN="$BIN" PHOTOCRAFT_ARCH="$FHS_ARCH" PHOTOCRAFT_VERSION="$VERSION" \
    sh "$HERE/dist.sh" --stage "$ROOTDIR" >/dev/null

INSTALLED_KB=$(du -sk "$ROOTDIR" | awk '{print $1}')

# ---- control.tar.gz ----------------------------------------------------------------------------
CTRL="$WORK/control"
mkdir -p "$CTRL"
cat > "$CTRL/control" <<EOF
Package: photocraft
Version: $DEB_VERSION-1
Architecture: $DEB_ARCH
Maintainer: $MAINTAINER
Installed-Size: $INSTALLED_KB
Section: graphics
Priority: optional
Homepage: https://getartcraft.com/apps/photocraft
Depends: libc6
Description: Open-source, native image editor with familiar, professional tools.
 Layers, masks, adjustment layers, layer styles, type and brushes; opens and
 saves layered PSD/PSB files; 8/16/32-bit documents; GPU compositing.
 Includes photocraft-cli, a headless converter and command runner.
 .
 Runtime libraries loaded at startup (install from your distribution): SDL3,
 OpenGL/EGL or Vulkan, freetype, harfbuzz, and the image codec libraries. The
 distro-accurate dependency names are in packaging/linux/nfpm.yaml.
EOF

# md5sums over the data tree (dpkg verifies these).
( cd "$ROOTDIR" && find . -type f -printf '%P\0' | sort -z |
    while IFS= read -r -d '' f; do md5sum "./$f" | sed 's|\./||'; done > "$CTRL/md5sums" )

# conffiles: none. postinst/postrm: update the desktop/icon/mime caches, best effort.
if [ -f "$HERE/postinst.sh" ]; then
    cp "$HERE/postinst.sh" "$CTRL/postinst"
    cp "$HERE/postinst.sh" "$CTRL/postrm"
    chmod 755 "$CTRL/postinst" "$CTRL/postrm"
fi

( cd "$CTRL" && tar --owner=0 --group=0 --format=gnu -czf "$WORK/control.tar.gz" ./* )

# ---- data.tar.gz -------------------------------------------------------------------------------
( cd "$ROOTDIR" && tar --owner=0 --group=0 --format=gnu -czf "$WORK/data.tar.gz" ./* )

# ---- debian-binary + assemble ------------------------------------------------------------------
printf '2.0\n' > "$WORK/debian-binary"

mkdir -p "$DIST"
DEB="$DIST/photocraft_${DEB_VERSION}-1_${DEB_ARCH}.deb"
rm -f "$DEB"
# The members must be in this order (debian-binary first). `ar rc` writes a System V archive, which
# dpkg accepts.
( cd "$WORK" && ar rc "$DEB" debian-binary control.tar.gz data.tar.gz )

( cd "$DIST" && sha256sum "$(basename "$DEB")" >> SHA256SUMS.txt )
echo "wrote $DEB"
ls -lh "$DEB"

# ---- verify ------------------------------------------------------------------------------------
echo "---- ar members ----"
ar t "$DEB"
if command -v dpkg-deb >/dev/null 2>&1; then
    echo "---- dpkg-deb --info ----"
    dpkg-deb --info "$DEB" || true
    echo "---- dpkg-deb --contents (first 12) ----"
    dpkg-deb --contents "$DEB" 2>/dev/null | head -12 || true
else
    echo "(dpkg-deb not installed: structure checked with ar + tar; install dpkg to run dpkg-deb --info)"
fi
