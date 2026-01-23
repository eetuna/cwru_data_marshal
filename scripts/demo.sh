#!/bin/bash
# Full demo - Docker marshals + local clients
# Works from BOTH WSL2 and devcontainer
#   - WSL2: Uses localhost, runs viz_client with X display
#   - Devcontainer: Uses docker exec, skips viz (no display)

set -e

# ============ CONFIGURATION ============
DEMO_DURATION=30          # Total demo duration (seconds)

# Image streamer settings
IMAGE_SIZE=64             # Image width/height (pixels)
IMAGE_SLICES=4            # Number of slices
IMAGE_INTERVAL=0.5        # Frame interval (seconds)

# Client intervals
ECG_INTERVAL=1.0          # ECG sample interval (seconds)
POSE_INTERVAL=0.5         # Pose update interval (seconds)
ROBOT_INTERVAL=2          # Robot write interval (seconds)
# =======================================

# X11 setup for GUI (handles WSL2 + devcontainer)
if [ -z "$DISPLAY" ]; then
    if [ -d "/mnt/wslg" ]; then
        export DISPLAY=:0
    else
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

GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)

# Find MRI worktree
if [ -d "$ROOT_DIR/../mri_data_marshal_worktree/build" ]; then
    MRI_BUILD="$ROOT_DIR/../mri_data_marshal_worktree/build"
    MRI_MOCKS="$ROOT_DIR/../mri_data_marshal_worktree/clients/mocks"
elif [ -d "/workspaces/mri_data_marshal_worktree/build" ]; then
    MRI_BUILD="/workspaces/mri_data_marshal_worktree/build"
    MRI_MOCKS="/workspaces/mri_data_marshal_worktree/clients/mocks"
else
    echo -e "${RED}ERROR: Cannot find mri_data_marshal_worktree${NC}"
    exit 1
fi

echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}   CWRU DATA MARSHAL - FULL DEMO                        ${NC}"
echo -e "${CYAN}   Docker Marshals + Local Clients                      ${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo ""

# Detect environment
if curl -sf --max-time 2 http://localhost:8080/health > /dev/null 2>&1; then
    USE_EXEC=false
    ENV="WSL2/Host"
elif docker exec cwru-mri-marshal curl -sf --max-time 2 http://localhost:8080/health > /dev/null 2>&1; then
    USE_EXEC=true
    ENV="Devcontainer"
else
    echo -e "${RED}ERROR: Containers not reachable${NC}"
    echo "Run: docker compose up -d"
    exit 1
fi

echo -e "Environment: ${GREEN}$ENV${NC}"
echo -e "${GREEN}✓ MRI Marshal reachable${NC}"

# Check robot
if [ "$USE_EXEC" = true ]; then
    docker exec cwru-robot-marshal curl -sf --max-time 1 http://localhost:8081/ > /dev/null 2>&1 && \
        echo -e "${GREEN}✓ Robot Marshal reachable${NC}" || echo -e "${YELLOW}⚠ Robot Marshal not responding${NC}"
else
    curl -sf --max-time 1 http://localhost:8081/ > /dev/null 2>&1 && \
        echo -e "${GREEN}✓ Robot Marshal reachable${NC}" || echo -e "${YELLOW}⚠ Robot Marshal not responding${NC}"
fi

# Check binaries exist
if [ ! -x "$MRI_BUILD/image_streamer" ]; then
    echo -e "${RED}ERROR: image_streamer not found${NC}"
    echo "Build: cd $MRI_BUILD && cmake .. && make image_streamer viz_client"
    exit 1
fi
echo -e "${GREEN}✓ Local binaries found${NC}"
echo ""

# Cleanup
cleanup() {
    echo ""
    echo -e "${YELLOW}Stopping clients...${NC}"
    kill $IMAGE_PID $VIZ_PID $ECG_PID $POSE_PID $ROBOT_PID 2>/dev/null || true
    echo -e "${GREEN}✓ Done${NC}"
}
trap cleanup EXIT

echo -e "${YELLOW}Starting clients...${NC}"
echo ""

# ============ START CLIENTS ============

