#!/bin/bash
# scripts/benchmark_mrd.sh
# Benchmarks the Data Marshal performance across a wide range of image sizes.

set -e

PORT=8085
DATA_DIR="./data_bench"

cleanup() {
    pkill -f "build/marshal" || true
    rm -rf "$DATA_DIR"
}
trap cleanup EXIT

mkdir -p "$DATA_DIR"

# Start server with 1GB limit to handle very large volumes
./build/marshal --http 127.0.0.1:$PORT --data "$DATA_DIR" --sink mrd --max-body-size 1073741824 > /dev/null 2>&1 &
sleep 2

run_bench() {
    local label=$1
    local nx=$2
    local ny=$3
    local nz=$4
    local frames=$5
    
    local voxels=$(( nx * ny * nz ))
    local bytes=$(( voxels * 4 )) 
    local total_mb=$(( bytes * frames / 1024 / 1024 ))
    
    # Print header for parsing
    echo "--- CASE: $label ---"
    echo "Dimensions: $nx x $ny x $nz"
    echo "Payload: $(( bytes / 1024 )) KB"
    
    START=$(date +%s%N)
    ./build/image_streamer --http http://127.0.0.1:$PORT --nx $nx --ny $ny --slices $nz --frames $frames --dt-ms 0 > /dev/null
    END=$(date +%s%N)
    
    DIFF_MS=$(( (END - START) / 1000000 ))
    FPS=$(awk "BEGIN {print $frames / ($DIFF_MS / 1000)}")
    MBPS=$(awk "BEGIN {print $total_mb / ($DIFF_MS / 1000)}")
    
    echo "Time: ${DIFF_MS} ms"
    echo "Speed: $FPS FPS"
    echo "Throughput: $MBPS MB/s"
}

run_bench "Scout 2D" 64 64 1 200
run_bench "Standard 2D" 128 128 1 100
run_bench "High-Res 2D" 512 512 1 50
run_bench "Ultra-Res 2D" 1024 1024 1 20
run_bench "Standard 3D" 128 128 32 20
run_bench "High-Res 3D" 256 256 64 10
run_bench "Heavy 3D" 512 512 64 5
