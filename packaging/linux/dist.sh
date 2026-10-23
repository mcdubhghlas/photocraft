#!/bin/sh
# Stage a Linux tarball of the C17 build (REWRITE_PLAN.md Z1 `make dist` seam).
#
#   packaging/linux/dist.sh [--bin DIR] [--dist DIR] [--arch ARCH]
#
# Produces $DIST/photocraft-<version>-linux-<arch>.tar.gz, a plain FHS-style tree
# (usr/bin + usr/share) plus a matching entry in $DIST/SHA256SUMS.txt. This is the
# download-and-extract fallback; the signed AppImage/.deb/.rpm/Flatpak formats are a
# remaining release-engineering item (see docs/releasing.md, still on the Rust
# packaging scripts in this directory until they are ported to the make build).
#
# No cargo, no network, no signing. It stages whatever binaries are already in --bin
# (default build/default/bin), so build them first: `make` (or `make BUILD=... all`).
# The version comes from src/engine/build_info.c (override with PHOTOCRAFT_VERSION);
# PHOTOCRAFT_BUILD_DATE (default: today, UTC) fills the AppStream <release> date.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/packaging/linux
APP_ID=ai.storyteller.photocraft

BIN=${PHOTOCRAFT_BIN:-$ROOT/build/default/bin}
DIST=${DIST:-$ROOT/dist/release}
ARCH=${PHOTOCRAFT_ARCH:-$(uname -m)}
# --stage DIR: stage the FHS tree into DIR/usr and exit (no tarball). The AppImage and .deb
# builders (packaging/linux/appimage.sh, deb.sh) reuse this so the staging logic lives in one place.
STAGE_ONLY=

while [ $# -gt 0 ]; do
    case $1 in
    --bin) BIN=$2; shift 2 ;;
    --dist) DIST=$2; shift 2 ;;
    --arch) ARCH=$2; shift 2 ;;
    --stage) STAGE_ONLY=$2; shift 2 ;;
    -h | --help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

case $ARCH in
arm64) ARCH=aarch64 ;;
esac

# Version: the single C source of truth is src/engine/build_info.c's PC_ENGINE_VERSION
# literal (the Rust reference read it from Cargo.toml; that file goes away at Z3).
VERSION=${PHOTOCRAFT_VERSION:-$(
    awk '/define +PC_ENGINE_VERSION/ { for (i=1;i<=NF;i++) if ($i ~ /^"/) { v=$i; gsub(/"/,"",v); print v; exit } }' \
        "$ROOT/src/engine/build_info.c"
)}
[ -n "$VERSION" ] || { echo "dist: could not read version from src/engine/build_info.c" >&2; exit 1; }
DATE=${PHOTOCRAFT_BUILD_DATE:-$(date -u +%Y-%m-%d)}

for b in photocraft photocraft-cli; do
    [ -x "$BIN/$b" ] || { echo "dist: $BIN/$b missing; run 'make' first" >&2; exit 1; }
done

BASENAME="photocraft-$VERSION-linux-$ARCH"
if [ -n "$STAGE_ONLY" ]; then
    # Stage straight into the caller's directory ($STAGE_ONLY/usr/...); no temp dir, no tarball.
    STAGE="$STAGE_ONLY"
    USR="$STAGE/usr"
    mkdir -p "$USR"
    echo "==> PhotoCraft $VERSION FHS tree -> $STAGE_ONLY (stage only)"
else
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/pc-dist.XXXXXX")
    trap 'rm -rf "$WORK"' EXIT INT TERM
    STAGE="$WORK/$BASENAME"
    USR="$STAGE/usr"
    echo "==> PhotoCraft $VERSION for Linux $ARCH (tar.gz)"
fi

# ---- stage an FHS tree -------------------------------------------------------------------------
install -Dm755 "$BIN/photocraft" "$USR/bin/photocraft"
install -Dm755 "$BIN/photocraft-cli" "$USR/bin/photocraft-cli"
strip "$USR/bin/photocraft" "$USR/bin/photocraft-cli" 2>/dev/null || true

install -Dm644 "$HERE/$APP_ID.desktop" "$USR/share/applications/$APP_ID.desktop"
install -Dm644 "$HERE/$APP_ID.mime.xml" "$USR/share/mime/packages/$APP_ID.xml"

mkdir -p "$USR/share/metainfo"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@DATE@/$DATE/g" \
    "$HERE/$APP_ID.metainfo.xml.in" >"$USR/share/metainfo/$APP_ID.metainfo.xml"

mkdir -p "$USR/share/icons"
cp -R "$ROOT/assets/app-icon/hicolor" "$USR/share/icons/"

mkdir -p "$USR/share/doc/photocraft"
for f in README.md LICENSE-MIT LICENSE-APACHE NOTICE ATTRIBUTION.md; do
    [ -f "$ROOT/$f" ] && cp "$ROOT/$f" "$USR/share/doc/photocraft/"
done

# ---- validate what the tools we have can check (best effort) -----------------------------------
if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$USR/share/applications/$APP_ID.desktop" || true
fi
if command -v appstreamcli >/dev/null 2>&1; then
    appstreamcli validate --no-net "$USR/share/metainfo/$APP_ID.metainfo.xml" >/dev/null 2>&1 || true
fi

# ---- stage-only: stop here (appimage.sh / deb.sh reuse the tree above) -------------------------
if [ -n "$STAGE_ONLY" ]; then
    echo "staged FHS tree at $STAGE_ONLY"
    exit 0
fi

# ---- tarball + checksum ------------------------------------------------------------------------
mkdir -p "$DIST"
tar -C "$WORK" -czf "$DIST/$BASENAME.tar.gz" "$BASENAME"
( cd "$DIST" && sha256sum "$BASENAME.tar.gz" >>"SHA256SUMS.txt" )
echo "wrote $DIST/$BASENAME.tar.gz"
ls -lh "$DIST/$BASENAME.tar.gz"
