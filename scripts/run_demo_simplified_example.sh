#!/bin/bash
# Example: Simplified Demo Script Using Helpers
# This demonstrates how the demo script could look with helper functions
# Compare this to run_demo_simultaneous_noninteractive.sh (522 lines)

set -e

# ============================================================================
# SETUP
# ============================================================================

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
source "$SCRIPT_DIR/tools/robot_marshal_env.sh"
source "$SCRIPT_DIR/tools/mri_marshal_env.sh"
source "$SCRIPT_DIR/tools/demo_helpers.sh"  # New helper library
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT_DIR"

# Configuration
MRI_ENDPOINT="127.0.0.1:8080"
MRI_WS_ENDPOINT="127.0.0.1:8090"
ROBOT_ENDPOINT="127.0.0.1:8081"
DATA_MRI="./data_demo_mri/run_$(date +%Y%m%d_%H%M%S)"
DATA_ROBOT="./data_demo_robot"
DEMO_DURATION_SEC=30
IMAGE_INTERVAL_MS=20
ECG_INTERVAL_MS=250
POSE_INTERVAL_MS=500
SHUTDOWN_TIMEOUT_SEC=15
KEEP_DEMO_DATA=1

# Calculated values
IMAGE_FRAME_COUNT=$((DEMO_DURATION_SEC * 1000 / IMAGE_INTERVAL_MS))
ECG_COUNT_TARGET=$((DEMO_DURATION_SEC * 1000 / ECG_INTERVAL_MS))
POSE_COUNT_TARGET=$((DEMO_DURATION_SEC * 1000 / POSE_INTERVAL_MS))
IMAGE_SIZE=64
IMAGE_NSLICES=10

# Setup environment (HDF5 + X11)
setup_demo_environment
echo "Using DISPLAY=$DISPLAY"

# ============================================================================
# CLEANUP HANDLER
# ============================================================================

cleanup() {
    echo ""
    echo "[CLEANUP] Terminating all demo processes..."

    # Stop producers/clients
    [ -n "$STREAMER_PID" ] && graceful_stop "$STREAMER_PID" "Image Streamer" 5
    [ -n "$ECG_PID" ] && graceful_stop "$ECG_PID" "ECG Sender" 5
    [ -n "$POSE_PID" ] && graceful_stop "$POSE_PID" "Pose Sender" 5

    for pid in "${CLIENT_PIDS[@]}"; do
        [ -n "$pid" ] && graceful_stop "$pid" "Robot Client" 5
    done

    # Stop marshals
    [ -n "$MRI_PID" ] && graceful_stop "$MRI_PID" "MRI Marshal" "$SHUTDOWN_TIMEOUT_SEC"
    [ -n "$ROBOT_PID" ] && graceful_stop "$ROBOT_PID" "Robot Marshal" 5

    # Cleanup files
    cleanup_demo_files "$KEEP_DEMO_DATA"

    echo "✓ Cleanup complete"
}

trap cleanup EXIT

# ============================================================================
# STEP 1: Start Marshals
# ============================================================================

print_header "STEP 1: Starting Dual-Marshal Servers"

mkdir -p "$DATA_MRI" "$DATA_ROBOT"

# Create robot config files
create_files_json > ./files.json
create_file_routes_json ./file_routes.json
mkdir -p ./log_files
ROBOT_FILES_DIR="."
[ -d "$ROBOT_MARSHAL_DIR/files" ] && ROBOT_FILES_DIR="./files"
mkdir -p "$ROBOT_FILES_DIR"
initialize_robot_files "$ROBOT_FILES_DIR"

# Start MRI Marshal
echo "Starting MRI Marshal..."
ensure_mri_ready || { echo "Failed to build MRI marshal"; exit 1; }
"$MRI_MARSHAL_BIN" --http "$MRI_ENDPOINT" \
                   --ws "$MRI_WS_ENDPOINT" \
                   --data "$DATA_MRI" \
                   --shutdown-timeout-sec "$SHUTDOWN_TIMEOUT_SEC" \
                   > "$DATA_MRI/server.log" 2>&1 &
MRI_PID=$!

# Start Robot Marshal
echo "Starting Robot Marshal..."
export ROBOT_MARSHAL_PORT="${ROBOT_ENDPOINT##*:}"
ensure_robot_marshal_ready || { echo "Failed to build Robot marshal"; exit 1; }
"$ROBOT_MARSHAL_BIN" "$ROBOT_MARSHAL_PORT" > "$DATA_ROBOT/server.log" 2>&1 &
ROBOT_PID=$!

sleep 2

# Health checks
echo ""
echo -n "MRI Marshal:   "
check_mri_health "$MRI_ENDPOINT" && print_status "Ready" 0 || print_status "Failed" 1

echo -n "Robot Marshal: "
check_robot_health "$ROBOT_ENDPOINT" "robot_status" && print_status "Ready" 0 || print_status "Failed" 1

echo ""
sleep 1

