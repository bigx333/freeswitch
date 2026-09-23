#!/bin/bash
# Build the core and the modules the Opus/RTP unit tests need, then run those
# tests, inside the Buster dependency image (the Dockerfile's "deps" stage).
#
#   ./test.sh                               # switch_opus switch_rtp switch_core_codec
#   ./test.sh switch_opus                   # a subset
#
# Works on a scratch clone of the committed HEAD in .build/test-src, which is
# kept between runs so rebuilds are incremental. Uncommitted changes are not
# tested; commit first.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(git rev-parse --show-toplevel)
IMAGE=fs-buster-deps
TESTS=("$@")
[ ${#TESTS[@]} -gt 0 ] || TESTS=(switch_opus switch_rtp switch_core_codec)

sudo docker buildx build --progress=quiet --target deps --load -t "$IMAGE" .

SRC=.build/test-src
if [ -d "$SRC/.git" ]; then
  sudo git -c safe.directory="*" -C "$SRC" fetch -q "file://$ROOT" HEAD
  sudo git -c safe.directory="*" -C "$SRC" checkout -q -f FETCH_HEAD
else
  mkdir -p .build
  git clone -q --no-local "file://$ROOT" "$SRC"
fi

sudo docker run --rm -v "$PWD/$SRC:/fs" -v "$PWD/run-unit-tests.sh:/run-unit-tests.sh:ro" \
  "$IMAGE" bash /run-unit-tests.sh "${TESTS[@]}"
