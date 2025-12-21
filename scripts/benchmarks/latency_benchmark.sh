#!/bin/bash
# scripts/latency_benchmark.sh
# Measures precise latency for SWMR frames vs. Full File Ingest.

set -e

PORT=8085
DATA_DIR="./data_latency_bench"

cleanup() {
    pkill -f "build/marshal" || true
    rm -rf "$DATA_DIR"
}
trap cleanup EXIT

mkdir -p "$DATA_DIR"

# Start server
./build/marshal --http 127.0.0.1:$PORT --data "$DATA_DIR" --sink mrd --max-body-size 1073741824 > /dev/null 2>&1 &
sleep 2

measure_swmr() {
    local nx=$1
    local nz=$2
    ./build/image_streamer --http http://127.0.0.1:$PORT --nx $nx --ny $nx --slices $nz --frames 1 --dt-ms 0 > /dev/null
}

measure_full() {
    local nx=$1
    local nz=$2
    local bytes=$(( nx * nx * nz * 4 ))
    local out="$DATA_DIR/bench_${nx}_${nz}.mrd"
    head -c $bytes </dev/zero > "$out"
    curl -s -o /dev/null -X POST http://127.0.0.1:$PORT/v1/mrd/ingest -H "Content-Type: application/octet-stream" --data-binary "@$out"
}

get_ms() {
    local label=$1
    local type=$2
    local nx=$3
    local nz=$4
    
    local start=$(date +%s%N)
    if [ "$type" == "swmr" ]; then measure_swmr $nx $nz; else measure_full $nx $nz; fi
    local end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

echo "================================================================"
echo "          EXPANDED LATENCY BENCHMARK (ms per request)           "
echo "================================================================"
printf "| %-7s | %-6s | %-12s | %-12s |\n" "Res" "Slices" "SWMR Latency" "Bulk Latency"
echo "----------------------------------------------------------------"

for res in 128 192 256; do
    for slices in 1 2 5 10 12 15 18 20; do
        L_SWMR=$(get_ms "$res" "swmr" $res $slices)
        L_FULL=$(get_ms "$res" "full" $res $slices)
        printf "| %-7s | %-6s | %-12s | %-12s |\n" "${res}x${res}" "$slices" "${L_SWMR} ms" "${L_FULL} ms"
    done
    echo "----------------------------------------------------------------"
done