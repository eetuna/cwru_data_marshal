#!/bin/bash
# scripts/run_demo_docker.sh
# Docker Demo - Uses Docker Compose marshals with Python mock clients
#
# Works from both:
#   - WSL2/Host: Uses localhost directly
#   - Devcontainer: Uses docker exec to reach containers
#
# Prerequisites:
#   1. Docker containers running: docker compose up -d
#   2. MRI worktree exists: ../mri_data_marshal_worktree

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT_DIR"

# Paths
MRI_WORKTREE="../mri_data_marshal_worktree"
ECG_CLIENT="$MRI_WORKTREE/clients/mocks/ecg_client.py"
POSE_CLIENT="$MRI_WORKTREE/clients/mocks/pose_client.py"

# Configuration
MRI_HTTP=8080
MRI_WS=8090
ROBOT_HTTP=8081
DEMO_DURATION_SEC=30

# Container names
MRI_CONTAINER="cwru-mri-marshal"
ROBOT_CONTAINER="cwru-robot-marshal"

# Colors
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# Detect environment and set endpoint
detect_environment() {
    # Try localhost first (works in WSL2/host)
    if curl -sf --max-time 1 http://localhost:$MRI_HTTP/health > /dev/null 2>&1; then
        USE_DOCKER_EXEC=false
        MRI_ENDPOINT="http://localhost:$MRI_HTTP"
        ROBOT_ENDPOINT="http://localhost:$ROBOT_HTTP"
        ENV_NAME="WSL2/Host"
    # Fall back to docker exec (works in devcontainer)
    elif docker exec $MRI_CONTAINER curl -sf --max-time 1 http://localhost:$MRI_HTTP/health > /dev/null 2>&1; then
        USE_DOCKER_EXEC=true
        MRI_ENDPOINT="http://localhost:$MRI_HTTP"
        ROBOT_ENDPOINT="http://localhost:$ROBOT_HTTP"
        ENV_NAME="Devcontainer (via docker exec)"
    else
        return 1
    fi
    return 0
}

# Wrapper for curl that works in both environments
do_curl() {
    if [ "$USE_DOCKER_EXEC" = true ]; then
        docker exec $MRI_CONTAINER curl "$@"
    else
        curl "$@"
    fi
}

do_curl_robot() {
    if [ "$USE_DOCKER_EXEC" = true ]; then
        docker exec $ROBOT_CONTAINER curl "$@"
    else
        curl "$@"
    fi
}

header() {
    clear 2>/dev/null || true
    echo -e "${CYAN}================================================================${NC}"
    echo -e "${CYAN}   CWRU DATA MARSHAL - DOCKER DEMO                             ${NC}"
    echo -e "${CYAN}================================================================${NC}"
    echo ""
}

cleanup() {
    echo ""
    echo -e "${YELLOW}[CLEANUP] Stopping demo processes...${NC}"
    [ -n "$ECG_PID" ] && kill $ECG_PID 2>/dev/null || true
    [ -n "$POSE_PID" ] && kill $POSE_PID 2>/dev/null || true
    [ -n "$ROBOT_PID" ] && kill $ROBOT_PID 2>/dev/null || true
    echo -e "${GREEN}✓ Demo processes stopped${NC}"
    echo ""
    echo -e "${YELLOW}Note: Docker containers are still running.${NC}"
    echo "  Stop with: docker compose down"
}

trap cleanup EXIT

# ============================================================================
# INTRO
# ============================================================================
header
echo -e "${GREEN}Docker Demo${NC}"
echo ""
echo "This demo uses Docker containers for the marshals:"
echo "  • MRI Marshal:   port $MRI_HTTP (HTTP), port $MRI_WS (WS)"
echo "  • Robot Marshal: port $ROBOT_HTTP (HTTP)"
echo ""
echo "Python mock clients send data to the containerized marshals."
echo ""

# ============================================================================
# CHECK PREREQUISITES
# ============================================================================
echo -e "${CYAN}[CHECK] Verifying prerequisites...${NC}"

