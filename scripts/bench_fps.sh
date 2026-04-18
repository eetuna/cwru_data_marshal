#!/bin/bash
# scripts/bench_fps.sh — Native FPS benchmark for the MRI marshal pipeline
#
# Spawns marshal + mock_recon + kspace_streamer + viz_client as host processes
# (no docker), runs for DURATION seconds, collects [FPS DEBUG] samples from
# viz_client stderr, and prints mean/median/min/max/count.
#
# Usage:
#   ./scripts/bench_fps.sh                 # 30s run, default interval
#   DURATION=60 ./scripts/bench_fps.sh     # 60s run
#   KSPACE_INTERVAL=0.05 ./scripts/bench_fps.sh
#   VIZ_INTERVAL=0.033 ./scripts/bench_fps.sh
#
# Prereqs: cmake --build build

# set -e disabled: readiness loop uses curl which fails until marshal is ready
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$PROJECT_DIR/build"

DURATION="${DURATION:-30}"
KSPACE_INTERVAL="${KSPACE_INTERVAL:-0.033}"
VIZ_INTERVAL="${VIZ_INTERVAL:-0.033}"
HTTP_PORT="${HTTP_PORT:-28080}"
MRD_PORT="${MRD_PORT:-29100}"
RECON_PORT="${RECON_PORT:-29002}"

DATA_DIR=$(mktemp -d -t bench_fps.XXXXXX)
LOG_DIR=$(mktemp -d -t bench_fps_logs.XXXXXX)

cleanup() {
    echo ""
    echo "=== stopping processes ==="
    for pid in $VIZ_PID $KSPACE_PID $MARSHAL_PID $RECON_PID; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    echo "logs kept in: $LOG_DIR"
    echo "data kept in: $DATA_DIR"
}
trap cleanup EXIT

echo "=== bench_fps ==="
echo "  duration:        ${DURATION}s"
echo "  KSPACE_INTERVAL: ${KSPACE_INTERVAL}s"
echo "  VIZ_INTERVAL:    ${VIZ_INTERVAL}s"
echo "  ports:           http=$HTTP_PORT mrd=$MRD_PORT recon=$RECON_PORT"
echo "  data dir:        $DATA_DIR"
echo "  log dir:         $LOG_DIR"
echo ""

echo "=== starting mock_recon on port $RECON_PORT ==="
python3 "$PROJECT_DIR/docker/mock-recon/mock_recon.py" --port "$RECON_PORT" \
    >"$LOG_DIR/mock_recon.log" 2>&1 &
RECON_PID=$!
sleep 1

echo "=== starting marshal on http=$HTTP_PORT mrd=$MRD_PORT ==="
"$BUILD_DIR/marshal" \
    --http "0.0.0.0:$HTTP_PORT" \
    --mrd-port "$MRD_PORT" \
    --dump-dir "$DATA_DIR" \
    --recon-host localhost \
    --recon-port "$RECON_PORT" \
    >"$LOG_DIR/marshal.log" 2>&1 &
MARSHAL_PID=$!
sleep 1

echo "=== starting kspace_streamer (interval=$KSPACE_INTERVAL) ==="
"$BUILD_DIR/kspace_streamer" \
    --host localhost --port "$MRD_PORT" \
    --ecg \
    --interval "$KSPACE_INTERVAL" \
    --slices 5 \
    >"$LOG_DIR/kspace_streamer.log" 2>&1 &
KSPACE_PID=$!

echo "=== waiting for first image to be published ==="
for i in $(seq 1 30); do
    path=$(curl -fsS "http://localhost:$HTTP_PORT/image/latest" 2>/dev/null | python3 -c 'import sys,json; print(json.load(sys.stdin).get("path",""))' 2>/dev/null)
    if [ -n "$path" ] && [ -f "$path" ]; then
        # Ensure it's fully written: non-zero size
        size=$(stat -c '%s' "$path" 2>/dev/null || echo 0)
        [ "$size" -gt 1024 ] && echo "  first image ready: $path ($size bytes)" && break
    fi
    sleep 0.5
done

echo "=== starting viz_client (interval=$VIZ_INTERVAL) ==="
"$BUILD_DIR/viz_client" \
    --http "http://localhost:$HTTP_PORT" \
    --interval "$VIZ_INTERVAL" \
    >"$LOG_DIR/viz_client.log" 2>&1 &
VIZ_PID=$!

echo ""
echo "=== running for ${DURATION}s ==="
sleep "$DURATION"

echo ""
echo "=== results ==="
python3 - "$LOG_DIR/viz_client.log" <<'PY'
import re, sys, statistics
path = sys.argv[1]
vals = []
with open(path) as f:
    for line in f:
        m = re.search(r"FPS:\s*([\d.]+)", line)
        if m:
            vals.append(float(m.group(1)))
if not vals:
    print("  no FPS samples collected"); sys.exit(1)
# skip the first sample — usually 0 while pipeline warms up
vals = vals[1:] if len(vals) > 1 else vals
print(f"  samples: {len(vals)}")
print(f"  mean:    {statistics.mean(vals):.2f}")
print(f"  median:  {statistics.median(vals):.2f}")
print(f"  min:     {min(vals):.2f}")
print(f"  max:     {max(vals):.2f}")
if len(vals) > 1:
    print(f"  stdev:   {statistics.stdev(vals):.2f}")
PY
