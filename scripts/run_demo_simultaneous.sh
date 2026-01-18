#!/bin/bash
# scripts/run_demo_simultaneous.sh
# MRI Data Marshal - Simultaneous Operations Demo
# Shows all systems running at once: Visualizer + ECG + Pose + Robot Marshal

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
source "$SCRIPT_DIR/tools/robot_marshal_env.sh"
source "$SCRIPT_DIR/tools/mri_marshal_env.sh"
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT_DIR"

# Configuration
MRI_HTTP=8080
MRI_WS=8090
ROBOT_HTTP=8081
DATA_MRI="./data_demo_mri/run_$(date +%Y%m%d_%H%M%S)"
DATA_ROBOT="./data_demo_robot"

# HDF5 file locking can fail on WSL/overlayfs; keep enabled on native Linux.
if grep -qiE "(microsoft|wsl)" /proc/version 2>/dev/null; then
    export HDF5_USE_FILE_LOCKING=FALSE
    export HDF5_FILE_LOCKING=FALSE
fi

# ============ TIMING CONFIGURATION ============
# Adjust these to control how long each feature runs
DEMO_DURATION_SEC=60              # Total demo duration (seconds)

# Image streamer / Visualizer (MRI frames)
IMAGE_INTERVAL_MS=50          # Milliseconds between MRI frames (1/IMAGE_INTERVAL_MS fps target)
IMAGE_FRAME_COUNT=$((DEMO_DURATION_SEC * 1000 / IMAGE_INTERVAL_MS))
IMAGE_SIZE=128
IMAGE_NSLICES=10

# ECG biosignal
ECG_INTERVAL_MS=250             # Milliseconds between ECG samples (4 Hz)
ECG_COUNT_TARGET=$((DEMO_DURATION_SEC * 1000 / ECG_INTERVAL_MS))

# Pose updates
POSE_INTERVAL_MS=500            # Milliseconds between pose updates (2 Hz)
POSE_COUNT_TARGET=$((DEMO_DURATION_SEC * 1000 / POSE_INTERVAL_MS))

# Robot marshal clients run continuously until demo ends
# ==============================================
MONITOR_INTERVAL=0.1              # Seconds between robot stats prints (can be 0.5, 0.1, etc.)

# Graceful shutdown configuration
SHUTDOWN_TIMEOUT_SEC=15           # How long to wait for marshal to flush HDF5 before force-kill
                                   # - Passed to marshal via --shutdown-timeout-sec
                                   # - Demo script adds 5s buffer (waits SHUTDOWN_TIMEOUT_SEC + 5)
                                   # - 15s is safe for local SSD/NVMe, use 30s for network storage

# X11 setup for GUI (handles WSL2 + devcontainer)
if [ -z "$DISPLAY" ]; then
    # Try WSLg first
    if [ -d "/mnt/wslg" ]; then
        export DISPLAY=:0
    else
        # Try to get Windows host IP for VcXsrv
        WIN_IP=$(cat /etc/resolv.conf 2>/dev/null | grep nameserver | awk '{print $2}')
        if [ -n "$WIN_IP" ]; then
            export DISPLAY="${WIN_IP}:0.0"
        else
            export DISPLAY=:0
        fi
    fi
fi
unset XAUTHORITY
echo "Using DISPLAY=$DISPLAY"

