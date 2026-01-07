#!/bin/bash
# scripts/run_demo_simultaneous.sh
# MRI Data Marshal - Simultaneous Operations Demo
# Shows all systems running at once: Visualizer + ECG + Pose + Robot Marshal

set -e

# Configuration
MRI_HTTP=8080
MRI_WS=8090
ROBOT_HTTP=8081
DATA_MRI="./data_demo_mri"
DATA_ROBOT="./data_demo_robot"

# ============ TIMING CONFIGURATION ============
# Adjust these to control how long each feature runs
DEMO_DURATION_SEC=60              # Total demo duration (seconds)

# Image streamer / Visualizer (MRI frames)
IMAGE_INTERVAL_MS=100          # Milliseconds between MRI frames (1/IMAGE_INTERVAL_MS fps target)
IMAGE_FRAME_COUNT=$((DEMO_DURATION_SEC * 1000 / IMAGE_INTERVAL_MS))
IMAGE_SIZE=64
IMAGE_NSLICES=3

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
SHUTDOWN_TIMEOUT_SEC=30           # Marshal's internal timeout for flushing HDF5 (passed to --shutdown-timeout-sec)
SHUTDOWN_WAIT_SEC=35              # Demo script wait time (should be > SHUTDOWN_TIMEOUT_SEC)

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

    # Send SIGTERM first to allow graceful shutdown (marshal will flush HDF5)
    if [ -n "$MRI_PID" ] && kill -0 $MRI_PID 2>/dev/null; then
        echo "  → Sending SIGTERM to MRI Marshal (PID: $MRI_PID) for graceful shutdown..."
        kill -TERM $MRI_PID 2>/dev/null || true
        # Wait for graceful shutdown (uses SHUTDOWN_WAIT_SEC from config)
        WAIT_LOOPS=$((SHUTDOWN_WAIT_SEC * 2))
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
    rm -rf "$DATA_MRI" "$DATA_ROBOT" "./files.json" "./files" "./log_files" 2>/dev/null || true
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

# Copy file_routes.json for robot clients (required by client code)
cp ./scripts/robot_marshal_src/file_routes.json ./

# Start MRI Marshal
echo "Starting MRI Marshal (HTTP:$MRI_HTTP, WebSocket:$MRI_WS)..."
./build/marshal --http 127.0.0.1:$MRI_HTTP \
                --ws 127.0.0.1:$MRI_WS \
                --data "$DATA_MRI" \
                --flush-frames 1 \
                --shutdown-timeout-sec $SHUTDOWN_TIMEOUT_SEC \
                > "$DATA_MRI/server.log" 2>&1 &
MRI_PID=$!

# Setup and start Robot Marshal
echo '["file1.json", "file2.json", "file3.json", "robot_status", "robot_commands"]' > ./files.json
mkdir -p ./files ./log_files
echo '{}' > ./files/robot_status
# Initialize client data files BEFORE starting robot marshal
# Client code expects: sent_at, client_id, values fields
echo '{"client_id":"seed","sent_at":1,"values":[1.0,2.0,3.0]}' > ./files/file1.json
echo '{"client_id":"seed","sent_at":1,"values":[1.0,2.0,3.0]}' > ./files/file2.json
echo '{"client_id":"seed","sent_at":1,"values":[1.0,2.0,3.0]}' > ./files/file3.json

echo "Starting Robot Marshal (HTTP:$ROBOT_HTTP)..."
./build/robot_marshal_demo $ROBOT_HTTP > "$DATA_ROBOT/server.log" 2>&1 &
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
if curl -s --max-time 2 http://127.0.0.1:$ROBOT_HTTP/read/robot_status > /dev/null 2>&1; then
    echo "✓ Ready"
else
    echo "✗ Failed"
    exit 1
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

if [ -f "./build/viz_client" ]; then
    echo "Starting C++ OpenCV visualizer..."
    echo "  • HTTP polling every ${IMAGE_INTERVAL_MS}ms"
    echo "  • Simple single-loop design"
    echo "  • Controls: UP/DOWN for slices, ESC to exit"
    echo ""

    ./build/viz_client --http http://127.0.0.1:$MRI_HTTP/v1/mrd/latest \
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
./build/image_streamer --http http://127.0.0.1:$MRI_HTTP \
                       --frames $IMAGE_FRAME_COUNT \
                       --dt-ms $IMAGE_INTERVAL_MS \
                       --size $IMAGE_SIZE \
                       --nslices $IMAGE_NSLICES > /tmp/streamer.log 2>&1 &
