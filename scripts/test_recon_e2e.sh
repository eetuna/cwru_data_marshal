#!/bin/bash
# End-to-end test of the python-ismrmrd-server recon path.
#
# Starts recon-sim (docker) and marshal (native), runs the scanner k-space
# simulator, verifies a reconstructed image lands at GET /v1/mrd/latest,
# and tears everything down.
#
# Usage: scripts/test_recon_e2e.sh
#
# Exits 0 on success, non-zero with a loud failure message otherwise.

set -euo pipefail

WORKTREE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$WORKTREE"

RECON_CONTAINER="recon-sim-e2e"
MARSHAL_LOG="/tmp/marshal-e2e.log"
DATA_DIR="/tmp/marshal-e2e-run"
MARSHAL_PID=""

# --- cleanup on any exit ---------------------------------------------------
cleanup() {
    local exit_code=$?
    echo
    echo "=== cleanup ==="
    # Dump recon-sim logs BEFORE tearing down the container.
    docker logs "$RECON_CONTAINER" > /tmp/recon-sim-e2e.log 2>&1 || true
    if [[ -n "$MARSHAL_PID" ]] && kill -0 "$MARSHAL_PID" 2>/dev/null; then
        kill "$MARSHAL_PID" 2>/dev/null || true
        wait "$MARSHAL_PID" 2>/dev/null || true
    fi
    docker rm -f "$RECON_CONTAINER" >/dev/null 2>&1 || true
    rm -rf "$DATA_DIR"
    if [[ "$exit_code" -ne 0 ]]; then
        echo "=== FAILED (exit=$exit_code) ==="
        echo "--- marshal log ---"
        cat "$MARSHAL_LOG" 2>/dev/null || true
        echo
        echo "--- recon-sim log ---"
        cat /tmp/recon-sim-e2e.log 2>/dev/null || true
    fi
    exit "$exit_code"
}
trap cleanup EXIT INT TERM

# --- prerequisites ---------------------------------------------------------
echo "=== prerequisites ==="
[[ -x ./build/marshal ]] || { echo "ERROR: ./build/marshal not built"; exit 2; }
docker image inspect mri_data_marshal-recon-sim:latest >/dev/null 2>&1 \
    || { echo "ERROR: recon-sim image not built. run: docker compose build recon-sim"; exit 2; }

# Devcontainer's IP on the default bridge - recon-sim resolves mri-marshal
# back to this when POSTing the callback.
HOST_IP=$(hostname -I | awk '{print $1}')
echo "devcontainer IP (for callback): $HOST_IP"

# --- start recon-sim -------------------------------------------------------
echo
echo "=== starting recon-sim ==="
docker rm -f "$RECON_CONTAINER" >/dev/null 2>&1 || true
docker run -d --name "$RECON_CONTAINER" \
    --add-host "mri-marshal:$HOST_IP" \
    mri_data_marshal-recon-sim:latest >/dev/null

# Wait for it to be healthy
for i in $(seq 1 20); do
    status=$(docker inspect "$RECON_CONTAINER" --format '{{.State.Health.Status}}' 2>/dev/null || echo starting)
    if [[ "$status" == "healthy" ]]; then
        break
    fi
    sleep 0.5
done
if [[ "$status" != "healthy" ]]; then
    echo "ERROR: recon-sim not healthy after 10s"
    docker logs "$RECON_CONTAINER" 2>&1 | tail -30
    exit 3
fi
RECON_IP=$(docker inspect "$RECON_CONTAINER" --format '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
echo "recon-sim up at $RECON_IP:9003 (healthy)"

# --- start marshal ---------------------------------------------------------
echo
echo "=== starting marshal ==="
rm -rf "$DATA_DIR" && mkdir -p "$DATA_DIR"
./build/marshal \
    --http 0.0.0.0:8080 \
    --ws 0.0.0.0:8090 \
    --data "$DATA_DIR" \
    --sink mrd \
    --recon-endpoint "http://$RECON_IP:9003" \
    > "$MARSHAL_LOG" 2>&1 &
MARSHAL_PID=$!
echo "marshal pid=$MARSHAL_PID"

# Wait for health
for i in $(seq 1 20); do
    if curl -sf http://localhost:8080/health >/dev/null 2>&1; then
        break
    fi
    sleep 0.5
done
if ! curl -sf http://localhost:8080/health >/dev/null 2>&1; then
    echo "ERROR: marshal not healthy after 10s"
    tail -30 "$MARSHAL_LOG"
    exit 4
fi
echo "marshal up and healthy on :8080"

# --- run scanner simulator -------------------------------------------------
echo
echo "=== running scanner k-space simulator ==="
python3 clients/mocks/scanner_kspace_client.py \
    --marshal http://localhost:8080 \
    --stream e2e_test \
    --matrix 128 \
    --slices 3 \
    --coils 1 \
    --frames 1 2>&1

# --- wait for recon callback to store the image ----------------------------
echo
echo "=== waiting for reconstructed image in marshal store ==="
FRAME_INDEX=""
for i in $(seq 1 20); do
    resp=$(curl -s http://localhost:8080/v1/mrd/latest 2>/dev/null || echo "")
    # Look for a stream id that matches our test
    if echo "$resp" | grep -q '"stream_id":"e2e_test"' 2>/dev/null \
       || echo "$resp" | grep -q '"stream":"e2e_test"' 2>/dev/null; then
        FRAME_INDEX=$(echo "$resp" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',d).get('frame_index',d.get('data',{}).get('frame_index','?')))" 2>/dev/null || echo "?")
        echo "reconstructed image present: $resp" | head -c 600
        echo
        break
    fi
    sleep 0.5
done

if [[ -z "$FRAME_INDEX" ]]; then
    echo "ERROR: no reconstructed image arrived within 10s"
    echo "--- last /v1/mrd/latest ---"
    curl -s http://localhost:8080/v1/mrd/latest || true
    echo
    echo "--- marshal log (tail) ---"
    tail -50 "$MARSHAL_LOG"
    echo
    echo "--- recon-sim log (tail) ---"
    docker logs "$RECON_CONTAINER" 2>&1 | tail -50
    exit 5
fi

# --- verify HDF5 actually has data ----------------------------------------
echo
echo "=== verifying HDF5 store on disk ==="
H5_FILE=$(find "$DATA_DIR" \( -name '*.h5' -o -name '*.mrd' \) -type f | head -n 1 || true)
if [[ -z "$H5_FILE" ]]; then
    echo "ERROR: no .h5 file in $DATA_DIR"
    ls -la "$DATA_DIR"
    exit 6
fi
H5_SIZE=$(stat -c %s "$H5_FILE")
echo "HDF5 file: $H5_FILE ($H5_SIZE bytes)"
if [[ "$H5_SIZE" -lt 1000 ]]; then
    echo "ERROR: HDF5 file is suspiciously small"
    exit 7
fi

echo
echo "=== PASS ==="
echo "k-space -> marshal -> recon-sim -> callback -> HDF5 round trip succeeded."
exit 0
