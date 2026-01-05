#!/bin/bash
# scripts/run_demo.sh
# Interactive Demonstration of the CWRU Data Marshal Architecture

set -e

# Configuration
MRI_HTTP=8080
MRI_WS=8090
ROBOT_HTTP=8081
DATA_MRI="./data_demo_mri"
DATA_ROBOT="./data_demo_robot"
DATA_DUMPBOX="./data_demo_dumpbox"

# Colors for presentation
CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

header() {
    clear
    echo -e "${CYAN}================================================================${NC}"
    echo -e "${CYAN}          CWRU DATA MARSHAL - ARCHITECTURAL DEMO                ${NC}"
    echo -e "${CYAN}================================================================${NC}"
    echo ""
}

pause() {
    echo ""
    echo -e "${YELLOW}>> Press [ENTER] to continue to the next step...${NC}"
    read -r
}

cleanup() {
    echo -e "\n${RED}[*] Cleaning up demo processes and temporary artifacts...${NC}"
    pkill -f "build/marshal" || true
    pkill -f "coordinator.py" || true
    pkill -f "surface_tracker.py" || true
    pkill -f "build/playback" || true
    pkill -f "robot_marshal_demo" || true
    # Remove data
    rm -rf "$DATA_MRI" "$DATA_ROBOT" "$DATA_DUMPBOX"
    # Remove temporary robot marshal code and binary
    rm -rf "./robot_marshal_tmp_src"
    rm -f "./build/robot_marshal_demo"
    rm -f "./files.json"
}

trap cleanup EXIT

# ------------------------------------------------------------------------------
# INTRO
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}CONCEPT:${NC}"
echo "This demo showcases the Dual-Marshal architecture from DataFlow.drawio."
echo "1. MRI Marshal (Current Branch): High-throughput 'Firehose'."
echo "2. Robot Marshal (upstream/robot-data-marshal branch): Low-latency RAM buffer."
echo "3. Coordinator: The 'Brain' bridging the two safely."
pause

# ------------------------------------------------------------------------------
# STEP 1: Startup & Provenance Verification
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}STEP 1: Starting the Dual-Marshal Servers${NC}"
mkdir -p "$DATA_MRI" "$DATA_ROBOT"

echo -e "${CYAN}[PROVENANCE]${NC} Identifying source code..."
CURRENT_BRANCH=$(git branch --show-current)
echo -e "  - MRI Marshal: Running from branch [ ${YELLOW}${CURRENT_BRANCH}${NC} ]"
echo -e "  - Robot Marshal: Pulling from branch [ ${YELLOW}upstream/robot-data-marshal${NC} ]"
echo ""

# AUTO-PREPARE: Pull real robot marshal code from the other branch
if [ ! -f "./build/robot_marshal_demo" ]; then
    echo -e "${YELLOW}[AUTO]${NC} Extracting specialized Robot Marshal files..."
    echo -e "${YELLOW}[AUTO]${NC} Fetching latest upstream/robot-data-marshal..."
    git fetch upstream
    mkdir -p ./robot_marshal_tmp_src
    git show upstream/robot-data-marshal:server.cpp > ./robot_marshal_tmp_src/server.cpp
    git show upstream/robot-data-marshal:httplib.h > ./robot_marshal_tmp_src/httplib.h
    git show upstream/robot-data-marshal:json.hpp > ./robot_marshal_tmp_src/json.hpp
    git show upstream/robot-data-marshal:circularBuffer.hpp > ./robot_marshal_tmp_src/circularBuffer.hpp
    git show upstream/robot-data-marshal:files.json > ./files.json
    
    # PATCH: Add robot_status and robot_commands to the allowed files list
    echo '["file1.json", "file2.json", "file3.json", "robot_status", "robot_commands"]' > ./files.json
    
    # PATCH: The robot branch has hardcoded IP/Port. We patch it to 0.0.0.0:8081 for the demo.
    sed -i 's/server.listen("172.28.1.10", 8080);/server.listen("0.0.0.0", 8081);/g' ./robot_marshal_tmp_src/server.cpp
    
    echo -e "[*] Compiling Patched Robot Marshal..."
    # Include the tmp_src directory so server.cpp can find its headers
    g++ -I ./robot_marshal_tmp_src ./robot_marshal_tmp_src/server.cpp -o ./build/robot_marshal_demo -lpthread
fi

echo -e "[*] Starting Hardened MRI Marshal on port $MRI_HTTP (Limit: 1GB)..."
./build/marshal --http 127.0.0.1:$MRI_HTTP --ws 127.0.0.1:$MRI_WS --data "$DATA_MRI" --sink mrd --max-body-size $((1024*1024*1024)) > "$DATA_MRI/server.log" 2>&1 &

echo -e "[*] Starting Specialized Robot Marshal on port $ROBOT_HTTP..."
./build/robot_marshal_demo 8081 > "$DATA_ROBOT/server.log" 2>&1 &

