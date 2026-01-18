#!/bin/bash
set -e

OUT_DIR=${1:-./exported_images}
mkdir -p "$OUT_DIR"

COMPOSE="docker compose"
if ! $COMPOSE version >/dev/null 2>&1; then
  COMPOSE="docker-compose"
fi

$COMPOSE build mri-marshal robot-marshal

MRI_IMAGE="cwru/mri-marshal:latest"
ROBOT_IMAGE="cwru/robot-marshal:latest"

docker save -o "$OUT_DIR/mri-marshal.tar" "$MRI_IMAGE"
docker save -o "$OUT_DIR/robot-marshal.tar" "$ROBOT_IMAGE"

echo "Saved images to $OUT_DIR"
