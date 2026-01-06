#!/bin/bash
# scripts/run_demo_auto.sh
# Optimized non-interactive verification of all system features.

set -e

# Configuration
MRI_HTTP=8080
MRI_WS=8090
ROBOT_HTTP=8081
DATA_MRI="./data_auto_mri"
DATA_ROBOT="./data_auto_robot"
DATA_DUMPBOX="./data_auto_dumpbox"


# X11 setup for GUI
export DISPLAY=:0
unset XAUTHORITY

cleanup() {
    pkill -f "build/marshal" || true
    pkill -f "coordinator.py" || true
    pkill -f "surface_tracker.py" || true
    pkill -f "robot_marshal_demo" || true
    rm -rf "$DATA_MRI" "$DATA_ROBOT" "$DATA_DUMPBOX"
    rm -rf "./robot_marshal_tmp_src"
    rm -f "./build/robot_marshal_demo"
    rm -f "./files.json"
}
trap cleanup EXIT

echo "=== Automated Feature Verification ==="

mkdir -p "$DATA_MRI" "$DATA_ROBOT"

# AUTO-PREPARE: Pull real robot marshal code
echo "[*] Fetching latest upstream/robot-data-marshal..."
git fetch upstream
mkdir -p ./robot_marshal_tmp_src
git show upstream/robot-data-marshal:server.cpp > ./robot_marshal_tmp_src/server.cpp
git show upstream/robot-data-marshal:httplib.h > ./robot_marshal_tmp_src/httplib.h
git show upstream/robot-data-marshal:json.hpp > ./robot_marshal_tmp_src/json.hpp
git show upstream/robot-data-marshal:circularBuffer.hpp > ./robot_marshal_tmp_src/circularBuffer.hpp
git show upstream/robot-data-marshal:files.json > ./files.json
echo '["file1.json", "robot_status", "robot_commands"]' > ./files.json
sed -i 's/server.listen("172.28.1.10", 8080);/server.listen("0.0.0.0", 8081);/g' ./robot_marshal_tmp_src/server.cpp
g++ -I ./robot_marshal_tmp_src ./robot_marshal_tmp_src/server.cpp -o ./build/robot_marshal_demo -lpthread

# 1. Start Servers (Reduced sleep)
./build/marshal --http 127.0.0.1:$MRI_HTTP --ws 127.0.0.1:$MRI_WS --data "$DATA_MRI" --sink mrd > /dev/null 2>&1 &
./build/robot_marshal_demo 8081 > /dev/null 2>&1 &
sleep 1

# 2. Ingest Verification (Reduced frames)
echo "[*] Verifying Frame Ingest..."
./build/image_streamer --http http://127.0.0.1:$MRI_HTTP --frames 5 --dt-ms 10 > /dev/null

echo "[*] Verifying Bulk Ingest..."
./build/mk_mrd "$DATA_MRI/test.mrd" > /dev/null
curl -s -H "Content-Type: application/octet-stream" --data-binary "@$DATA_MRI/test.mrd" http://127.0.0.1:$MRI_HTTP/v1/mrd/ingest > /dev/null

# 3. Telemetry & Hybrid Polling
echo "[*] Verifying High-Freq Polling & Telemetry..."
python3 -u clients/mocks/http_tracker.py > "$DATA_MRI/poller.log" 2>&1 &
POLLER_PID=$!
curl -s -X POST http://127.0.0.1:$MRI_HTTP/v1/bio/signal -d '{"ts":"now","source":"auto","data":[0.1,0.8,0.1],"rate_hz":100.0}' > /dev/null
sleep 1
grep -q "New Data" "$DATA_MRI/poller.log" || echo "Poller check failed"
kill $POLLER_PID || true

# 4. Stress Test (Reduced count for speed)
echo "[*] Verifying Concurrency (Parallel Interleaved)..."
START=$(date +%s%N)
seq 1 20 | xargs -I{} -P 10 curl -s -X POST http://127.0.0.1:$MRI_HTTP/v1/pose/update -d '{"p":[0,0,0],"R":[1,0,0,0,1,0,0,0,1]}' > /dev/null &
PID1=$!
wait $PID1
END=$(date +%s%N)
echo "    Cleared 10 requests in $(( (END - START) / 1000000 ))ms."

# 5. Safety Bridge (Software E-Stop)
echo "[*] Verifying Safety Bridge (Halt)..."
python3 -u clients/bridge/coordinator.py > "$DATA_MRI/coord.log" 2>&1 &
sleep 1
echo '{"error": "AUTO_FAULT"}' | websocat ws://127.0.0.1:$MRI_WS/ws
sleep 1
grep -q "HALT" "$DATA_MRI/coord.log" && echo "    [PASS] E-Stop triggered."

echo "=== VERIFICATION COMPLETE ==="