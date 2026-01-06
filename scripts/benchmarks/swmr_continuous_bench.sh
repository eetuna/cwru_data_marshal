#!/bin/bash
# scripts/benchmarks/swmr_continuous_bench.sh
# SWMR Continuous Write Benchmark
# Tests sustained /v1/mrd/frame writes at 192x192x15 @ 10ms intervals (100 fps)

HTTP_PORT=8086
WS_PORT=8096
DATA_DIR="./data_swmr_bench"
FRAME_SIZE=192
NSLICES=15
INTERVAL_MS=10  # 100 fps target
DURATION_SEC=30  # 30 second sustained load

cleanup() {
    pkill -f "build/marshal.*$HTTP_PORT" 2>/dev/null || true
    pkill -f "build/image_streamer.*$HTTP_PORT" 2>/dev/null || true
    rm -rf "$DATA_DIR" 2>/dev/null || true
}

trap cleanup EXIT

mkdir -p "$DATA_DIR"
./build/marshal --http 127.0.0.1:$HTTP_PORT \
                --ws 127.0.0.1:$WS_PORT \
                --data "$DATA_DIR" \
                --flush-frames 1 > "$DATA_DIR/marshal.log" 2>&1 &
MARSHAL_PID=$!
sleep 2

if ! curl -s --max-time 2 http://127.0.0.1:$HTTP_PORT/health > /dev/null 2>&1; then
    echo "Marshal failed to start"
    exit 1
fi

echo "============================================================"
echo "  SWMR Continuous Write Benchmark"
echo "  192x192x15 @ ${INTERVAL_MS}ms intervals ($(awk "BEGIN {printf \"%.0f\", 1000/$INTERVAL_MS}") fps)"
echo "  Duration: ${DURATION_SEC}s"
echo "============================================================"
echo ""

EXPECTED_FRAMES=$((DURATION_SEC * 1000 / INTERVAL_MS))
echo "Expected frames: $EXPECTED_FRAMES"
echo "Starting continuous SWMR write test..."
echo ""

start_time=$(date +%s%N)

./build/image_streamer --http http://127.0.0.1:$HTTP_PORT \
                       --frames $EXPECTED_FRAMES \
                       --dt-ms $INTERVAL_MS \
                       --size $FRAME_SIZE \
                       --nslices $NSLICES 2>&1 | tee /tmp/swmr_bench.log &
STREAMER_PID=$!

# Monitor progress
echo "Progress:"
while kill -0 $STREAMER_PID 2>/dev/null; do
    frame_count=$(grep -c "^frame" /tmp/swmr_bench.log 2>/dev/null || echo 0)
    elapsed=$(( ($(date +%s%N) - start_time) / 1000000000 ))
    if [ $elapsed -gt 0 ]; then
        actual_fps=$(awk "BEGIN {printf \"%.1f\", $frame_count / $elapsed}")
    else
        actual_fps="0"
    fi
    echo "  [$elapsed/$DURATION_SEC sec] Frames: $frame_count | FPS: $actual_fps"
    sleep 2
done

wait $STREAMER_PID
end_time=$(date +%s%N)

elapsed_sec=$(awk "BEGIN {printf \"%.2f\", ($(date +%s%N) - $start_time) / 1000000000}")
frame_count=$(grep -c "^frame" /tmp/swmr_bench.log 2>/dev/null || echo 0)
actual_fps=$(awk "BEGIN {printf \"%.2f\", $frame_count / $elapsed_sec}")

frame_bytes=$((FRAME_SIZE * FRAME_SIZE * NSLICES * 4))
throughput=$(awk "BEGIN {printf \"%.2f\", $frame_bytes * $actual_fps / 1048576}")

file_count=$(ls -1 "$DATA_DIR/mrd/"*.mrd 2>/dev/null | wc -l)
disk_usage=$(du -sh "$DATA_DIR/mrd/" 2>/dev/null | cut -f1)

echo ""
echo "============================================================"
echo "  Results"
echo "============================================================"
echo "  Duration:       ${elapsed_sec}s"
echo "  Frames:         $frame_count / $EXPECTED_FRAMES"
echo "  FPS:            $actual_fps"
echo "  Throughput:     ${throughput} MB/s"
echo "  HDF5 files:     $file_count"
echo "  Disk usage:     $disk_usage"
echo ""

success_pct=$(awk "BEGIN {printf \"%.1f\", $frame_count * 100 / $EXPECTED_FRAMES}")
echo "  Success rate:   ${success_pct}%"
