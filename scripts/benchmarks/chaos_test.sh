#!/bin/bash
# scripts/chaos_test.sh
# Exhaustive Stress Test: Interleaved high-frequency data ingestion.

PORT=8089
WS_PORT=8099
DATA_DIR="./data_chaos"

cleanup() {
    echo -e "\n[*] Stopping processes..."
    pkill -f "build/marshal" || true
    rm -rf "$DATA_DIR"
}
trap cleanup EXIT

mkdir -p "$DATA_DIR"

echo "=== Chaos Test: Parallel Interleaved Ingestion ==="

# Start server
./build/marshal --http 127.0.0.1:$PORT --ws 127.0.0.1:$WS_PORT --data "$DATA_DIR" --sink mrd --max-body-size 1073741824 > /dev/null 2>&1 &
sleep 2

echo "[1/4] Starting Parallel Bombardment..."

# 1. Pose Updates
(
    for i in {1..50}; do
        curl -s -X POST http://127.0.0.1:$PORT/v1/pose/update -d '{"p":[0,0,0],"R":[1,0,0,0,1,0,0,0,1]}' > /dev/null
    done
    echo "    [DONE] Pose Stream."
) &
P1=$!

# 2. Bio Signals
(
    for i in {1..50}; do
        curl -s -X POST http://127.0.0.1:$PORT/v1/bio/signal -d '{"ts":"2025-12-19T12:00:00.000Z","source":"stress","data":[0.1],"rate_hz":10}' > /dev/null
    done
    echo "    [DONE] Bio Stream."
) &
P2=$!

# 3. Bulk File Ingest
(
    ./build/mk_mrd "$DATA_DIR/stress.mrd" > /dev/null
    for i in {1..5}; do
        curl -s -H "Content-Type: application/octet-stream" --data-binary "@$DATA_DIR/stress.mrd" http://127.0.0.1:$PORT/v1/mrd/ingest > /dev/null
    done
    echo "    [DONE] Bulk File Ingest."
) &
P3=$!

wait $P1 $P2 $P3

echo "[2/4] Verifying Data Integrity..."

# Use more reliable counting
POSE_COUNT=$(cat "$DATA_DIR/mrd/poses.jsonl" 2>/dev/null | wc -l || echo 0)
BIO_COUNT=$(cat "$DATA_DIR/mrd/bio.jsonl" 2>/dev/null | wc -l || echo 0)
FILE_COUNT=$(grep -c "type" "$DATA_DIR/mrd/index.jsonl" 2>/dev/null || echo 0)

echo "    - Poses captured: $POSE_COUNT"
echo "    - Bio signals captured: $BIO_COUNT"
echo "    - Bulk files captured: $FILE_COUNT"

if [ "$POSE_COUNT" -lt 50 ]; then echo "FAIL: Pose count $POSE_COUNT < 50"; exit 1; fi
if [ "$BIO_COUNT" -lt 50 ]; then echo "FAIL: Bio count $BIO_COUNT < 50"; exit 1; fi
if [ "$FILE_COUNT" -lt 5 ]; then echo "FAIL: File count $FILE_COUNT < 5"; exit 1; fi

echo -e "\n[3/4] Checking for Server Stability..."
if ps aux | grep -v grep | grep -q "build/marshal"; then
    echo "    [PASS] Server is still alive."
else
    echo "    [FAIL] Server crashed under load."
    exit 1
fi

echo -e "\n=== CHAOS STRESS TEST PASSED ==="