#!/bin/bash
# CWRU Data Marshal - Docker Demo

cd "$(dirname "$0")/.."

# ===================== CONFIG =====================
# Match original demo performance: 50ms = 20fps (original was 50ms @ 128x128)
# Docker overhead adds ~10-20ms latency, so use faster interval to compensate
DEMO_DURATION=30
export IMAGE_WIDTH=64
export IMAGE_HEIGHT=64
export IMAGE_SLICES=5
export IMAGE_INTERVAL_MS=50          # 50ms = 20fps target (matches original)
export ECG_INTERVAL= 1              # 500ms (matches original ECG_INTERVAL_MS=500)
export ECG_HEART_RATE=72
export POSE_INTERVAL= 1            # 100ms = 10Hz for smooth trajectory
export POSE_TRAJECTORY=circular
export DISPLAY=${DISPLAY:-:0}
ENABLE_VIZ=true
CLEANUP_DATA=true                    # Remove mri-data volume after demo (set false to keep data)
MONITOR_INTERVAL=2  # Print robot client ops count every N seconds
# ==================================================

COMPOSE_FILE="docker-compose.demo.yml"

echo "=========================================="
echo "  CWRU Data Marshal - Docker Demo"
echo "=========================================="
echo "Duration: ${DEMO_DURATION}s"
echo "Image: ${IMAGE_WIDTH}x${IMAGE_HEIGHT}x${IMAGE_SLICES} @ ${IMAGE_INTERVAL_MS}ms"
echo "Cleanup data: ${CLEANUP_DATA}"
echo "=========================================="

# Stop existing and clean up stale networks
docker compose -f "$COMPOSE_FILE" down 2>/dev/null || true
docker rm -f cwru-viz-client 2>/dev/null || true
docker network prune -f 2>/dev/null || true

# Clean up old demo data to start fresh
if [ "$CLEANUP_DATA" = "true" ]; then
    docker volume rm cwru_data_marshal_mri-data 2>/dev/null || true
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
    docker kill cwru-viz-client 2>/dev/null || true
    docker compose -f "$COMPOSE_FILE" down

    if [ "$CLEANUP_DATA" = "true" ]; then
        echo "Cleaning up mri-data volume..."
        docker volume rm cwru_data_marshal_mri-data 2>/dev/null || true
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

if [ "$CLEANUP_DATA" = "true" ]; then
    echo "Cleaning up mri-data volume..."
    docker volume rm cwru_data_marshal_mri-data 2>/dev/null || true
fi
