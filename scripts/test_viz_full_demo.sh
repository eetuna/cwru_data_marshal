#!/bin/bash
# scripts/test_viz_full_demo.sh
# Full viz_client feature test with configurable parameters

set -e

cd /workspaces/cwru_data_marshal

# ============================================================================
# CONFIGURABLE PARAMETERS - Adjust these!
# ============================================================================
IMAGE_SIZE=${1:-192}          # Image dimensions (WxH), default: 192
NUM_SLICES=${2:-10}           # Number of Z slices, default: 10
NUM_FRAMES=${3:-2400}         # Total frames to generate, default: 2400
FRAME_INTERVAL_MS=${4:-50}    # Milliseconds between frames, default: 50 (20fps)

# Calculate demo duration
DURATION_SEC=$((NUM_FRAMES * FRAME_INTERVAL_MS / 1000))
DURATION_MIN=$((DURATION_SEC / 60))

MRI_HTTP=8080
DATA_DIR="./data_viz_demo"

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

cleanup() {
    echo ""
    echo -e "${YELLOW}[CLEANUP] Stopping all processes...${NC}"
    pkill -f "build/marshal" 2>/dev/null || true
    pkill -f "viz_client" 2>/dev/null || true
    pkill -f "image_streamer" 2>/dev/null || true
    rm -rf "$DATA_DIR" 2>/dev/null || true
    sleep 1
    echo -e "${GREEN}✓ Cleanup complete${NC}"
}

trap cleanup EXIT

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  viz_client Full Feature Demo - Configurable Test         ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${GREEN}Configuration:${NC}"
echo "  • Volume size: ${IMAGE_SIZE}x${IMAGE_SIZE}x${NUM_SLICES}"
echo "  • Frame rate: $((1000 / FRAME_INTERVAL_MS)) fps (${FRAME_INTERVAL_MS}ms per frame)"
echo "  • Duration: ~${DURATION_MIN} minutes (${NUM_FRAMES} frames)"
echo "  • Data dir: $DATA_DIR"
echo ""
echo -e "${YELLOW}Usage: ./scripts/test_viz_full_demo.sh [SIZE] [SLICES] [FRAMES] [INTERVAL_MS]${NC}"
echo "  Examples:"
echo "    ./scripts/test_viz_full_demo.sh              # 192x192x10, 2400 frames, 50ms"
echo "    ./scripts/test_viz_full_demo.sh 256 15 500  # 256x256x15, 500 frames, 50ms"
echo "    ./scripts/test_viz_full_demo.sh 96 5 1000 100  # 96x96x5, 1000 frames, 100ms"
echo ""

# ============================================================================
# Step 1: Start MRI Marshal
# ============================================================================
echo -e "${BLUE}[STEP 1/3] Starting MRI Marshal...${NC}"
mkdir -p "$DATA_DIR"

./build/marshal --http 127.0.0.1:$MRI_HTTP \
                --ws 127.0.0.1:9999 \
                --data "$DATA_DIR" \
                --flush-frames 1 \
                > "$DATA_DIR/marshal.log" 2>&1 &
MARSHAL_PID=$!

sleep 2

# Verify marshal is running
if curl -s --max-time 2 http://127.0.0.1:$MRI_HTTP/health > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Marshal running (PID: $MARSHAL_PID)${NC}"
else
    echo -e "${YELLOW}✗ Marshal failed to start${NC}"
    exit 1
fi

# ============================================================================
# Step 2: Start viz_client
# ============================================================================
echo ""
echo -e "${BLUE}[STEP 2/3] Starting viz_client...${NC}"
echo ""
echo -e "${YELLOW}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${YELLOW}║  VISUALIZER CONTROLS:                                      ║${NC}"
echo -e "${YELLOW}║                                                            ║${NC}"
echo -e "${YELLOW}║  ↑ (UP arrow)    - Next slice                             ║${NC}"
echo -e "${YELLOW}║  ↓ (DOWN arrow)  - Previous slice                         ║${NC}"
echo -e "${YELLOW}║  ESC             - Exit visualization                     ║${NC}"
echo -e "${YELLOW}║                                                            ║${NC}"
echo -e "${YELLOW}║  Display shows:  Slice Z/${NUM_SLICES}  FPS               ║${NC}"
echo -e "${YELLOW}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Start viz_client in background, logging to file
./build/viz_client --http http://127.0.0.1:$MRI_HTTP/v1/mrd/latest \
                   > "$DATA_DIR/viz.log" 2>&1 &
VIZ_PID=$!

sleep 2

if ps -p $VIZ_PID > /dev/null; then
    echo -e "${GREEN}✓ viz_client started (PID: $VIZ_PID)${NC}"
    echo -e "${GREEN}  Check your display for the OpenCV window!${NC}"
else
    echo -e "${YELLOW}✗ viz_client failed to start${NC}"
    exit 1
fi

