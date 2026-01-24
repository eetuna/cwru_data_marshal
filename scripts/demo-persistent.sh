#!/bin/bash
# CWRU Data Marshal - Persistent Demo
# Containers stay running after demo ends. Re-run to monitor again.

cd "$(dirname "$0")/.."

# ===================== CONFIG =====================
set -a
source .env.demo
set +a
# ==================================================

COMPOSE_FILE="docker-compose.demo.yml"

echo "=========================================="
echo "  CWRU Data Marshal - Persistent Demo"
echo "=========================================="

# Check if services are already running
RUNNING=$(docker ps --filter "name=cwru-mri-marshal" --format "{{.Names}}" 2>/dev/null)

if [ -z "$RUNNING" ]; then
    echo "First run - starting all services..."
    echo "Duration: ${DEMO_DURATION}s (then monitoring stops, containers stay)"
    echo "Image: ${IMAGE_WIDTH}x${IMAGE_HEIGHT}x${IMAGE_SLICES} @ ${IMAGE_INTERVAL}ms"
    echo "=========================================="

    # Clean up stale networks
    docker compose --env-file .env.demo -f "$COMPOSE_FILE" down 2>/dev/null || true
    docker network prune -f 2>/dev/null || true

    # Create session-data directory and clear old data
    mkdir -p "${SESSION_DATA_DIR:-./session-data}/mrd"
    rm -rf "${SESSION_DATA_DIR:-./session-data}/mrd/"* 2>/dev/null || true

    # Start all services
    echo "Starting services..."
    docker compose --env-file .env.demo -f "$COMPOSE_FILE" up -d

    echo ""
    echo "Waiting for services to be healthy..."
    sleep 3

    # Check robot clients
    for client in catheter-tracking controller planning front-end surface-tracking; do
        if docker ps --format '{{.Names}}' | grep -q "cwru-$client"; then
            echo "  ✓ $client running"
        else
            echo "  ✗ $client failed"
        fi
    done

    # Start viz if enabled
    if [ "$ENABLE_VIZ" = "true" ]; then
        echo ""
        echo "Starting visualization client..."
        sleep 2
        docker compose --env-file .env.demo -f "$COMPOSE_FILE" --profile viz up -d >/dev/null 2>&1
    fi
else
    echo "Services already running - attaching to monitor..."
    echo "=========================================="
fi

echo ""
echo "Demo running! Press Ctrl+C to stop monitoring (containers keep running)"
echo "To stop containers: docker compose -f docker-compose.demo.yml down"
echo ""

# Cleanup just stops monitoring, NOT containers
cleanup() {
    echo ""
    echo "Stopping monitor... (containers still running)"
    kill $MONITOR_PID 2>/dev/null || true
    echo ""
    echo "Containers are still running. To stop them:"
    echo "  docker compose -f docker-compose.demo.yml down"
    echo ""
    exit 0
}
trap cleanup INT TERM

# Background monitor
monitor_status() {
    sleep 2
    while true; do
        # Frame info if enabled
        if [ "${IMAGE_LOG_STRIDE:-0}" -gt 0 ]; then
            FRAME_LINE=$(docker logs cwru-image-streamer --tail 1 2>&1 | grep "frame" || echo "")
            if [ -n "$FRAME_LINE" ]; then
                echo "$FRAME_LINE"
            fi
        fi

        # Robot ops count
        CATHETER=$(docker logs cwru-catheter-tracking 2>/dev/null | grep -c "CATHETER:")
        CONTROLLER=$(docker logs cwru-controller 2>/dev/null | grep -c "CONTROLLER:")
        PLANNING=$(docker logs cwru-planning 2>/dev/null | grep -c "PLANNING:")
        FRONTEND=$(docker logs cwru-front-end 2>/dev/null | grep -c "FRONTEND:")
        SURFACE=$(docker logs cwru-surface-tracking 2>/dev/null | grep -c "SURFACE:")
        printf "[%s] Robot: cath=%s ctrl=%s plan=%s fe=%s surf=%s\n" "$(date +%H:%M:%S)" "$CATHETER" "$CONTROLLER" "$PLANNING" "$FRONTEND" "$SURFACE"

        sleep "$MONITOR_INTERVAL"
    done
}

if [ "$DEMO_DURATION" -gt 0 ]; then
    monitor_status &
    MONITOR_PID=$!

    # Show ECG/pose logs for duration
    timeout --foreground "$DEMO_DURATION" docker compose --env-file .env.demo -f "$COMPOSE_FILE" logs -f --no-log-prefix 2>&1 \
        | grep --line-buffered -E "^\[[0-9]" || true

    kill $MONITOR_PID 2>/dev/null || true

    echo ""
    echo "=========================================="
    echo "Demo duration complete. Containers still running."
    echo ""
    echo "Re-run this script to monitor again."
    echo "To stop: docker compose -f docker-compose.demo.yml down"
    echo "=========================================="
else
    monitor_status &
    MONITOR_PID=$!

    docker compose --env-file .env.demo -f "$COMPOSE_FILE" logs -f --no-log-prefix 2>&1 \
        | grep --line-buffered -E "^\[[0-9]"
fi
