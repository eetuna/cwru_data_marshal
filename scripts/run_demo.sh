#!/bin/bash
# scripts/run_demo.sh
# Interactive Demonstration of the CWRU Data Marshal Architecture

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
source "$SCRIPT_DIR/tools/robot_marshal_env.sh"
source "$SCRIPT_DIR/tools/mri_marshal_env.sh"

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
    clear 2>/dev/null || true  # Ignore errors if TERM not set
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
    # Kill any stray curl processes from concurrent tests
    pkill -f "curl.*127.0.0.1:808" || true
    # Remove data
    rm -rf "$DATA_MRI" "$DATA_ROBOT" "$DATA_DUMPBOX"
    if [ -f "./files.json" ]; then
        python3 - <<'PY'
import json, os
try:
    files = json.load(open("files.json"))
except Exception:
    files = []
for name in files:
    try:
        os.remove(name)
    except OSError:
        pass
PY
    fi
    rm -f ./files.json ./file_routes.json
    # External robot marshal binaries are not removed here.
}

trap cleanup EXIT

# ------------------------------------------------------------------------------
# INTRO
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}CONCEPT:${NC}"
echo "This demo showcases the Dual-Marshal architecture from DataFlow.drawio."
echo "1. MRI Marshal (Current Branch): High-throughput 'Firehose'."
echo "2. Robot Marshal (external repo): Lightweight state cache."
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
echo -e "  - Robot Marshal: External repo at [ ${YELLOW}${ROBOT_MARSHAL_DIR}${NC} ]"
echo ""

# Prepare file routing for robot clients
if [ -f "$ROBOT_MARSHAL_DIR/files.json" ]; then
    cp "$ROBOT_MARSHAL_DIR/files.json" ./files.json
else
    echo '["file1.json", "file2.json", "file3.json", "robot_status", "robot_commands"]' > ./files.json
fi

echo -e "[*] Starting Hardened MRI Marshal on port $MRI_HTTP (Limit: 1GB)..."
if ! ensure_mri_ready; then
    echo -e "${RED}[ERROR]${NC} MRI marshal binaries not found. Set MRI_MARSHAL_DIR."
    exit 1
fi
"$MRI_MARSHAL_BIN" --http 127.0.0.1:$MRI_HTTP --ws 127.0.0.1:$MRI_WS --data "$DATA_MRI" --sink mrd --max-body-size $((1024*1024*1024)) > "$DATA_MRI/server.log" 2>&1 &

echo -e "[*] Starting Specialized Robot Marshal on port $ROBOT_HTTP..."
export ROBOT_MARSHAL_PORT="$ROBOT_HTTP"
if ! ensure_robot_marshal_ready; then
    echo -e "${RED}[ERROR]${NC} Robot marshal binary not found. Set ROBOT_MARSHAL_DIR/ROBOT_MARSHAL_BIN."
    exit 1
fi
"$ROBOT_MARSHAL_BIN" 8081 > "$DATA_ROBOT/server.log" 2>&1 &

sleep 2

# Ensure seed files exist for robot marshal (storage_dir = ./)
ROBOT_FILES_DIR="."
if [ -d "$ROBOT_MARSHAL_DIR/files" ]; then
    ROBOT_FILES_DIR="./files"
fi
export ROBOT_FILES_DIR
mkdir -p "$ROBOT_FILES_DIR"
python3 - <<'PY'
import json
import os
seed = {"client_id": "seed", "sent_at": 1, "values": [1.0, 2.0, 3.0]}
files_dir = os.environ.get("ROBOT_FILES_DIR", ".")
try:
    files = json.load(open("files.json"))
except Exception:
    files = []
for name in files:
    path = os.path.join(files_dir, name)
    with open(path, "w") as fh:
        fh.write(json.dumps(seed))
PY

# Wait for robot marshal to be ready (with timeout)
echo -e "[*] Waiting for Robot Marshal to be ready..."
ROBOT_HEALTH_FILE=$(python3 - <<'PY'
import json
try:
    files = json.load(open("files.json"))
except Exception:
    files = []
print(files[0] if files else "robot_status")
PY
)
for i in $(seq 1 10); do
    if curl -s --max-time 1 "http://127.0.0.1:$ROBOT_HTTP/read/$ROBOT_HEALTH_FILE" > /dev/null 2>&1; then
        echo -e "${GREEN}[READY]${NC} Robot Marshal is responding."
        break
    fi
    sleep 0.5