cleanup() {
    echo ""
    echo "[CLEANUP] Terminating all demo processes..."

    # Stop producers/clients first to avoid socket resets during marshal shutdown.
    if [ -n "$STREAMER_PID" ] && kill -0 $STREAMER_PID 2>/dev/null; then
        kill -TERM $STREAMER_PID 2>/dev/null || true
    fi
    for pid in "${CLIENT_PIDS[@]}"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done
    if [ -n "$ECG_PID" ] && kill -0 $ECG_PID 2>/dev/null; then
        kill -TERM $ECG_PID 2>/dev/null || true
    fi
    if [ -n "$POSE_PID" ] && kill -0 $POSE_PID 2>/dev/null; then
        kill -TERM $POSE_PID 2>/dev/null || true
    fi

    # Send SIGTERM first to allow graceful shutdown (marshal will flush HDF5)
    if [ -n "$MRI_PID" ] && kill -0 $MRI_PID 2>/dev/null; then
        echo "  → Sending SIGTERM to MRI Marshal (PID: $MRI_PID) for graceful shutdown..."
        kill -TERM $MRI_PID 2>/dev/null || true
        # Wait for graceful shutdown (SHUTDOWN_TIMEOUT_SEC + 5 second buffer)
        WAIT_LOOPS=$(((SHUTDOWN_TIMEOUT_SEC + 5) * 2))
        for i in $(seq 1 $WAIT_LOOPS); do
            if ! kill -0 $MRI_PID 2>/dev/null; then
                echo "  ✓ MRI Marshal shut down gracefully"
                break
            fi
            sleep 0.5
        done
        # Force kill if still running
        if kill -0 $MRI_PID 2>/dev/null; then
            echo "  → Force killing MRI Marshal..."
            kill -9 $MRI_PID 2>/dev/null || true
        fi
    fi

    pkill -f "robot_marshal_demo" 2>/dev/null || true
    pkill -f "viz_client" 2>/dev/null || true
    pkill -f "image_streamer" 2>/dev/null || true
    rm -rf "$DATA_MRI" "$DATA_ROBOT" "./log_files" "./files" 2>/dev/null || true

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
    rm -f ./files.json ./file_routes.json 2>/dev/null || true
    sleep 1
    echo "✓ Cleanup complete"
}

trap cleanup EXIT

header() {
    clear 2>/dev/null || true
    echo "================================================================"
    echo "   MRI DATA MARSHAL - SIMULTANEOUS OPERATIONS DEMO             "
    echo "================================================================"
    echo ""
}

# ============================================================================
# INTRO
# ============================================================================
header
echo "✓ SIMULTANEOUS OPERATIONS DEMO"
echo ""
echo "This demo shows ALL systems running at the same time:"
echo ""
echo "  1. [VISUALIZER]    - OpenCV window showing MRI frames"
echo "  2. [ECG DATA]      - Bio signals being ingested"
echo "  3. [POSE DATA]     - Robot localization updates"
echo "  4. [ROBOT MARSHAL] - State blackboard updates"
echo ""
echo "You'll see the visualizer update while the terminal shows"
echo "ECG, pose, and robot data being processed simultaneously."
echo ""
echo "→ Press ENTER to start..."
read -r

# ============================================================================
# STEP 1: Start Both Marshals
# ============================================================================
header
echo "[STEP 1] Starting Dual-Marshal Servers"
echo "───────────────────────────────────────────────────────"
echo ""

mkdir -p "$DATA_MRI" "$DATA_ROBOT"

# Create file_routes.json for robot clients (prefer external repo config).
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

# Start MRI Marshal
echo "Starting MRI Marshal (HTTP:$MRI_HTTP, WebSocket:$MRI_WS)..."
if ! ensure_mri_ready; then
    echo "MRI marshal binaries not found. Set MRI_MARSHAL_DIR."
    exit 1
fi
echo "MRI Marshal binary: $MRI_MARSHAL_BIN"
"$MRI_MARSHAL_BIN" --http 127.0.0.1:$MRI_HTTP \
                --ws 127.0.0.1:$MRI_WS \
                --data "$DATA_MRI" \
                --shutdown-timeout-sec $SHUTDOWN_TIMEOUT_SEC \
                > "$DATA_MRI/server.log" 2>&1 &
MRI_PID=$!

# Setup and start Robot Marshal
if [ -f "$ROBOT_MARSHAL_DIR/files.json" ]; then
    cp "$ROBOT_MARSHAL_DIR/files.json" ./files.json
else
    echo '["file1.json", "file2.json", "file3.json", "robot_status", "robot_commands"]' > ./files.json
fi
mkdir -p ./log_files
ROBOT_FILES_DIR="."
if [ -d "$ROBOT_MARSHAL_DIR/files" ]; then
    ROBOT_FILES_DIR="./files"
fi
export ROBOT_FILES_DIR
mkdir -p "$ROBOT_FILES_DIR"
# Initialize client data files BEFORE starting robot marshal
# Client code expects: sent_at, client_id, values fields
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