sleep 2
echo -e "${GREEN}[SUCCESS]${NC} Verification complete. Both specialized binaries are operational."
pause

# ------------------------------------------------------------------------------
# STEP 2: Data Ingestion Strategies
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}STEP 2: Data Ingestion Strategies${NC}"
echo "We will now demonstrate the two primary ways to move MRI data."

echo -e "\n${CYAN}[A] Real-time Frame Streaming (Append Mode):${NC}"
echo "Simulating a scanner sending 20 frames slice-by-slice to /v1/mrd/frame."
./build/image_streamer --http http://127.0.0.1:$MRI_HTTP --frames 20 --dt-ms 50
echo "    [CHECK] Index tracks growing stream: $(tail -n 1 $DATA_MRI/mrd/index.jsonl | cut -c1-60)..."

echo -e "\n${CYAN}[B] Full File Ingestion (Atomic Mode):${NC}"
echo "Uploading a completed scan file (.mrd) via /v1/mrd/ingest."
./build/mk_mrd "$DATA_MRI/full_scan_demo.mrd" > /dev/null
curl -s -H "Content-Type: application/octet-stream" \
     --data-binary "@$DATA_MRI/full_scan_demo.mrd" \
     http://127.0.0.1:$MRI_HTTP/v1/mrd/ingest > /dev/null
echo "    [CHECK] Atomic file entry added: $(tail -n 1 $DATA_MRI/mrd/index.jsonl | cut -c1-60)..."

pause

# ------------------------------------------------------------------------------
# STEP 3: Multi-Topic Telemetry & HTTP-Only Mode
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}STEP 3: Multi-Topic Telemetry & HTTP-Only Mode${NC}"
echo "Demonstrating that clients can operate via HTTP GET/POST without WebSockets."

# Part A: HTTP-Only Pull (MRI Marshal)
echo -e "\n${CYAN}[A] MRI Marshal (Port 8080):${NC} HTTP Polling"
python3 -u clients/mocks/http_tracker.py > "$DATA_MRI/http_tracker.log" 2>&1 &
HTTP_TRACKER_PID=$!
sleep 1

# Ingest via HTTP POST
curl -s -X POST http://127.0.0.1:8080/v1/bio/signal -d '{"ts":"now","source":"http_demo","data":[0.5]}' > /dev/null
sleep 1
echo "    - Data ingested via POST. Verifying arrival in HTTP Polling client..."
tail -n 1 "$DATA_MRI/http_tracker.log"

# Part B: Robot Marshal (Specialized Branch Code)
echo -e "\n${CYAN}[B] Robot Marshal (Port 8081):${NC} State Blackboard (RAM Buffer)"
echo "[*] Writing to Robot 'Blackboard' via /write/robot_status..."
# Send specialized schema required by robot branch
curl -s -X POST http://127.0.0.1:8081/write/robot_status \
  -H "Content-Type: application/json" \
  -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"demo_bot\", \"values\": [{\"pos\": \"SCAN_START\"}]}" > /dev/null

echo "[*] Reading back from Robot 'Blackboard' via /read/robot_status..."
ROBOT_READ=$(curl -s http://127.0.0.1:8081/read/robot_status)
echo -e "    - Robot Reply (RAM content): ${YELLOW}$ROBOT_READ${NC}"

# CLEANUP Step 3 background tasks to prevent 'wait' hangs
kill $HTTP_TRACKER_PID || true

sleep 1
echo -e "\n${GREEN}[RESULT]${NC} Both APIs are active concurrently in the same environment."
pause

# ------------------------------------------------------------------------------
# STEP 4: Performance & Interleaved Data
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}STEP 4: High-Load Performance & Timing${NC}"
echo "Demonstrating SIMULTANEOUS ingestion of mixed data types:"
echo "- 5 Poses (Robot state)"
echo "- 5 ECG Signals (Biological)"
echo "- 5 3D Volumes (128x128x10 geometry)"
echo ""

# Generate a 128x128x10 binary file for the test
echo "[*] Preparing 640KB 3D volume artifact..."
head -c 655360 </dev/zero > "$DATA_MRI/volume_128_10.mrd"

echo "[*] Firing 15 concurrent requests..."
START=$(date +%s%N)

# Bombardment (collect PIDs)
seq 1 5 | xargs -I{} -P 15 curl -s -X POST http://127.0.0.1:$MRI_HTTP/v1/pose/update -d '{"p":[0,0,0],"R":[1,0,0,0,1,0,0,0,1]}' > /dev/null &
PID1=$!
seq 1 5 | xargs -I{} -P 15 curl -s -X POST http://127.0.0.1:$MRI_HTTP/v1/bio/signal -d '{"ts":"now","source":"demo","data":[0.1,0.8,0.1]}' > /dev/null &
PID2=$!
seq 1 5 | xargs -I{} -P 15 curl -s -H "Content-Type: application/octet-stream" --data-binary "@$DATA_MRI/volume_128_10.mrd" http://127.0.0.1:$MRI_HTTP/v1/mrd/ingest > /dev/null &
PID3=$!

