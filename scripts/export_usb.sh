#!/bin/bash
#
# USB Export — portable deployment of the CWRU Data Marshal stack.
#
# Packages the runtime Docker images + docker-compose.yml + a receiver README
# into a directory you copy to a USB drive. The receiving machine just runs
# `docker load` then `docker compose up -d`.
#
# Usage:
#   ./scripts/export_usb.sh <output_directory>
# Example:
#   ./scripts/export_usb.sh /media/usb/cwru_marshal
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ $# -ne 1 ]; then
    echo "Usage: $0 <output_directory>"
    exit 1
fi
OUT_DIR="$1"
if [ -e "$OUT_DIR" ]; then
    echo "Error: output directory already exists: $OUT_DIR"
    exit 1
fi
cd "$PROJECT_ROOT"

# The 5 images the single-file stack needs.
IMAGES=(
    cwru/mri-marshal:latest
    cwru/robot-marshal:latest
    cwru/robot-clients:latest
    cwru/webgl-client:latest
    fire-python:latest
)

echo "[1/4] Checking images are built..."
for img in "${IMAGES[@]}"; do
    docker image inspect "$img" >/dev/null 2>&1 \
        || { echo "  ✗ missing: $img  (build it first)"; exit 1; }
    echo "  ✓ $img"
done

mkdir -p "$OUT_DIR"

echo "[2/4] Saving images to tar (several minutes)..."
docker save -o "$OUT_DIR/cwru-images.tar" "${IMAGES[@]}"
echo "  ✓ cwru-images.tar ($(du -h "$OUT_DIR/cwru-images.tar" | cut -f1))"

echo "[3/4] Copying compose file..."
cp docker-compose.yml "$OUT_DIR/"

echo "[4/4] Writing receiver README..."
cat > "$OUT_DIR/README.md" <<'EOF'
# CWRU Data Marshal — portable deployment

## 1. Load images
    docker load -i cwru-images.tar
    docker images | grep -E "cwru|fire-python"

## 2. Run
    mkdir -p session-data
    docker compose up -d                       # live mode
    # MARSHAL_DUMP=--dump docker compose up -d   # dump/archival mode
    docker compose ps                          # wait until healthy

- WebGL UI:  http://<host>:3000
- MRI HTTP:  http://<host>:8080   (/image/latest, /health)
- MRD TCP:   <host>:9100   <-- point the scanner here
- Recon:     python-ismrmrd-server (invertcontrast); used only for k-space.
             Scanner-sent images pass straight through to webgl.

## 3. Feed data without a real scanner (optional)
    docker run --rm -v "$PWD/session-data:/data" fire-python:latest \
      python3 generate_cartesian_shepp_logan_dataset.py -o /data/phantom.h5

    docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" \
      fire-python:latest python3 client.py -c invertcontrast -o /data/out.h5 \
      --address mri-marshal --port 9100 /data/phantom.h5

## Requirements
- amd64 Linux host, Docker Engine 20.10+ / Compose v2.
- Ports 3000, 3001, 8080, 8081, 9100 free.

## Teardown
    docker compose down
EOF

echo ""
echo "Done. Package: $OUT_DIR ($(du -sh "$OUT_DIR" | cut -f1))"
echo "Copy it to USB. On the target: docker load -i cwru-images.tar && docker compose up -d"