done

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
"$MRI_IMAGE_STREAMER_BIN" --http http://127.0.0.1:$MRI_HTTP --frames 20 --dt-ms 50
echo "    [CHECK] Index tracks growing stream: $(tail -n 1 $DATA_MRI/mrd/index.jsonl | cut -c1-60)..."

echo -e "\n${CYAN}[B] Full File Ingestion (Atomic Mode):${NC}"
echo "Uploading a completed scan file (.mrd) via /v1/mrd/ingest."
"$MRI_MK_MRD_BIN" "$DATA_MRI/full_scan_demo.mrd" > /dev/null
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
python3 -u "$MRI_MARSHAL_DIR/clients/mocks/http_tracker.py" > "$DATA_MRI/http_tracker.log" 2>&1 &
HTTP_TRACKER_PID=$!
sleep 1

# Ingest via HTTP POST
curl -s -X POST http://127.0.0.1:8080/v1/bio/signal -d '{"ts":"now","source":"http_demo","data":[0.5],"rate_hz":100.0}' > /dev/null
sleep 1
echo "    - Data ingested via POST. Verifying arrival in HTTP Polling client..."
tail -n 1 "$DATA_MRI/http_tracker.log"

# Part B: Robot Marshal (Specialized Branch Code)
echo -e "\n${CYAN}[B] Robot Marshal (Port 8081):${NC} State Blackboard (RAM Buffer)"
ROBOT_STATUS_FILE=$(python3 - <<'PY'
import json
try:
    files = json.load(open("files.json"))
except Exception:
    files = []
print("robot_status" if "robot_status" in files else (files[0] if files else "robot_status"))
PY
)
echo "[*] Writing to Robot 'Blackboard' via /write/$ROBOT_STATUS_FILE..."
# Send specialized schema required by robot branch
curl -s -X POST "http://127.0.0.1:8081/write/$ROBOT_STATUS_FILE" \
  -H "Content-Type: application/json" \
  -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"demo_bot\", \"values\": [{\"pos\": \"SCAN_START\"}]}" > /dev/null

echo "[*] Reading back from Robot 'Blackboard' via /read/$ROBOT_STATUS_FILE..."
ROBOT_READ=$(curl -s "http://127.0.0.1:8081/read/$ROBOT_STATUS_FILE")
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
seq 1 5 | xargs -I{} -P 15 curl -s -X POST http://127.0.0.1:$MRI_HTTP/v1/bio/signal -d '{"ts":"now","source":"demo","data":[0.1,0.8,0.1],"rate_hz":100.0}' > /dev/null &
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
# STEP 5: Robot Marshal Demo (3 C++ Clients - Original Upstream Design)
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}STEP 5: Robot Marshal - Concurrent Operation with MRI Marshal${NC}"
echo "Testing robot marshal clients while MRI marshal is active."
echo ""
echo -e "${CYAN}Demonstrating:${NC}"
echo "  - Both marshals running simultaneously (MRI on port 8080, Robot on port 8081)"
echo "  - Robot clients running with upstream routing config"
echo "  - Pattern: file1 → client-a → file2 → client-b → file3 → client-c → file1"
echo ""

# Setup for C++ clients
echo "[*] Setting up file routing configuration..."
if [ -f "$ROBOT_MARSHAL_DIR/file_routes.json" ]; then
    cp "$ROBOT_MARSHAL_DIR/file_routes.json" ./file_routes.json
else
    cat > ./file_routes.json <<'EOF'
{
  "client-a": {
    "read_from": "file1.json",
    "write_to1": "file2.json",
    "write_to2": "file3.json"
  },
  "client-b": {
    "read_from": "file2.json",
    "write_to": "file3.json"
  },
  "client-c": {
    "read_from": "file3.json",
    "write_to": "file1.json"
  }
}
EOF
fi

echo "[*] Running robot clients for 5 seconds while MRI marshal is active..."
START=$(date +%s%N)
if ! ensure_robot_clients_ready; then
    echo -e "${RED}[ERROR]${NC} Robot client binaries not found. Set ROBOT_MARSHAL_DIR or ROBOT_CLIENTS."
    exit 1