# Check mock clients exist
if [ ! -f "$ECG_CLIENT" ]; then
    echo -e "${RED}✗ ECG client not found: $ECG_CLIENT${NC}"
    echo "  Create worktree: git worktree add ../mri_data_marshal_worktree mri-data-marhsal"
    exit 1
fi
echo "  ✓ ECG client found"

if [ ! -f "$POSE_CLIENT" ]; then
    echo -e "${RED}✗ Pose client not found: $POSE_CLIENT${NC}"
    exit 1
fi
echo "  ✓ Pose client found"

# Detect environment and check containers
echo -n "  Detecting environment... "
if detect_environment; then
    echo -e "${GREEN}$ENV_NAME${NC}"
else
    echo -e "${RED}✗ Containers not reachable${NC}"
    echo ""
    echo "Start Docker containers first:"
    echo "  docker compose up -d"
    exit 1
fi

echo -e "  ✓ MRI Marshal reachable"

# Check Robot Marshal
echo -n "  Checking Robot Marshal... "
if [ "$USE_DOCKER_EXEC" = true ]; then
    if docker exec $ROBOT_CONTAINER curl -sf --max-time 1 http://localhost:$ROBOT_HTTP/ > /dev/null 2>&1; then
        echo -e "${GREEN}✓ Running${NC}"
    else
        echo -e "${YELLOW}⚠ Not responding${NC}"
    fi
else
    if curl -sf --max-time 1 http://localhost:$ROBOT_HTTP/ > /dev/null 2>&1; then
        echo -e "${GREEN}✓ Running${NC}"
    else
        echo -e "${YELLOW}⚠ Not responding${NC}"
    fi
fi

echo ""
echo -e "${GREEN}✓ All prerequisites met${NC}"
echo ""
echo "→ Press ENTER to start demo (${DEMO_DURATION_SEC}s)..."
read -r

# ============================================================================
# RUN DEMO
# ============================================================================
header
echo -e "${GREEN}[DEMO] Running simultaneous data streams${NC}"
echo -e "Environment: ${CYAN}$ENV_NAME${NC}"
echo "───────────────────────────────────────────────────────"
echo ""

# Calculate counts based on duration
ECG_INTERVAL=1.0
POSE_INTERVAL=0.5
ECG_COUNT=$(echo "$DEMO_DURATION_SEC / $ECG_INTERVAL" | bc)
POSE_COUNT=$(echo "$DEMO_DURATION_SEC / $POSE_INTERVAL" | bc)

echo "Starting mock clients..."
echo "  • ECG:  $ECG_COUNT samples @ ${ECG_INTERVAL}s interval"
echo "  • Pose: $POSE_COUNT updates @ ${POSE_INTERVAL}s interval"
echo ""

# For devcontainer, we need to run clients differently
if [ "$USE_DOCKER_EXEC" = true ]; then
    # Copy mock clients to MRI container and run there
    echo -e "${YELLOW}Running clients via docker exec...${NC}"

    # ECG client via docker exec
    echo -e "${CYAN}[ECG CLIENT]${NC}"
    (
        for i in $(seq 1 $ECG_COUNT); do
            docker exec $MRI_CONTAINER curl -s -X POST http://localhost:$MRI_HTTP/v1/bio/signal \
                -H "Content-Type: application/json" \
                -d "{\"source\":\"ecg_monitor\",\"data\":[0.5,0.6,0.7],\"rate_hz\":100.0}" > /dev/null 2>&1
            echo "  [ECG] ✓ Sample #$i"
            sleep $ECG_INTERVAL
        done
    ) &
    ECG_PID=$!

    sleep 0.5

    # Pose client via docker exec
    echo -e "${CYAN}[POSE CLIENT]${NC}"
    (
        for i in $(seq 1 $POSE_COUNT); do
            docker exec $MRI_CONTAINER curl -s -X POST http://localhost:$MRI_HTTP/v1/pose/update \
                -H "Content-Type: application/json" \
                -d "{\"p\":[$i.0,0.0,100.0],\"R\":[1,0,0,0,1,0,0,0,1]}" > /dev/null 2>&1
            if [ $((i % 10)) -eq 0 ]; then
                echo "  [POSE] ✓ Update #$i"
            fi
            sleep $POSE_INTERVAL
        done
    ) &
    POSE_PID=$!

    # Robot writes via docker exec
    echo -e "${CYAN}[ROBOT MARSHAL]${NC}"
    (
        for i in $(seq 1 $((DEMO_DURATION_SEC / 2))); do
            TIMESTAMP=$(date +%s%N)
            docker exec $ROBOT_CONTAINER curl -s -X POST "http://localhost:$ROBOT_HTTP/write/file1.json" \
                -H "Content-Type: application/json" \
                -d "{\"client_id\":\"demo\",\"sent_at\":$TIMESTAMP,\"values\":[{\"iteration\":$i}]}" > /dev/null 2>&1
            echo "  [ROBOT] ✓ Write #$i to file1.json"
            sleep 2
        done
    ) &
    ROBOT_PID=$!
