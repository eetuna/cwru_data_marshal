#!/bin/bash
# Start recon-sim (docker) and marshal (native) and leave them running.
# Ctrl-C to stop and clean up.
#
# Use scripts/test_recon_e2e.sh for the scripted non-interactive version.

set -euo pipefail

WORKTREE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$WORKTREE"

RECON_CONTAINER="recon-sim-stack"
MARSHAL_LOG="/tmp/marshal-stack.log"
DATA_DIR="${DATA_DIR:-/tmp/marshal-stack-run}"
MARSHAL_PID=""

cleanup() {
    echo
    echo "=== stopping ==="
    [[ -n "$MARSHAL_PID" ]] && kill "$MARSHAL_PID" 2>/dev/null || true
    docker rm -f "$RECON_CONTAINER" >/dev/null 2>&1 || true
    exit 0
}
trap cleanup INT TERM EXIT

[[ -x ./build/marshal ]] || { echo "ERROR: ./build/marshal not built"; exit 2; }
docker image inspect mri_data_marshal-recon-sim:latest >/dev/null 2>&1 \
    || { echo "ERROR: run 'docker compose build recon-sim' first"; exit 2; }

HOST_IP=$(hostname -I | awk '{print $1}')
echo "devcontainer IP: $HOST_IP"

docker rm -f "$RECON_CONTAINER" >/dev/null 2>&1 || true
docker run -d --name "$RECON_CONTAINER" \
    --add-host "mri-marshal:$HOST_IP" \
    mri_data_marshal-recon-sim:latest >/dev/null
for i in $(seq 1 20); do
    status=$(docker inspect "$RECON_CONTAINER" --format '{{.State.Health.Status}}' 2>/dev/null || echo starting)
    [[ "$status" == "healthy" ]] && break
    sleep 0.5
done
RECON_IP=$(docker inspect "$RECON_CONTAINER" --format '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
echo "recon-sim up at $RECON_IP:9002"

rm -rf "$DATA_DIR" && mkdir -p "$DATA_DIR"
./build/marshal \
    --http 0.0.0.0:8080 \
    --ws 0.0.0.0:8090 \
    --data "$DATA_DIR" \
    --sink mrd \
    --recon-endpoint "http://$RECON_IP:9002" \
    > "$MARSHAL_LOG" 2>&1 &
MARSHAL_PID=$!
for i in $(seq 1 20); do
    curl -sf http://localhost:8080/health >/dev/null 2>&1 && break
    sleep 0.5
done
echo "marshal up on :8080, data dir = $DATA_DIR, log = $MARSHAL_LOG"
echo
echo "=== ready ==="
echo "Send k-space:   python3 clients/mocks/scanner_kspace_client.py --matrix 128"
echo "Run viz:        ./build/viz_client --http http://localhost:8080/v1/mrd/latest"
echo "Tail marshal:   tail -f $MARSHAL_LOG"
echo
echo "Ctrl-C to stop."
wait "$MARSHAL_PID"