# ============================================================================
# STEP 2: Launch Visualizer
# ============================================================================

print_header "STEP 2: Launching Visualizer"

if [ -f "$MRI_VIZ_CLIENT_BIN" ]; then
    "$MRI_VIZ_CLIENT_BIN" --http "http://${MRI_ENDPOINT}/v1/mrd/latest" \
        > "$DATA_MRI/viz.log" 2>&1 &
    VIZ_PID=$!
    echo "✓ Visualizer launched (PID: $VIZ_PID)"
else
    echo "⚠ viz_client not found - skipping visualizer"
fi

echo ""
sleep 1

# ============================================================================
# STEP 3: Run All Systems Simultaneously
# ============================================================================

print_header "STEP 3: Running All Systems Simultaneously"

echo "Watch the visualizer while the terminal shows data flow!"
echo ""

# Start image streamer
echo "Starting image_streamer ($IMAGE_FRAME_COUNT frames @ ${IMAGE_INTERVAL_MS}ms)..."
"$MRI_IMAGE_STREAMER_BIN" --http "http://${MRI_ENDPOINT}" \
                       --frames "$IMAGE_FRAME_COUNT" \
                       --dt-ms "$IMAGE_INTERVAL_MS" \
                       --size "$IMAGE_SIZE" \
                       --nslices "$IMAGE_NSLICES" > /tmp/streamer.log 2>&1 &
STREAMER_PID=$!

# Start robot clients
ensure_robot_clients_ready || { echo "Failed to build robot clients"; exit 1; }
CLIENT_NAMES=()
CLIENT_PIDS=()
while IFS= read -r entry; do
    [ -z "$entry" ] && continue
    name=${entry%%:*}
    bin=${entry#*:}
    "$bin" > "/tmp/${name}.log" 2>&1 &
    CLIENT_NAMES+=("$name")
    CLIENT_PIDS+=("$!")
    echo "  • $name (PID: $!)"
done <<< "$ROBOT_CLIENTS"

echo ""
print_separator

# Start ECG and Pose loops using helpers
echo "Starting ECG sender ($ECG_COUNT_TARGET samples @ ${ECG_INTERVAL_MS}ms)..."
ECG_PID=$(start_ecg_loop "$MRI_ENDPOINT" "$ECG_COUNT_TARGET" "$ECG_INTERVAL_MS")

echo "Starting Pose sender ($POSE_COUNT_TARGET updates @ ${POSE_INTERVAL_MS}ms)..."
POSE_PID=$(start_pose_loop "$MRI_ENDPOINT" "$POSE_COUNT_TARGET" "$POSE_INTERVAL_MS")

echo ""
print_separator
echo "     Visualization running - watch the OpenCV window"
print_separator
echo ""

# Monitor loop
MONITOR_ITERATIONS=$((DEMO_DURATION_SEC * 10))
for i in $(seq 1 $MONITOR_ITERATIONS); do
    ROBOT_TOTAL=0
    DETAILS=""
    for idx in "${!CLIENT_NAMES[@]}"; do
        name="${CLIENT_NAMES[$idx]}"
        ops=$(grep -cE "Read|Result sent" "/tmp/${name}.log" 2>/dev/null || echo 0)
        ROBOT_TOTAL=$((ROBOT_TOTAL + ops))
        DETAILS="${DETAILS} ${name}:${ops}"
    done
    echo "[$(date +%T)] Robot clients: $ROBOT_TOTAL ops (${DETAILS# })"
    sleep 0.1
done

# Wait for background loops
wait "$ECG_PID" 2>/dev/null || true
wait "$POSE_PID" 2>/dev/null || true

# Stop streamer and clients
kill "$STREAMER_PID" 2>/dev/null || true
for pid in "${CLIENT_PIDS[@]}"; do
    kill "$pid" 2>/dev/null || true
done

# ============================================================================
# STEP 4: Summary
# ============================================================================

print_header "STEP 4: Summary"

echo "Final Robot Marshal State:"
curl -s "http://${ROBOT_ENDPOINT}/read/robot_status" | python3 -m json.tool 2>/dev/null | head -15
echo ""

echo "Latest MRI Frame:"
curl -s "http://${MRI_ENDPOINT}/v1/mrd/latest" | python3 -m json.tool 2>/dev/null | head -10
echo ""

print_separator
echo "         ✓ DEMO COMPLETE - ALL SYSTEMS WORKED"
print_separator
echo ""
echo "Statistics:"
VIZ_FPS=$((1000 / IMAGE_INTERVAL_MS))
echo "  • Visualizer:   ~$IMAGE_FRAME_COUNT frames @ ${VIZ_FPS}fps"
echo "  • ECG signals:  ~$ECG_COUNT_TARGET sent @ ${ECG_INTERVAL_MS}ms"
echo "  • Pose updates: ~$POSE_COUNT_TARGET sent @ ${POSE_INTERVAL_MS}ms"
echo ""
echo "✓ Auto-exit and cleanup"
echo ""

# Cleanup happens via trap