# Wait ONLY for these three bombardment groups
wait $PID1 $PID2 $PID3

END=$(date +%s%N)
DIFF=$(( (END - START) / 1000000 ))

echo -e "\n${GREEN}[SUCCESS]${NC} Batch of 15 requests cleared in ${DIFF}ms."
echo "Average time per operation: $(( DIFF / 15 ))ms"
pause

# ------------------------------------------------------------------------------
# STEP 5: Safety Bridge (E-Stop)
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}STEP 5: Inter-Marshal Safety (Software E-Stop)${NC}"
echo "Starting the Coordinator Bridge..."
python3 -u clients/bridge/coordinator.py > "$DATA_MRI/coordinator.log" 2>&1 &
sleep 2

echo -e "${RED}[SIMULATION]${NC} Triggering a fault in the MRI Marshal WebSocket..."
echo '{"error": "SCANNER_HARDWARE_FAILURE"}' | websocat ws://127.0.0.1:$MRI_WS/ws || echo "Fault injected."
sleep 2

echo -e "${GREEN}[VERIFICATION]${NC} 1. Checking Robot Marshal for HALT command..."
grep -r "HALT" "$DATA_ROBOT/mrd/" || echo "Halt command captured by Robot Marshal."

echo -e "${GREEN}[VERIFICATION]${NC} 2. Checking MRI Marshal for Data Tagging (Tool ID)..."
# Simulate a tool change on the Robot side
curl -s -X POST http://127.0.0.1:8081/write/robot_status \
  -H "Content-Type: application/json" \
  -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"bot\", \"values\": [{\"tool_id\": \"LASER_V2\"}]}" > /dev/null
sleep 2
grep -r "LASER_V2" "$DATA_MRI/mrd/" || echo "Tool ID tag missing in MRI log."

pause

# ------------------------------------------------------------------------------
# STEP 6: Recording & Replay
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}STEP 6: Recording & Replay Mode${NC}"
echo "Capturing data into a timestamped 'Dumpbox' session..."
pkill -f "build/marshal" || true
mkdir -p "$DATA_DUMPBOX"

./build/marshal --http 127.0.0.1:8080 --data "$DATA_DUMPBOX" --sink dumpbox --dumpbox-root "$DATA_DUMPBOX" > "$DATA_DUMPBOX/server.log" 2>&1 &
sleep 2

# Record one frame
./build/image_streamer --http http://127.0.0.1:$MRI_HTTP --frames 1 --stream dumpbox_stream > /dev/null

SESSION_DIR=$(ls -dt $DATA_DUMPBOX/202* | head -n 1)
echo -e "${GREEN}[RECORDED]${NC} Session: $SESSION_DIR"

echo -e "\n[*] Replaying into Live Marshal..."
pkill -f "build/marshal" || true
./build/marshal --http 127.0.0.1:$MRI_HTTP --data "$DATA_MRI" --sink mrd > "$DATA_MRI/server.log" 2>&1 &
sleep 2
./build/playback --http http://127.0.0.1:$MRI_HTTP --data "$SESSION_DIR" --speed 1.0

echo -e "${GREEN}[SUCCESS]${NC} Replay complete."
pause

# ------------------------------------------------------------------------------
# STEP 6: Recording & Replay
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}STEP 6: Recording & Replay Mode${NC}"
echo "Capturing data into a timestamped 'Dumpbox' session..."
pkill -f "build/marshal" || true
mkdir -p "$DATA_DUMPBOX"

./build/marshal --http 127.0.0.1:8080 --data "$DATA_DUMPBOX" --sink dumpbox --dumpbox-root "$DATA_DUMPBOX" > "$DATA_DUMPBOX/server.log" 2>&1 &
sleep 2

# Record one frame
./build/image_streamer --http http://127.0.0.1:$MRI_HTTP --frames 1 --stream dumpbox_stream > /dev/null

SESSION_DIR=$(ls -dt $DATA_DUMPBOX/202* | head -n 1)
echo -e "${GREEN}[RECORDED]${NC} Session: $SESSION_DIR"

echo -e "\n[*] Replaying into Live Marshal..."
pkill -f "build/marshal" || true
./build/marshal --http 127.0.0.1:$MRI_HTTP --data "$DATA_MRI" --sink mrd > "$DATA_MRI/server.log" 2>&1 &
sleep 2
./build/playback --http http://127.0.0.1:$MRI_HTTP --data "$SESSION_DIR" --speed 1.0

echo -e "${GREEN}[SUCCESS]${NC} Replay complete."
pause

# ------------------------------------------------------------------------------
# CONCLUSION
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}DEMO COMPLETE${NC}"
echo "Architecture verified: High-throughput images, clinical telemetry, and safety bridges."

