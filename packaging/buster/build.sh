#!/bin/bash
# Build FreeSWITCH Debian packages for Debian Buster from the committed HEAD
# of this repository. Output lands in ./debs/ (dependency + FreeSWITCH .debs).
#
#   ./build.sh                      # full build
#   ./build.sh --target deps-out    # dependency .debs only
#   PKG_VERSION=1.10.12-5~buster1 ./build.sh
#   ./build.sh --bundle             # also write dist/<name>.tar.gz for transfer
#
# Uncommitted changes are NOT included; commit first.
set -euo pipefail
cd "$(dirname "$0")"
HERE=$PWD
ROOT=$(git rev-parse --show-toplevel)

TARGET=export
BUNDLE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --target) TARGET=$2; shift 2 ;;
    --bundle) BUNDLE=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

PKG_VERSION=${PKG_VERSION:-1.10.12-5~buster1}
COMMIT=$(git -C "$ROOT" rev-parse --short=10 HEAD)
if [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]; then
  echo "warning: uncommitted changes are not part of the build (building $COMMIT)" >&2
fi

./fetch-libs.sh

rm -rf .build && mkdir -p .build
git clone -q --no-local --depth 1 "file://$ROOT" .build/freeswitch

mkdir -p debs
sudo docker buildx build --progress=plain \
  --target "$TARGET" \
  --build-arg PKG_VERSION="$PKG_VERSION" \
  --build-arg PKG_CHANGELOG="Rebuild for Debian Buster from $(git -C "$ROOT" rev-parse --abbrev-ref HEAD) @ $COMMIT" \
  --output type=local,dest=./debs \
  .

echo "=== Built packages ==="
ls -la debs/

if [ "$BUNDLE" = 1 ]; then
  NAME="freeswitch-${PKG_VERSION//\~/-}-$COMMIT"
  rm -rf "dist/$NAME" && mkdir -p "dist/$NAME"
  cp debs/*.deb "dist/$NAME/"
  sed -e "s/@PKG_VERSION@/$PKG_VERSION/g" -e "s/@COMMIT@/$COMMIT/g" -e "s/@DATE@/$(date -u +%F)/g" \
    INSTALL.txt.in > "dist/$NAME/INSTALL.txt"
  (cd "dist/$NAME" && sha256sum *.deb > SHA256SUMS)
  tar -C dist -czf "dist/$NAME.tar.gz" "$NAME"
  echo "=== Bundle: $HERE/dist/$NAME.tar.gz"
fi
