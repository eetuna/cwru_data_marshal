#!/bin/bash
# scripts/verify_system_integration.sh
# Verification of Task 8 features with authentic branch logic.

set -e

MRI_PORT=8080
MRI_WS=8090
ROBOT_PORT=8081
DATA_MRI="./data_verify_mri"
DATA_ROBOT="./data_verify_robot"

cleanup() {
    echo "[*] Cleaning up verification artifacts..."
    pkill -f "build/marshal" || true
    pkill -f "robot_marshal_demo" || true
    pkill -f "coordinator.py" || true
    rm -rf "$DATA_MRI" "$DATA_ROBOT"
    rm -rf "./robot_marshal_tmp_src"
    rm -f "./build/robot_marshal_demo"
    rm -f "./files.json"
}
trap cleanup EXIT

mkdir -p "$DATA_MRI" "$DATA_ROBOT"

echo "[1/5] Starting MRI Marshal (Current Branch) on $MRI_PORT..."
./build/marshal --http 127.0.0.1:$MRI_PORT --ws 127.0.0.1:$MRI_WS --data "$DATA_MRI" --sink mrd > "$DATA_MRI/server.log" 2>&1 &

echo "[2/5] Starting Robot Marshal (robot-data-marshal branch) on $ROBOT_PORT..."
# Pull and build the real robot marshal
mkdir -p ./robot_marshal_tmp_src
git show robot-data-marshal:server.cpp > ./robot_marshal_tmp_src/server.cpp
git show robot-data-marshal:httplib.h > ./robot_marshal_tmp_src/httplib.h
git show robot-data-marshal:json.hpp > ./robot_marshal_tmp_src/json.hpp
git show robot-data-marshal:circularBuffer.hpp > ./robot_marshal_tmp_src/circularBuffer.hpp
git show robot-data-marshal:files.json > ./files.json
# Patch port and allowed files
sed -i 's/server.listen("172.28.1.10", 8080);/server.listen("0.0.0.0", 8081);/g' ./robot_marshal_tmp_src/server.cpp
echo '["file1.json", "robot_status", "robot_commands"]' > ./files.json
# Build
g++ -I ./robot_marshal_tmp_src ./robot_marshal_tmp_src/server.cpp -o ./build/robot_marshal_demo -lpthread
# Start
./build/robot_marshal_demo 8081 > "$DATA_ROBOT/server.log" 2>&1 &

sleep 3

# Test Bio Ingest (MRI)
echo "[3/5] Testing Bio Signal Ingest..."
RESPONSE=$(curl -s -X POST http://127.0.0.1:$MRI_PORT/v1/bio/signal \
  -H "Content-Type: application/json" \
  -d '{"ts": "2025-12-19T12:00:00Z", "source": "ecg", "data": [0.5, 0.6], "rate_hz": 100}')

if echo "$RESPONSE" | grep -q "ok"; then
    echo "    [PASS] Bio Signal Ingested."
else
    echo "    [FAIL] Bio Signal Failed. Response: $RESPONSE"
    exit 1
fi

echo "[4/5] Starting Coordinator Bridge..."
python3 -u clients/bridge/coordinator.py > "$DATA_MRI/coordinator.log" 2>&1 &
sleep 2

# Test Halt
echo "[5/5] Testing Safety Halt Bridge..."
echo '{"error": "FAULT"}' | websocat ws://127.0.0.1:$MRI_WS/ws
sleep 4

# Verify that the Halt arrived at the real robot marshal (stored in current dir for that binary)
# The robot marshal writes to the file matching the endpoint name.
if [ -f "robot_commands" ] && grep -q "FAULT" "robot_commands"; then
    echo "    [PASS] Coordinator Halt working on authentic Robot Marshal."
else
    echo "    [FAIL] Coordinator Halt failed."
    ls -F
    exit 1
fi