# ============================================================================
# Step 3: Start image_streamer
# ============================================================================
echo ""
echo -e "${BLUE}[STEP 3/3] Starting image_streamer...${NC}"
echo "  • Generating ${NUM_FRAMES} frames at $((1000 / FRAME_INTERVAL_MS)) fps (${FRAME_INTERVAL_MS}ms interval)"
echo "  • Volume: ${IMAGE_SIZE}x${IMAGE_SIZE}x${NUM_SLICES} slices"
echo "  • Estimated duration: ${DURATION_SEC} seconds (~${DURATION_MIN} minutes)"
echo ""

./build/image_streamer --http http://127.0.0.1:$MRI_HTTP \
                       --frames ${NUM_FRAMES} \
                       --dt-ms ${FRAME_INTERVAL_MS} \
                       --size ${IMAGE_SIZE} \
                       --nslices ${NUM_SLICES} 2>&1 | tee "$DATA_DIR/streamer.log" &
STREAMER_PID=$!

echo -e "${GREEN}✓ image_streamer started (PID: $STREAMER_PID)${NC}"
echo ""

# ============================================================================
# Monitor Progress
# ============================================================================
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  DEMO RUNNING - Test these features:                      ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "  1. Watch frames update smoothly (should show 1, 2, 3, 4...)"
echo "  2. Press UP arrow multiple times to scroll through slices"
echo "  3. Press DOWN arrow to go back"
echo "  4. Observe FPS counter (should be ~20 fps)"
echo "  5. Check slice indicator changes when you press UP/DOWN"
echo ""
echo -e "${YELLOW}Monitoring for 120 seconds...${NC}"
echo ""

# Monitor progress every 15 seconds
MONITOR_ITERATIONS=$((DURATION_SEC / 15 + 1))
for i in $(seq 1 $MONITOR_ITERATIONS); do
    sleep 15
    ELAPSED=$((i * 15))

    # Check if processes are still running
    if ! ps -p $VIZ_PID > /dev/null; then
        echo -e "${YELLOW}⚠ viz_client exited early (user pressed ESC or crash)${NC}"
        break
    fi

    if ! ps -p $STREAMER_PID > /dev/null; then
        echo -e "${GREEN}✓ image_streamer completed${NC}"
        break
    fi

    # Show progress
    FPS=$((1000 / FRAME_INTERVAL_MS))
    FRAMES_EXPECTED=$((ELAPSED * FPS))
    echo -e "${BLUE}[$ELAPSED s / ${DURATION_SEC} s] Status: ${FRAMES_EXPECTED}/${NUM_FRAMES} frames expected...${NC}"

    # Sample viz.log for recent activity
    if [ -f "$DATA_DIR/viz.log" ]; then
        ENQUEUED=$(grep -c "enqueued frame" "$DATA_DIR/viz.log" 2>/dev/null || echo "0")
        echo "  • Frames discovered: $ENQUEUED"
    fi
done

echo ""
echo -e "${GREEN}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  DEMO COMPLETE                                             ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Wait for processes to finish
if ps -p $STREAMER_PID > /dev/null; then
    echo "Waiting for image_streamer to complete..."
    wait $STREAMER_PID 2>/dev/null || true
fi

echo ""
echo -e "${BLUE}[SUMMARY]${NC}"
echo "─────────────────────────────────────────────────"

# Count frames discovered
if [ -f "$DATA_DIR/viz.log" ]; then
    TOTAL_ENQUEUED=$(grep "enqueued frame" "$DATA_DIR/viz.log" | tail -1 | grep -oE "total=[0-9]+" | cut -d= -f2 || echo "unknown")
    echo "  • Total frames in HDF5: $TOTAL_ENQUEUED"

    # Check for frame sequence
    FRAME_0=$(grep -c "enqueued frame 0 " "$DATA_DIR/viz.log" 2>/dev/null || echo "0")
    if [ "$FRAME_0" -gt 0 ]; then
        echo -e "  • ${GREEN}✓ Frame 0 discovered${NC}"
    fi

    # Check display FPS
    AVG_FPS=$(grep "display_fps=" "$DATA_DIR/viz.log" | tail -5 | grep -oE "[0-9]+\.[0-9]+" | awk '{sum+=$1} END {if(NR>0) print sum/NR; else print "N/A"}')
    echo "  • Average display FPS: $AVG_FPS"
fi

# Check streamer output
if [ -f "$DATA_DIR/streamer.log" ]; then
    LAST_FRAME=$(grep "frame [0-9]* ->" "$DATA_DIR/streamer.log" | tail -1 | grep -oE "frame [0-9]+" | cut -d' ' -f2 || echo "unknown")
    echo "  • Last frame sent: $LAST_FRAME"
fi

echo ""
echo -e "${YELLOW}Log files saved to: $DATA_DIR/${NC}"
echo "  • marshal.log"
echo "  • viz.log"
echo "  • streamer.log"
echo ""

# Check if viz_client is still running
if ps -p $VIZ_PID > /dev/null; then
    echo -e "${YELLOW}viz_client is still running. Press ESC in the window to exit.${NC}"
    echo "Waiting for user to close viz_client..."
    wait $VIZ_PID 2>/dev/null || true
fi

echo ""
echo -e "${GREEN}✓ Demo finished successfully!${NC}"
echo ""
