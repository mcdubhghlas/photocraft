#!/bin/sh
# Build a PhotoCraft AppImage, or stage its AppDir when the AppImage tooling is absent
# (REWRITE_PLAN.md Z1 packaging).
#
#   packaging/linux/appimage.sh [--bin DIR] [--dist DIR] [--arch ARCH]
#
# Stages a complete AppDir (AppRun + top-level .desktop + .DirIcon + the FHS tree from
# `dist.sh --stage`), which is exactly what `appimagetool` turns into a single-file .AppImage.
# If appimagetool (and the mksquashfs it uses) are installed, it produces the .AppImage; otherwise
# it leaves the staged AppDir and a .AppDir.tar.gz artifact, prints how to finish on a host that
# has the tooling, and exits cleanly (never fails the build).
#
# No cargo, no network, no signing. Stages whatever is in --bin (default build/default/bin): run
# `make` first (the GUI `photocraft` binary must be present).
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

case $ARCH in
arm64) ARCH=aarch64 ;;
amd64) ARCH=x86_64 ;;
esac

[ -x "$BIN/photocraft" ] || { echo "appimage: $BIN/photocraft (the GUI binary) missing; run 'make' first" >&2; exit 1; }

VERSION=${PHOTOCRAFT_VERSION:-$(
    awk '/define +PC_ENGINE_VERSION/ { for (i=1;i<=NF;i++) if ($i ~ /^"/) { v=$i; gsub(/"/,"",v); print v; exit } }' \
        "$ROOT/src/engine/build_info.c"
)}
[ -n "$VERSION" ] || { echo "appimage: could not read version from src/engine/build_info.c" >&2; exit 1; }

echo "==> PhotoCraft $VERSION AppImage for $ARCH"

mkdir -p "$DIST"
APPDIR="$DIST/photocraft-$VERSION-$ARCH.AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR"

# ---- stage the FHS tree (shared with dist.sh) --------------------------------------------------
PHOTOCRAFT_BIN="$BIN" PHOTOCRAFT_ARCH="$ARCH" PHOTOCRAFT_VERSION="$VERSION" \
    sh "$HERE/dist.sh" --stage "$APPDIR" >/dev/null

# ---- AppDir top-level entries (AppImage conventions) -------------------------------------------
install -m755 "$HERE/AppRun" "$APPDIR/AppRun"
cp "$APPDIR/usr/share/applications/$APP_ID.desktop" "$APPDIR/$APP_ID.desktop"

# The top-level icon: AppImage uses .DirIcon plus a <name>.png/.svg matching the desktop Icon=.
ICON=$(find "$APPDIR/usr/share/icons" -name "$APP_ID.png" | sort | tail -1)
if [ -n "${ICON:-}" ] && [ -f "$ICON" ]; then
    cp "$ICON" "$APPDIR/$APP_ID.png"
    cp "$ICON" "$APPDIR/.DirIcon"
fi

# ---- verify the staged AppDir is runnable ------------------------------------------------------
echo "---- AppDir layout ----"
( cd "$APPDIR" && ls -1 ) | sed 's/^/  /'
echo "---- the staged CLI runs from inside the AppDir ----"
if "$APPDIR/usr/bin/photocraft-cli" version >/dev/null 2>&1; then
    "$APPDIR/usr/bin/photocraft-cli" version | sed 's/^/  /'
else
    echo "  warning: photocraft-cli did not report a version"
fi
[ -x "$APPDIR/AppRun" ] && echo "  AppRun: present and executable"
[ -f "$APPDIR/$APP_ID.desktop" ] && echo "  $APP_ID.desktop: present"
[ -f "$APPDIR/.DirIcon" ] && echo "  .DirIcon: present"

# ---- produce the .AppImage if the tooling exists, else ship the AppDir tarball -----------------
if command -v appimagetool >/dev/null 2>&1; then
    OUT="$DIST/PhotoCraft-$VERSION-$ARCH.AppImage"
    echo "---- appimagetool ----"
    if ARCH="$ARCH" appimagetool --no-appstream "$APPDIR" "$OUT"; then
        ( cd "$DIST" && sha256sum "$(basename "$OUT")" >> SHA256SUMS.txt )
        echo "wrote $OUT"
        ls -lh "$OUT"
    else
        echo "appimage: appimagetool failed; the staged AppDir remains at $APPDIR" >&2
    fi
else
    TARBALL="$DIST/photocraft-$VERSION-$ARCH.AppDir.tar.gz"
    tar -C "$DIST" -czf "$TARBALL" "$(basename "$APPDIR")"
    ( cd "$DIST" && sha256sum "$(basename "$TARBALL")" >> SHA256SUMS.txt )
    echo
    echo "appimage: appimagetool is not installed on this host, so the single-file .AppImage was not"
    echo "          produced. The complete, runnable AppDir is staged at:"
    echo "            $APPDIR"
    echo "          and archived as: $TARBALL"
    echo "          Finish on a host with appimagetool (+ mksquashfs):"
    echo "            appimagetool $APPDIR PhotoCraft-$VERSION-$ARCH.AppImage"
fi