STREAMER_PID=$!

# Build robot clients only if needed (compilation is slow due to header-only libs)
if [ ! -f ./build/client-a ] || [ ! -f ./build/client-b ] || [ ! -f ./build/client-c ]; then
    echo "  Compiling robot clients (this may take ~60s due to large headers)..."
    g++ -std=c++17 -I ./scripts/robot_marshal_src ./scripts/robot_marshal_src/client-a.cpp -o ./build/client-a -lpthread
    g++ -std=c++17 -I ./scripts/robot_marshal_src ./scripts/robot_marshal_src/client-b.cpp -o ./build/client-b -lpthread
    g++ -std=c++17 -I ./scripts/robot_marshal_src ./scripts/robot_marshal_src/client-c.cpp -o ./build/client-c -lpthread
    echo "  Robot clients compiled."
else
    echo "  Robot clients already built."
fi

# Start the 3 robot clients in background
echo "Starting 3 Robot Marshal clients (circular data flow)..."
./build/client-a > /tmp/client-a.log 2>&1 &
CLIENT_A_PID=$!
./build/client-b > /tmp/client-b.log 2>&1 &
CLIENT_B_PID=$!
./build/client-c > /tmp/client-c.log 2>&1 &
CLIENT_C_PID=$!
echo "  • client-a (PID: $CLIENT_A_PID) file1 → file2"
echo "  • client-b (PID: $CLIENT_B_PID) file2 → file3"
echo "  • client-c (PID: $CLIENT_C_PID) file3 → file1"

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
    CLIENT_A_OPS=$(grep -cE "Read values|Write" /tmp/client-a.log 2>/dev/null || true)
    CLIENT_B_OPS=$(grep -cE "Read values|Write" /tmp/client-b.log 2>/dev/null || true)
    CLIENT_C_OPS=$(grep -cE "Read values|Write" /tmp/client-c.log 2>/dev/null || true)
    CLIENT_A_OPS=${CLIENT_A_OPS:-0}
    CLIENT_B_OPS=${CLIENT_B_OPS:-0}
    CLIENT_C_OPS=${CLIENT_C_OPS:-0}
    ROBOT_TOTAL=$((CLIENT_A_OPS + CLIENT_B_OPS + CLIENT_C_OPS))
    echo "[$(date +%T)] Robot clients: $ROBOT_TOTAL ops (A:$CLIENT_A_OPS B:$CLIENT_B_OPS C:$CLIENT_C_OPS)"
    sleep $MONITOR_INTERVAL
done

wait $ECG_PID 2>/dev/null || true
wait $POSE_PID 2>/dev/null || true

echo ""
echo "═══════════════════════════════════════════════════════"

# Stop streamer and clients
kill $STREAMER_PID 2>/dev/null || true
kill $CLIENT_A_PID 2>/dev/null || true
kill $CLIENT_B_PID 2>/dev/null || true
kill $CLIENT_C_PID 2>/dev/null || true
wait $STREAMER_PID 2>/dev/null || true
wait $CLIENT_A_PID 2>/dev/null || true
wait $CLIENT_B_PID 2>/dev/null || true
wait $CLIENT_C_PID 2>/dev/null || true

# Final robot client stats
FINAL_A=$(grep -cE "Read values|Write" /tmp/client-a.log 2>/dev/null || true)
FINAL_B=$(grep -cE "Read values|Write" /tmp/client-b.log 2>/dev/null || true)
FINAL_C=$(grep -cE "Read values|Write" /tmp/client-c.log 2>/dev/null || true)
FINAL_A=${FINAL_A:-0}
FINAL_B=${FINAL_B:-0}
FINAL_C=${FINAL_C:-0}
ROBOT_FINAL=$((FINAL_A + FINAL_B + FINAL_C))

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
echo "  • Robot clients: $ROBOT_FINAL ops (A:$FINAL_A B:$FINAL_B C:$FINAL_C)"
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