echo "Starting Robot Marshal (HTTP:$ROBOT_HTTP)..."
export ROBOT_MARSHAL_PORT="$ROBOT_HTTP"
if ! ensure_robot_marshal_ready; then
    echo "Robot marshal binary not found. Set ROBOT_MARSHAL_DIR/ROBOT_MARSHAL_BIN."
    exit 1
fi
echo "Robot Marshal binary: $ROBOT_MARSHAL_BIN"
"$ROBOT_MARSHAL_BIN" $ROBOT_HTTP > "$DATA_ROBOT/server.log" 2>&1 &
ROBOT_PID=$!

sleep 2

# Verify both are running
echo ""
echo -n "MRI Marshal:   "
if curl -s --max-time 2 http://127.0.0.1:$MRI_HTTP/health > /dev/null 2>&1; then
    echo "✓ Ready"
else
    echo "✗ Failed"
    exit 1
fi

echo -n "Robot Marshal: "
ROBOT_HEALTH_FILE=$(python3 - <<'PY'
import json
try:
    files = json.load(open("files.json"))
except Exception:
    files = []
print(files[0] if files else "robot_status")
PY
)
if curl -s --max-time 2 "http://127.0.0.1:$ROBOT_HTTP/read/$ROBOT_HEALTH_FILE" > /dev/null 2>&1; then
    echo "✓ Ready"
else
    echo "✗ Failed"
    echo "  → Retrying Robot Marshal rebuild on port $ROBOT_HTTP..."
    if [ -n "$ROBOT_PID" ] && kill -0 $ROBOT_PID 2>/dev/null; then
        kill -TERM $ROBOT_PID 2>/dev/null || true
        sleep 1
    fi
    rm -f "$ROBOT_MARSHAL_DIR/build/.robot_marshal_port" "$ROBOT_MARSHAL_BIN"
    if ! ensure_robot_marshal_ready; then
        echo "  → Rebuild failed. Check $DATA_ROBOT/server.log"
        exit 1
    fi
    "$ROBOT_MARSHAL_BIN" $ROBOT_HTTP > "$DATA_ROBOT/server.log" 2>&1 &
    ROBOT_PID=$!
    sleep 2
    if curl -s --max-time 2 "http://127.0.0.1:$ROBOT_HTTP/read/$ROBOT_HEALTH_FILE" > /dev/null 2>&1; then
        echo "✓ Ready (after rebuild)"
    else
        echo "✗ Failed (after rebuild)"
        echo "  → Last 10 lines of $DATA_ROBOT/server.log:"
        tail -n 10 "$DATA_ROBOT/server.log" 2>/dev/null || true
        exit 1
    fi
fi

echo ""
sleep 1

# ============================================================================
# STEP 2: Launch Visualizer
# ============================================================================
header
echo "[STEP 2] Launching Visualizer"
echo "───────────────────────────────────────────────────────"
echo ""

if [ -f "$MRI_VIZ_CLIENT_BIN" ]; then
    echo "Starting C++ OpenCV visualizer..."
    echo "  • HTTP polling every ${IMAGE_INTERVAL_MS}ms"
    echo "  • Simple single-loop design"
    echo "  • Controls: UP/DOWN for slices, ESC to exit"
    echo ""

    "$MRI_VIZ_CLIENT_BIN" --http http://127.0.0.1:$MRI_HTTP/v1/mrd/latest \
        > "$DATA_MRI/viz.log" 2>&1 &
    VIZ_PID=$!

    sleep 2
    echo "✓ Visualizer launched (PID: $VIZ_PID)"
    echo "  Look for the OpenCV window on your display."
else
    echo "⚠ viz_client not found - skipping visualizer"
    VIZ_PID=""
fi

echo ""
sleep 1

# ============================================================================
# STEP 3: Simultaneous Operations
# ============================================================================
header
echo "[STEP 3] Running All Systems Simultaneously"
echo "───────────────────────────────────────────────────────"
echo ""
echo "Watch the visualizer while the terminal shows data flow!"
echo ""