if [ "$USE_EXEC" = true ]; then
    # === DEVCONTAINER MODE: no viz (binaries hardcoded to localhost), docker exec for curl ===

    echo -e "${CYAN}[IMAGE_STREAMER]${NC} Skipped (hardcoded to localhost)"
    echo -e "${CYAN}[VIZ_CLIENT]${NC} Skipped (hardcoded to localhost)"
    echo -e "${YELLOW}Note: Run from WSL2 for full viz demo${NC}"
    IMAGE_PID=""
    VIZ_PID=""

    echo ""
    echo -e "${GREEN}═══════════════════════════════════════════════════════${NC}"
    echo -e "${GREEN}   DEMO RUNNING (${DEMO_DURATION}s) - Ctrl+C to stop${NC}"
    echo -e "${GREEN}═══════════════════════════════════════════════════════${NC}"
    echo ""

    # ECG via docker exec
    ECG_COUNT=$((DEMO_DURATION / 1))
    for i in $(seq 1 $ECG_COUNT); do
        docker exec cwru-mri-marshal curl -s -X POST http://localhost:8080/v1/bio/signal \
            -H "Content-Type: application/json" \
            -d "{\"source\":\"ecg\",\"data\":[0.1,0.5,0.9],\"rate_hz\":100}" > /dev/null &
        echo -e "  ${CYAN}[ECG]${NC} ✓ Sample #$i"

        # Pose inline (every 2nd iteration = 0.5s interval equiv)
        if [ $((i % 2)) -eq 0 ]; then
            docker exec cwru-mri-marshal curl -s -X POST http://localhost:8080/v1/pose/update \
                -H "Content-Type: application/json" \
                -d "{\"p\":[$i.0,0.0,100.0],\"R\":[1,0,0,0,1,0,0,0,1]}" > /dev/null &
            echo -e "  ${CYAN}[POSE]${NC} ✓ Update #$((i/2))"
        fi

        # Robot inline (every 2nd iteration = 2s interval equiv)
        if [ $((i % 2)) -eq 0 ]; then
            TIMESTAMP=$(date +%s%N)
            docker exec cwru-robot-marshal curl -s -X POST "http://localhost:8081/write/file1.json" \
                -H "Content-Type: application/json" \
                -d "{\"client_id\":\"demo\",\"sent_at\":$TIMESTAMP,\"values\":[{\"i\":$i}]}" > /dev/null &
            echo -e "  ${CYAN}[ROBOT]${NC} ✓ Write #$((i/2))"
        fi

        sleep $ECG_INTERVAL
    done
    ECG_PID=""
    POSE_PID=""
    ROBOT_PID=""

else
    # === WSL2 MODE: localhost + viz ===

    echo -e "${CYAN}[IMAGE_STREAMER]${NC} Starting (${IMAGE_SIZE}x${IMAGE_SIZE}, ${IMAGE_SLICES} slices, ${IMAGE_INTERVAL}s interval)..."
    "$MRI_BUILD/image_streamer" --size $IMAGE_SIZE --slices $IMAGE_SLICES --interval $IMAGE_INTERVAL &
    IMAGE_PID=$!
    sleep 1

    echo -e "${CYAN}[VIZ_CLIENT]${NC} Starting..."
    "$MRI_BUILD/viz_client" &
    VIZ_PID=$!
    sleep 1

    ECG_COUNT=$((DEMO_DURATION / 1))
    echo -e "${CYAN}[ECG_CLIENT]${NC} Starting..."
    python3 "$MRI_MOCKS/ecg_client.py" \
        --endpoint "http://localhost:8080" \
        --count $ECG_COUNT --interval $ECG_INTERVAL --heart-rate 72 &
    ECG_PID=$!

    POSE_COUNT=$(awk "BEGIN {printf \"%d\", $DEMO_DURATION / $POSE_INTERVAL}")
    echo -e "${CYAN}[POSE_CLIENT]${NC} Starting..."
    python3 "$MRI_MOCKS/pose_client.py" \
        --endpoint "http://localhost:8080" \
        --count $POSE_COUNT --interval $POSE_INTERVAL --trajectory circular &
    POSE_PID=$!

    ROBOT_COUNT=$((DEMO_DURATION / ROBOT_INTERVAL))
    echo -e "${CYAN}[ROBOT_CLIENT]${NC} Starting..."
    (
        for i in $(seq 1 $ROBOT_COUNT); do
            TIMESTAMP=$(date +%s%N)
            curl -s -X POST "http://localhost:8081/write/file1.json" \
                -H "Content-Type: application/json" \
                -d "{\"client_id\":\"demo\",\"sent_at\":$TIMESTAMP,\"values\":[{\"i\":$i}]}" > /dev/null
            echo "  [ROBOT] ✓ Write #$i"
            sleep $ROBOT_INTERVAL
        done
    ) &
    ROBOT_PID=$!
fi

echo ""
echo -e "${GREEN}═══════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}   DEMO RUNNING - Press Ctrl+C to stop                  ${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════════${NC}"
echo ""

# Wait
if [ -n "$VIZ_PID" ]; then
    wait $VIZ_PID 2>/dev/null || true
else
    wait $ECG_PID 2>/dev/null || true
fi
