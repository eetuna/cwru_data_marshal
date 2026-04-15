#!/bin/bash
# scripts/run_demo.sh — Run the MRI marshal demo over MRD TCP
#
# Prerequisites: build the project first (cmake --build build)
#
# This starts:
#   1. mock_recon (python) on port 9002
#   2. marshal on HTTP 8080 + MRD TCP 9100 with dump mirroring enabled
#   3. kspace_streamer connecting to marshal over MRD TCP
#
# Press Ctrl-C to stop all.

set -e
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$PROJECT_DIR/build"
DATA_DIR="$PROJECT_DIR/data"

mkdir -p "$DATA_DIR"

cleanup() {
    echo "Stopping all processes..."
    kill $RECON_PID $MARSHAL_PID $KSPACE_PID 2>/dev/null || true
    wait 2>/dev/null || true
    echo "Done."
}
trap cleanup EXIT

echo "=== Starting mock_recon on port 9002 ==="
python3 "$PROJECT_DIR/docker/mock-recon/mock_recon.py" \
    --port 9002 &
RECON_PID=$!
sleep 1

echo "=== Starting marshal on HTTP 8080 and MRD TCP 9100 ==="
"$BUILD_DIR/marshal" \
    --http 0.0.0.0:8080 \
    --mrd-port 9100 \
    --dump-dir "$DATA_DIR" \
    --dump \
    --recon-host localhost \
    --recon-port 9002 &
MARSHAL_PID=$!
sleep 1

echo "=== Starting kspace_streamer over MRD TCP ==="
"$BUILD_DIR/kspace_streamer" \
    --host localhost \
    --port 9100 \
    --volumes 5 \
    --interval 1.0 \
    --samples 128 \
    --lines 128 \
    --slices 1 \
    --channels 1 &
KSPACE_PID=$!

echo "=== Demo running (Ctrl-C to stop) ==="
wait $KSPACE_PID 2>/dev/null || true

echo "=== kspace_streamer done, waiting 2s for reconstruction ==="
sleep 2

echo "=== Checking results ==="
echo "Live scanner files:"
ls -la "$DATA_DIR/live/from_scanner/" 2>/dev/null || echo "  (none)"
echo "Live reconstruction files:"
ls -la "$DATA_DIR/live/from_reconstruction/" 2>/dev/null || echo "  (none)"
echo "Dump scanner files:"
ls -la "$DATA_DIR/dump/from_scanner/" 2>/dev/null || echo "  (none)"
echo "Dump reconstruction files:"
ls -la "$DATA_DIR/dump/from_reconstruction/" 2>/dev/null || echo "  (none)"

echo "=== Demo complete ==="