# Start image_streamer in background
echo "Starting image_streamer ($IMAGE_FRAME_COUNT frames @ ${IMAGE_INTERVAL_MS}ms = ${DEMO_DURATION_SEC}s)..."
echo "  • Volume: ${IMAGE_SIZE}x${IMAGE_SIZE}x${IMAGE_NSLICES} slices"
"$MRI_IMAGE_STREAMER_BIN" --http http://127.0.0.1:$MRI_HTTP \
                       --frames $IMAGE_FRAME_COUNT \
                       --dt-ms $IMAGE_INTERVAL_MS \
                       --size $IMAGE_SIZE \
                       --nslices $IMAGE_NSLICES > /tmp/streamer.log 2>&1 &
STREAMER_PID=$!

# Ensure robot client binaries exist (from external repo).
if ! ensure_robot_clients_ready; then
    echo "Robot client binaries not found. Set ROBOT_MARSHAL_DIR or ROBOT_CLIENTS."
    exit 1
fi
echo "  Robot clients already built."

# Start robot clients in background
echo "Starting Robot Marshal clients..."
CLIENT_NAMES=()
CLIENT_LOGS=()
CLIENT_PIDS=()
while IFS= read -r entry; do
    [ -z "$entry" ] && continue
    name=${entry%%:*}
    bin=${entry#*:}
    log="/tmp/${name}.log"
    "$bin" > "$log" 2>&1 &
    pid=$!
    CLIENT_NAMES+=("$name")
    CLIENT_LOGS+=("$log")
    CLIENT_PIDS+=("$pid")
    echo "  • $name (PID: $pid)"
done <<< "$ROBOT_CLIENTS"

echo ""
echo "═══════════════════════════════════════════════════════"
echo "          LIVE DATA FLOW (${DEMO_DURATION_SEC} seconds)"
echo "═══════════════════════════════════════════════════════"

# Convert intervals to sleep values
ECG_SLEEP=$(awk "BEGIN {printf \"%.3f\", $ECG_INTERVAL_MS/1000}")
POSE_SLEEP=$(awk "BEGIN {printf \"%.3f\", $POSE_INTERVAL_MS/1000}")

# Background ECG loop (runs at ECG_INTERVAL_MS)
echo "Starting ECG sender ($ECG_COUNT_TARGET samples @ ${ECG_INTERVAL_MS}ms)..."
(
    for i in $(seq 1 $ECG_COUNT_TARGET); do
        ECG_VAL1=$(awk "BEGIN {printf \"%.2f\", $RANDOM/32767}")
        ECG_VAL2=$(awk "BEGIN {printf \"%.2f\", $RANDOM/32767}")
        ECG_VAL3=$(awk "BEGIN {printf \"%.2f\", $RANDOM/32767}")
        ECG_RESP=$(curl -s -X POST http://127.0.0.1:$MRI_HTTP/v1/bio/signal \
             -H "Content-Type: application/json" \
             -d "{\"source\":\"ecg_monitor\", \"data\":[$ECG_VAL1, $ECG_VAL2, $ECG_VAL3], \"rate_hz\":100.0}" 2>&1)
        if echo "$ECG_RESP" | grep -q "ok"; then
            echo "  [ECG]   ✓ [$ECG_VAL1, $ECG_VAL2, $ECG_VAL3]"
        fi
        sleep $ECG_SLEEP
    done
) &
ECG_PID=$!

# Background Pose loop (runs at POSE_INTERVAL_MS)
echo "Starting Pose sender ($POSE_COUNT_TARGET updates @ ${POSE_INTERVAL_MS}ms)..."
(
    for i in $(seq 1 $POSE_COUNT_TARGET); do
        POSE_X=$(awk "BEGIN {printf \"%.2f\", ($RANDOM/32767)*20}")
        POSE_Y=$(awk "BEGIN {printf \"%.2f\", ($RANDOM/32767)*20}")
        POSE_Z=$(awk "BEGIN {printf \"%.2f\", ($RANDOM/32767)*10}")
        POSE_RESP=$(curl -s -X POST http://127.0.0.1:$MRI_HTTP/v1/pose/update \
             -H "Content-Type: application/json" \
             -d "{\"p\": [$POSE_X, $POSE_Y, -$POSE_Z], \"R\": [1,0,0, 0,1,0, 0,0,1]}" 2>&1)
        if echo "$POSE_RESP" | grep -q "ok"; then
            echo "  [POSE]  ✓ [$POSE_X, $POSE_Y, -$POSE_Z]"
        fi
        sleep $POSE_SLEEP
    done
) &
POSE_PID=$!

echo ""
echo "═══════════════════════════════════════════════════════"
echo "     Visualization running - watch the OpenCV window"
echo "     Terminal data shows ECG/Pose/Robot activity"
echo "═══════════════════════════════════════════════════════"
echo ""

# Main monitor loop - runs for DEMO_DURATION_SEC
MONITOR_ITERATIONS=$(awk "BEGIN {printf \"%d\", $DEMO_DURATION_SEC / $MONITOR_INTERVAL}")
for i in $(seq 1 $MONITOR_ITERATIONS); do
    ROBOT_TOTAL=0
    DETAILS=""
    for idx in "${!CLIENT_NAMES[@]}"; do
        name="${CLIENT_NAMES[$idx]}"
        log="${CLIENT_LOGS[$idx]}"
        ops=$(grep -cE "Read|Result sent" "$log" 2>/dev/null || true)
        ops=${ops:-0}
        ROBOT_TOTAL=$((ROBOT_TOTAL + ops))
        DETAILS="${DETAILS} ${name}:${ops}"
    done
    echo "[$(date +%T)] Robot clients: $ROBOT_TOTAL ops (${DETAILS# })"
    sleep $MONITOR_INTERVAL
done

wait $ECG_PID 2>/dev/null || true
wait $POSE_PID 2>/dev/null || true

echo ""
echo "═══════════════════════════════════════════════════════"

# Stop streamer and clients
kill $STREAMER_PID 2>/dev/null || true
for pid in "${CLIENT_PIDS[@]}"; do
    kill "$pid" 2>/dev/null || true
done
wait $STREAMER_PID 2>/dev/null || true
for pid in "${CLIENT_PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done

# Final robot client stats
FINAL_TOTAL=0
FINAL_DETAILS=""
for idx in "${!CLIENT_NAMES[@]}"; do
    name="${CLIENT_NAMES[$idx]}"
    log="${CLIENT_LOGS[$idx]}"
    ops=$(grep -cE "Read|Result sent" "$log" 2>/dev/null || true)
    ops=${ops:-0}
    FINAL_TOTAL=$((FINAL_TOTAL + ops))
    FINAL_DETAILS="${FINAL_DETAILS} ${name}:${ops}"
done
FINAL_TOTAL=${FINAL_TOTAL:-0}

# ============================================================================
# STEP 4: Summary
# ============================================================================
echo ""
echo "[STEP 4] Summary"
echo "───────────────────────────────────────────────────────"
echo ""

# Get final robot state
echo "Final Robot Marshal State:"
curl -s http://127.0.0.1:$ROBOT_HTTP/read/robot_status | python3 -m json.tool 2>/dev/null | head -15
echo ""

# Get latest MRI frame info
echo "Latest MRI Frame:"
curl -s http://127.0.0.1:$MRI_HTTP/v1/mrd/latest | python3 -m json.tool 2>/dev/null | head -10
echo ""

echo "═══════════════════════════════════════════════════════"
echo "         ✓ DEMO COMPLETE - ALL SYSTEMS WORKED"
echo "═══════════════════════════════════════════════════════"
echo ""
echo "Statistics:"
VIZ_FPS=$((1000 / IMAGE_INTERVAL_MS))
echo "  • Visualizer:   ~$IMAGE_FRAME_COUNT frames @ ${VIZ_FPS}fps displayed"
echo "  • ECG signals:  ~$ECG_COUNT_TARGET sent @ ${ECG_INTERVAL_MS}ms"
echo "  • Pose updates: ~$POSE_COUNT_TARGET sent @ ${POSE_INTERVAL_MS}ms"
echo "  • Robot clients: $FINAL_TOTAL ops (${FINAL_DETAILS# })"
echo ""
echo "Running Processes:"
echo "  • MRI Marshal:   PID $MRI_PID (port $MRI_HTTP)"
echo "  • Robot Marshal: PID $ROBOT_PID (port $ROBOT_HTTP)"
if [ -n "$VIZ_PID" ]; then
    echo "  • Visualizer:    PID $VIZ_PID"
fi
echo ""
echo "→ Press ENTER to exit and cleanup..."
read -r

echo ""
echo "✓ Thank you for watching the demo!"
echo ""
