#!/bin/bash
# scripts/benchmarks/mri_marshal_stress_test.sh
# MRI Marshal Stress Test - Benchmark /v1/mrd/frame and /v1/mrd/ingest
#
# Tests 192x192x15 frame throughput

# Configuration
HTTP_PORT=8085
WS_PORT=8095
DATA_DIR="./data_stress_test"
FRAME_SIZE=192
NSLICES=15

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

cleanup() {
    pkill -f "build/marshal.*$HTTP_PORT" 2>/dev/null || true
    pkill -f "build/image_streamer.*$HTTP_PORT" 2>/dev/null || true
    rm -rf "$DATA_DIR" 2>/dev/null || true
}

trap cleanup EXIT

start_marshal() {
    mkdir -p "$DATA_DIR"
    ./build/marshal --http 127.0.0.1:$HTTP_PORT \
                    --ws 127.0.0.1:$WS_PORT \
                    --data "$DATA_DIR" \
                    --flush-frames 1 > "$DATA_DIR/marshal.log" 2>&1 &
    MARSHAL_PID=$!
    sleep 2
    if ! curl -s --max-time 2 http://127.0.0.1:$HTTP_PORT/health > /dev/null 2>&1; then
        echo -e "${RED}ERROR: Marshal failed to start${NC}"
        exit 1
    fi
}

# Generate test frame (ISMRMRD header + float32 data)
generate_frame() {
    local outfile=$1
    local pixels=$((FRAME_SIZE * FRAME_SIZE * NSLICES))
    local data_bytes=$((pixels * 4))
    dd if=/dev/urandom of="$outfile" bs=$((340 + data_bytes)) count=1 2>/dev/null
}

echo "============================================================"
echo "  MRI MARSHAL STRESS TEST"
echo "  Frame: ${FRAME_SIZE}x${FRAME_SIZE}x${NSLICES} float32"
echo "============================================================"

# Check binaries
if [ ! -f "./build/marshal" ] || [ ! -f "./build/image_streamer" ]; then
    echo -e "${RED}ERROR: Build binaries first with 'make'${NC}"
    exit 1
fi

cleanup
start_marshal
echo "Marshal started (PID: $MARSHAL_PID)"

# ============================================================================
# TEST 1: /v1/mrd/frame via image_streamer (SWMR mode)
# ============================================================================
echo ""
echo "--- TEST 1: /v1/mrd/frame (SWMR) via image_streamer ---"

FRAME_COUNT=50
start_time=$(date +%s%N)

./build/image_streamer --http http://127.0.0.1:$HTTP_PORT \
                       --frames $FRAME_COUNT \
                       --dt-ms 50 \
                       --size $FRAME_SIZE \
                       --nslices $NSLICES 2>&1 | grep -E "^frame (0|$((FRAME_COUNT-1)))" || true

end_time=$(date +%s%N)
elapsed_ms=$(( (end_time - start_time) / 1000000 ))
fps=$(awk "BEGIN {printf \"%.2f\", $FRAME_COUNT * 1000 / $elapsed_ms}")
frame_bytes=$((FRAME_SIZE * FRAME_SIZE * NSLICES * 4))
throughput=$(awk "BEGIN {printf \"%.2f\", $frame_bytes * $fps / 1048576}")

echo "  Frames: $FRAME_COUNT | Time: ${elapsed_ms}ms | FPS: $fps | Throughput: ${throughput} MB/s"

# ============================================================================
# TEST 2: /v1/mrd/ingest (full file write)
# ============================================================================
echo ""
echo "--- TEST 2: /v1/mrd/ingest (full file per frame) ---"

test_frame="/tmp/test_frame_$$.bin"
generate_frame "$test_frame"
frame_size=$(stat -c%s "$test_frame")

INGEST_COUNT=20
start_time=$(date +%s%N)
success=0

for i in $(seq 1 $INGEST_COUNT); do
    http_code=$(curl -s -o /dev/null -w "%{http_code}" -X POST \
        -H "Content-Type: application/octet-stream" \
        --data-binary @"$test_frame" \
        http://127.0.0.1:$HTTP_PORT/v1/mrd/ingest)
    if [ "$http_code" = "200" ] || [ "$http_code" = "201" ]; then
        success=$((success + 1))
    fi
done

end_time=$(date +%s%N)
elapsed_ms=$(( (end_time - start_time) / 1000000 ))
fps=$(awk "BEGIN {printf \"%.2f\", $success * 1000 / $elapsed_ms}")
throughput=$(awk "BEGIN {printf \"%.2f\", $frame_size * $fps / 1048576}")

rm -f "$test_frame"

echo "  Frames: $success/$INGEST_COUNT | Time: ${elapsed_ms}ms | FPS: $fps | Throughput: ${throughput} MB/s"

# ============================================================================
# TEST 3: Read throughput (GET /v1/mrd/latest)
# ============================================================================
echo ""
echo "--- TEST 3: GET /v1/mrd/latest (read throughput) ---"

READ_COUNT=100
start_time=$(date +%s%N)

for i in $(seq 1 $READ_COUNT); do
    curl -s http://127.0.0.1:$HTTP_PORT/v1/mrd/latest > /dev/null
done

end_time=$(date +%s%N)
elapsed_ms=$(( (end_time - start_time) / 1000000 ))
rps=$(awk "BEGIN {printf \"%.2f\", $READ_COUNT * 1000 / $elapsed_ms}")
avg_latency=$(awk "BEGIN {printf \"%.2f\", $elapsed_ms / $READ_COUNT}")

echo "  Requests: $READ_COUNT | Time: ${elapsed_ms}ms | RPS: $rps | Avg latency: ${avg_latency}ms"

# ============================================================================
# Summary
# ============================================================================
echo ""
echo "============================================================"
echo "  SUMMARY"
echo "============================================================"
file_count=$(ls -1 "$DATA_DIR/mrd/"*.mrd 2>/dev/null | wc -l)
disk_usage=$(du -sh "$DATA_DIR/mrd/" 2>/dev/null | cut -f1)
echo "  HDF5 files created: $file_count"
echo "  Disk usage: $disk_usage"
echo -e "  ${GREEN}Tests completed${NC}"