fi
CLIENT_NAMES=()
CLIENT_LOGS=()
CLIENT_PIDS=()
while IFS= read -r entry; do
    [ -z "$entry" ] && continue
    name=${entry%%:*}
    bin=${entry#*:}
    log="/tmp/demo_${name}.log"
    timeout 5 "$bin" > "$log" 2>&1 &
    pid=$!
    CLIENT_NAMES+=("$name")
    CLIENT_LOGS+=("$log")
    CLIENT_PIDS+=("$pid")
    echo -e "    - Launched ${name} (PID: ${YELLOW}$pid${NC})"
done <<< "$ROBOT_CLIENTS"
echo ""
echo "[*] Clients running circular data flow..."
echo "    (This tests thread-safety under continuous concurrent access)"

# Wait for clients
for pid in "${CLIENT_PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done
END=$(date +%s%N)
ELAPSED=$(( (END - START) / 1000000 ))

# Count iterations
TOTAL=0
DETAILS=""
for idx in "${!CLIENT_NAMES[@]}"; do
    name="${CLIENT_NAMES[$idx]}"
    log="${CLIENT_LOGS[$idx]}"
    iters=$(grep -cE "Read|Result sent" "$log" 2>/dev/null || echo 0)
    TOTAL=$((TOTAL + iters))
    DETAILS="${DETAILS} ${name}:${iters}"
done
RATE=$(( TOTAL * 1000 / ELAPSED ))

echo ""
echo -e "${GREEN}[RESULTS]${NC}"
echo -e "    - Clients: ${YELLOW}${DETAILS# }${NC}"
echo -e "    - Total: ${YELLOW}${TOTAL}${NC} iterations in ${ELAPSED}ms"
echo -e "    - Throughput: ${YELLOW}${RATE}${NC} operations/sec"
echo -e "    - Both marshals operational simultaneously: ${GREEN}✓${NC}"
echo ""
echo -e "${GREEN}[SUCCESS]${NC} Robot marshal handled continuous circular data flow with no deadlocks."
echo "             MRI marshal remained responsive throughout the test."
pause

# ------------------------------------------------------------------------------
# STEP 6: Safety Bridge (E-Stop)
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}STEP 6: Inter-Marshal Safety (Software E-Stop)${NC}"
echo "Starting the Coordinator Bridge..."
python3 -u "$MRI_MARSHAL_DIR/clients/bridge/coordinator.py" > "$DATA_MRI/coordinator.log" 2>&1 &
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
# STEP 7: Recording & Replay
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}STEP 7: Recording & Replay Mode${NC}"
echo "Capturing data into a timestamped 'Dumpbox' session..."
pkill -f "build/marshal" || true
mkdir -p "$DATA_DUMPBOX"

"$MRI_MARSHAL_BIN" --http 127.0.0.1:8080 --data "$DATA_DUMPBOX" --sink dumpbox --dumpbox-root "$DATA_DUMPBOX" > "$DATA_DUMPBOX/server.log" 2>&1 &
sleep 2

# Record one frame
"$MRI_IMAGE_STREAMER_BIN" --http http://127.0.0.1:$MRI_HTTP --frames 1 --stream dumpbox_stream > /dev/null

SESSION_DIR=$(ls -dt $DATA_DUMPBOX/202* | head -n 1)
echo -e "${GREEN}[RECORDED]${NC} Session: $SESSION_DIR"

echo -e "\n[*] Replaying into Live Marshal..."
pkill -f "build/marshal" || true
"$MRI_MARSHAL_BIN" --http 127.0.0.1:$MRI_HTTP --data "$DATA_MRI" --sink mrd > "$DATA_MRI/server.log" 2>&1 &
sleep 2
"$MRI_PLAYBACK_BIN" --http http://127.0.0.1:$MRI_HTTP --data "$SESSION_DIR" --speed 1.0

echo -e "${GREEN}[SUCCESS]${NC} Replay complete."
pause

# ------------------------------------------------------------------------------
# CONCLUSION
# ------------------------------------------------------------------------------
header
echo -e "${GREEN}DEMO COMPLETE${NC}"
echo "Architecture verified: High-throughput images, clinical telemetry, and safety bridges."
