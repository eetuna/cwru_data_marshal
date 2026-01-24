#!/bin/bash
# CWRU Data Marshal - Docker Demo

cd "$(dirname "$0")/.."

# ===================== CONFIG =====================
# Load all configuration from .env.demo (used by docker-compose and this script)
set -a  # Auto-export all variables
source .env.demo
set +a
# ==================================================

COMPOSE_FILE="docker-compose.demo.yml"

echo "=========================================="
echo "  CWRU Data Marshal - Docker Demo"
echo "=========================================="
echo "Duration: ${DEMO_DURATION}s"
echo "Image: ${IMAGE_WIDTH}x${IMAGE_HEIGHT}x${IMAGE_SLICES} @ ${IMAGE_INTERVAL}ms"
echo "Cleanup data: ${CLEANUP_DATA}"
echo "=========================================="

# Stop existing and clean up stale networks
docker compose -f "$COMPOSE_FILE" down 2>/dev/null || true
docker rm -f cwru-viz-client 2>/dev/null || true
docker network prune -f 2>/dev/null || true

# Create session-data directory
mkdir -p session-data

# Clean up old demo data to start fresh
if [ "$CLEANUP_DATA" = "true" ]; then
    rm -rf session-data/*
fi

# Start
echo "Starting services..."
if [ "$ENABLE_VIZ" = "true" ]; then
    docker compose -f "$COMPOSE_FILE" --profile viz up -d
else
    docker compose -f "$COMPOSE_FILE" up -d
fi

echo ""
echo "Demo running! Press Ctrl+C to stop."
echo ""

# Cleanup on Ctrl+C - force kill viz-client to close GUI
cleanup() {
    echo ""
    echo "Stopping..."
    kill $MONITOR_PID 2>/dev/null || true

    # Force stop viz-client to close GUI immediately
    if docker ps --format '{{.Names}}' | grep -q "cwru-viz-client"; then
        echo "Closing visualization window..."
        docker stop -t 1 cwru-viz-client 2>/dev/null || true
        docker rm -f cwru-viz-client 2>/dev/null || true
    fi

    docker compose -f "$COMPOSE_FILE" down

    if [ "$CLEANUP_DATA" = "true" ]; then
        echo "Cleaning up demo data..."
        rm -rf session-data/*
    fi

    exit 0
}
trap cleanup INT TERM

# Background monitor for robot client operations
monitor_robot_ops() {
    sleep 5  # Let things start up
    while true; do
        TOTAL=$(docker compose -f "$COMPOSE_FILE" logs --tail 200 2>&1 | grep -c "CATHETER:" || echo 0)
        echo "[$(date +%H:%M:%S)] Robot ops: $TOTAL"
        sleep "$MONITOR_INTERVAL"
    done
}

if [ "$DEMO_DURATION" -gt 0 ]; then
    # Start robot ops monitor in background
    monitor_robot_ops &
    MONITOR_PID=$!

    # Show MRI data: ECG, pose, images (skip robot catheter spam)
    timeout --foreground "$DEMO_DURATION" docker compose -f "$COMPOSE_FILE" logs -f 2>&1 \
        | grep --line-buffered -E "ecg-client.*\[|pose-client.*\[|image-streamer.*sent frame" \
        | sed -u 's/^.*ecg-client[^[]*\(\[[^]]*\] HR=.*\)$/ECG:  \1/' \
        | sed -u 's/^.*pose-client[^[]*\(\[[^]]*\] p=.*\)$/POSE: \1/' \
        | sed -u 's/^.*image-streamer.*sent frame \([0-9]*\).*/IMG:  frame \1/' || true

    kill $MONITOR_PID 2>/dev/null || true
else
    monitor_robot_ops &
    MONITOR_PID=$!

    docker compose -f "$COMPOSE_FILE" logs -f 2>&1 \
        | grep --line-buffered -E "ecg-client.*\[|pose-client.*\[|image-streamer.*sent frame" \
        | sed -u 's/^.*ecg-client[^[]*\(\[[^]]*\] HR=.*\)$/ECG:  \1/' \
        | sed -u 's/^.*pose-client[^[]*\(\[[^]]*\] p=.*\)$/POSE: \1/' \
        | sed -u 's/^.*image-streamer.*sent frame \([0-9]*\).*/IMG:  frame \1/'
fi

echo ""
echo "Demo complete."
docker compose -f "$COMPOSE_FILE" down

# Extra cleanup for viz-client if it's still running
if docker ps --format '{{.Names}}' | grep -q "cwru-viz-client"; then
    echo "Closing visualization window..."
    docker stop -t 1 cwru-viz-client 2>/dev/null || true
    docker rm -f cwru-viz-client 2>/dev/null || true
fi

if [ "$CLEANUP_DATA" = "true" ]; then
    echo "Cleaning up demo data..."
    rm -rf session-data/*
fi