else
    # Direct localhost access (WSL2/Host)

    # Start ECG client in background
    echo -e "${CYAN}[ECG CLIENT]${NC}"
    python3 "$ECG_CLIENT" \
        --endpoint "http://localhost:$MRI_HTTP" \
        --count $ECG_COUNT \
        --interval $ECG_INTERVAL \
        --heart-rate 72 \
        --samples 100 &
    ECG_PID=$!

    sleep 0.5

    # Start Pose client in background
    echo ""
    echo -e "${CYAN}[POSE CLIENT]${NC}"
    python3 "$POSE_CLIENT" \
        --endpoint "http://localhost:$MRI_HTTP" \
        --count $POSE_COUNT \
        --interval $POSE_INTERVAL \
        --trajectory circular \
        --radius 50 &
    POSE_PID=$!

    # Robot Marshal test - periodic writes
    echo ""
    echo -e "${CYAN}[ROBOT MARSHAL]${NC}"
    (
        for i in $(seq 1 $((DEMO_DURATION_SEC / 2))); do
            TIMESTAMP=$(date +%s%N)
            curl -s -X POST "http://localhost:$ROBOT_HTTP/write/file1.json" \
                -H "Content-Type: application/json" \
                -d "{\"client_id\":\"demo\",\"sent_at\":$TIMESTAMP,\"values\":[{\"iteration\":$i}]}" > /dev/null 2>&1
            echo "  [ROBOT] ✓ Write #$i to file1.json"
            sleep 2
        done
    ) &
    ROBOT_PID=$!
fi

echo ""
echo "═══════════════════════════════════════════════════════"
echo "          LIVE DATA FLOW (${DEMO_DURATION_SEC} seconds)"
echo "═══════════════════════════════════════════════════════"
echo ""

# Wait for clients to finish
wait $ECG_PID 2>/dev/null || true
wait $POSE_PID 2>/dev/null || true
wait $ROBOT_PID 2>/dev/null || true

# ============================================================================
# SUMMARY
# ============================================================================
echo ""
echo "═══════════════════════════════════════════════════════"
header
echo -e "${GREEN}[SUMMARY]${NC}"
echo "───────────────────────────────────────────────────────"
echo ""

echo "MRI Marshal Health:"
do_curl -s http://localhost:$MRI_HTTP/health 2>/dev/null | python3 -m json.tool 2>/dev/null || echo "  (no response)"
echo ""

echo "Robot Marshal State (file1.json):"
do_curl_robot -s http://localhost:$ROBOT_HTTP/read/file1.json 2>/dev/null | python3 -m json.tool 2>/dev/null || echo "  (no data)"
echo ""

echo "═══════════════════════════════════════════════════════"
echo -e "${GREEN}         ✓ DOCKER DEMO COMPLETE${NC}"
echo "═══════════════════════════════════════════════════════"
echo ""
echo "Docker containers are still running. To stop:"
echo "  docker compose down"
echo ""
echo "To view logs:"
echo "  docker compose logs -f"
echo ""
