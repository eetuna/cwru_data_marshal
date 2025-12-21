#!/bin/bash
# scripts/extensive_benchmark.sh
# Performs a grid search of performance across resolutions and slice counts.

set -e

PORT=8085
DATA_DIR="./data_extensive_bench"

cleanup() {
    pkill -f "build/marshal" || true
    rm -rf "$DATA_DIR"
}
trap cleanup EXIT

mkdir -p "$DATA_DIR"

# Start server with high limit
./build/marshal --http 127.0.0.1:$PORT --data "$DATA_DIR" --sink mrd --max-body-size 1073741824 > /dev/null 2>&1 &
sleep 2

run_case() {
    local nx=$1
    local ny=$1
    local nz=$2
    local frames=30
    
    local voxels=$(( nx * ny * nz ))
    local bytes=$(( voxels * 4 )) 
    local total_mb=$(( bytes * frames / 1024 / 1024 ))
    
    START=$(date +%s%N)
    ./build/image_streamer --http http://127.0.0.1:$PORT --nx $nx --ny $ny --slices $nz --frames $frames --dt-ms 0 > /dev/null
    END=$(date +%s%N)
    
    DIFF_MS=$(( (END - START) / 1000000 ))
    if [ $DIFF_MS -eq 0 ]; then DIFF_MS=1; fi # Avoid div by zero
    
    FPS=$(awk "BEGIN {print $frames / ($DIFF_MS / 1000)}")
    MBPS=$(awk "BEGIN {print $total_mb / ($DIFF_MS / 1000)}")
    
    printf "| %-7s | %-6s | %-10s | %-10s | %-10s |\n" "${nx}x${ny}" "$nz" "$((bytes/1024)) KB" "$FPS" "$MBPS"
}

echo "========================================================================"
echo "          EXTENSIVE PERFORMANCE MATRIX (Grid Search)                    "
echo "========================================================================"
printf "| %-7s | %-6s | %-10s | %-10s | %-10s |\n" "Res" "Slices" "Payload" "FPS" "MB/s"
echo "------------------------------------------------------------------------"

for res in 64 128 192 256 384 512; do
    for slices in 5 10 15 20; do
        run_case $res $slices
    done
    echo "------------------------------------------------------------------------"
done

echo "Benchmark grid complete."
