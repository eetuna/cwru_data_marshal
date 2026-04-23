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
echo "Flow: k-space + recon (${KSPACE_SLICES:-5} slices @ ${KSPACE_INTERVAL:-0.04}s)"
echo "Cleanup data: ${CLEANUP_DATA}"
echo "=========================================="

# Stop existing and clean up stale networks
docker compose --env-file .env.demo -f "$COMPOSE_FILE" down 2>/dev/null || true
docker rm -f cwru-viz-client >/dev/null 2>&1 || true
docker network prune -f 2>/dev/null || true

# Create session-data directory and clear ALL old data for fresh stream
mkdir -p "${SESSION_DATA_DIR:-./session-data}/mrd"
rm -rf "${SESSION_DATA_DIR:-./session-data}/mrd/"* 2>/dev/null || true

# Start
echo "Starting services..."
docker compose --env-file .env.demo -f "$COMPOSE_FILE" up -d

echo ""
echo "Checking robot client status..."
sleep 2
for client in catheter-tracking controller planning front-end surface-tracking; do
    if docker ps --format '{{.Names}}' | grep -q "cwru-$client"; then
        echo "  ✓ $client is running"
    else
        echo "  ✗ $client failed - check logs: docker logs cwru-$client"
    fi
done

if [ "$ENABLE_VIZ" = "true" ]; then
    echo ""
    echo "Waiting for first image frame..."
    sleep 3
    echo "Starting visualization client..."
    docker compose --env-file .env.demo -f "$COMPOSE_FILE" --profile viz up -d >/dev/null 2>&1
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
        docker stop -t 1 cwru-viz-client >/dev/null 2>&1 || true
        docker rm -f cwru-viz-client >/dev/null 2>&1 || true
    fi

    docker compose --env-file .env.demo -f "$COMPOSE_FILE" down 2>&1 | grep -v "totally borken" || true
    exit 0
}
trap cleanup INT TERM

# Background monitor for status
monitor_status() {
    sleep 5  # Let things start up
    while true; do
        # MRI Marshal: latest kspace-streamer volume
        if [ "${IMAGE_LOG_STRIDE:-0}" -gt 0 ]; then
            FRAME_LINE=$(docker logs cwru-kspace-streamer --tail 1 2>&1 | grep -E "volume|frame|image" || echo "")
            if [ -n "$FRAME_LINE" ]; then
                echo "$FRAME_LINE"
            fi
        fi

        # Robot Marshal: catheter, controller, planning, front-end, surface (total ops count)
        CATHETER=$(docker logs cwru-catheter-tracking 2>/dev/null | grep -c "CATHETER:")
        CONTROLLER=$(docker logs cwru-controller 2>/dev/null | grep -c "CONTROLLER:")
        PLANNING=$(docker logs cwru-planning 2>/dev/null | grep -c "PLANNING:")
        FRONTEND=$(docker logs cwru-front-end 2>/dev/null | grep -c "FRONTEND:")
        SURFACE=$(docker logs cwru-surface-tracking 2>/dev/null | grep -c "SURFACE:")
        printf "[%s] Robot Marshal: cath=%s ctrl=%s plan=%s fe=%s surf=%s\n" "$(date +%H:%M:%S)" "$CATHETER" "$CONTROLLER" "$PLANNING" "$FRONTEND" "$SURFACE"

        sleep "$MONITOR_INTERVAL"
    done
}

if [ "$DEMO_DURATION" -gt 0 ]; then
    # Start robot ops monitor in background
    monitor_status &
    MONITOR_PID=$!

    # Show MRI data: ECG, pose (no image-streamer spam)
    timeout --foreground "$DEMO_DURATION" docker compose --env-file .env.demo -f "$COMPOSE_FILE" logs -f --no-log-prefix 2>&1 \
        | grep --line-buffered -E "^\[[0-9]" || true

    kill $MONITOR_PID 2>/dev/null || true

    # Graceful shutdown after duration
    cleanup
else
    monitor_status &
    MONITOR_PID=$!

    docker compose --env-file .env.demo -f "$COMPOSE_FILE" logs -f --no-log-prefix 2>&1 \
        | grep --line-buffered -E "^\[[0-9]"
fi
