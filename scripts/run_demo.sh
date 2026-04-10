#!/bin/bash
# scripts/run_demo.sh — Run the MRI marshal demo (v2 API)
#
# Prerequisites: build the project first (cmake --build build)
#
# This starts:
#   1. mock_recon (python) on port 9002
#   2. marshal on port 8080 with --recon-url
#   3. kspace_streamer sending data to marshal
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
    --port 9002 \
    --marshal-url http://localhost:8080 &
RECON_PID=$!
sleep 1

echo "=== Starting marshal on port 8080 ==="
"$BUILD_DIR/marshal" \
    --http 0.0.0.0:8080 \
    --dump-dir "$DATA_DIR" \
    --recon-url http://localhost:9002 &
MARSHAL_PID=$!
sleep 1

echo "=== Starting kspace_streamer ==="
"$BUILD_DIR/kspace_streamer" \
    --http http://localhost:8080 \
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
echo "Scanner files:"
ls -la "$DATA_DIR/from_scanner/" 2>/dev/null || echo "  (none)"
echo "Recon files:"
ls -la "$DATA_DIR/from_reconstruction/" 2>/dev/null || echo "  (none)"

echo "=== Demo complete ==="
