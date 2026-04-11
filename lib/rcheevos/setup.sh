#!/bin/bash
# Downloads rcheevos library source for MiSTer RetroAchievements integration.
# Run this script from the Main_MiSTer/lib/rcheevos/ directory.
#
# Usage: ./setup.sh [version]
#   version: git tag (default: v11.6.0)

set -e

VERSION="${1:-v11.6.0}"
REPO="https://github.com/RetroAchievements/rcheevos.git"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

if [ -d "include/rc_client.h" ] 2>/dev/null || [ -f "include/rc_client.h" ]; then
    echo "rcheevos already present. To re-download, remove include/ and src/ first."
    exit 0
fi

echo "Downloading rcheevos ${VERSION}..."

TMPDIR=$(mktemp -d)
git clone --depth 1 --branch "$VERSION" "$REPO" "$TMPDIR/rcheevos"

echo "Copying source files..."
cp -r "$TMPDIR/rcheevos/include" "$SCRIPT_DIR/include"
cp -r "$TMPDIR/rcheevos/src" "$SCRIPT_DIR/src"

rm -rf "$TMPDIR"

echo "rcheevos ${VERSION} installed to ${SCRIPT_DIR}"
echo "  include/ - headers"
echo "  src/     - source files"
echo ""
echo "Done. The Makefile will now compile rcheevos automatically."
