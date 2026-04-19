#!/bin/bash
# scripts/bench_dump_retention.sh — measure dump (or live) retention via
# the /debug/sinks endpoint, NOT viz_client FPS.
#
# Default: dump mode. Pass DUMP_ENABLE=0 to bench live-mode retention.
#
# Spawns marshal + mock_recon + kspace_streamer (no viz_client), polls
# /debug/sinks every second, then at end compares persisted counts to
# sender counts and reports retention %.
#
# Usage:
#   ./scripts/bench_dump_retention.sh                      # dump mode, 60s, 50Hz
#   DURATION=600 ./scripts/bench_dump_retention.sh         # 10 min
#   KSPACE_INTERVAL=0.020 ./scripts/bench_dump_retention.sh
#   DUMP_ENABLE=0 ./scripts/bench_dump_retention.sh        # live-mode bench

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$PROJECT_DIR/build"

DURATION="${DURATION:-60}"
KSPACE_INTERVAL="${KSPACE_INTERVAL:-0.020}"
HTTP_PORT="${HTTP_PORT:-28080}"
MRD_PORT="${MRD_PORT:-29100}"
RECON_PORT="${RECON_PORT:-29002}"
SLICES="${SLICES:-5}"
DUMP_ENABLE="${DUMP_ENABLE:-1}"

DATA_DIR=$(mktemp -d -t bench_retention.XXXXXX)
LOG_DIR=$(mktemp -d -t bench_retention_logs.XXXXXX)

cleanup() {
    echo ""
    echo "=== stopping processes ==="
    for pid in $KSPACE_PID $MARSHAL_PID $RECON_PID; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    echo "logs kept in: $LOG_DIR"
    echo "data kept in: $DATA_DIR"
}
trap cleanup EXIT

DUMP_FLAG=""
[ "$DUMP_ENABLE" = "1" ] && DUMP_FLAG="--dump"

echo "=== bench_dump_retention ==="
echo "  mode:            $([ "$DUMP_ENABLE" = "1" ] && echo dump || echo live)"
echo "  duration:        ${DURATION}s"
echo "  KSPACE_INTERVAL: ${KSPACE_INTERVAL}s"
echo "  slices:          $SLICES"
echo "  data dir:        $DATA_DIR"
echo "  log dir:         $LOG_DIR"
echo ""

echo "=== starting mock_recon on port $RECON_PORT ==="
python3 "$PROJECT_DIR/docker/mock-recon/mock_recon.py" --port "$RECON_PORT" \
    >"$LOG_DIR/mock_recon.log" 2>&1 &
RECON_PID=$!
sleep 1

echo "=== starting marshal on http=$HTTP_PORT mrd=$MRD_PORT (dump=$DUMP_ENABLE) ==="
"$BUILD_DIR/marshal" \
    --http "0.0.0.0:$HTTP_PORT" \
    --mrd-port "$MRD_PORT" \
    --dump-dir "$DATA_DIR" \
    $DUMP_FLAG \
    --recon-host localhost \
    --recon-port "$RECON_PORT" \
    >"$LOG_DIR/marshal.log" 2>&1 &
MARSHAL_PID=$!
sleep 1

# Wait for /health
for i in $(seq 1 30); do
    if curl -fsS "http://localhost:$HTTP_PORT/health" >/dev/null 2>&1; then
        break
    fi
    sleep 0.5
done

echo "=== starting kspace_streamer (interval=$KSPACE_INTERVAL slices=$SLICES) ==="
"$BUILD_DIR/kspace_streamer" \
    --host localhost --port "$MRD_PORT" \
    --ecg \
    --interval "$KSPACE_INTERVAL" \
    --slices "$SLICES" \
    >"$LOG_DIR/kspace_streamer.log" 2>&1 &
KSPACE_PID=$!

echo "=== sampling /debug/sinks every 1s for ${DURATION}s ==="
SAMPLES_FILE="$LOG_DIR/sinks_samples.jsonl"
( for i in $(seq 1 "$DURATION"); do
    ts=$(date +%s)
    body=$(curl -fsS "http://localhost:$HTTP_PORT/debug/sinks" 2>/dev/null)
    [ -n "$body" ] && echo "{\"t\":$ts,\"sinks\":$body}" >>"$SAMPLES_FILE"
    sleep 1
  done ) &
SAMPLER_PID=$!

sleep "$DURATION"
wait "$SAMPLER_PID" 2>/dev/null || true

echo ""
echo "=== /debug/sinks during scan (spool counts) ==="
SPOOL=$(curl -fsS "http://localhost:$HTTP_PORT/debug/sinks" 2>/dev/null)
echo "$SPOOL" | python3 -m json.tool 2>/dev/null || echo "$SPOOL"

# Stop kspace_streamer cleanly so its volume counter is final.
kill "$KSPACE_PID" 2>/dev/null || true
sleep 1

# The converter runs on marshal shutdown (SIGTERM) in both dump mode
# (DumpRecorder) and live mode (LiveImageRecorder now also spools +
# converts). Snapshot counters first, then SIGTERM, then wait for
# marshal to exit so the on-disk .h5 is present before we inspect.
echo ""
echo "=== /debug/sinks before shutdown (spool counters) ==="
curl -fsS "http://localhost:$HTTP_PORT/debug/sinks" 2>/dev/null | python3 -m json.tool 2>/dev/null || true

echo ""
echo "=== triggering convert via SIGTERM ==="
kill -TERM "$MARSHAL_PID" 2>/dev/null || true
for i in $(seq 1 60); do
    kill -0 "$MARSHAL_PID" 2>/dev/null || break
    sleep 1
done
wait "$MARSHAL_PID" 2>/dev/null || true

# Post-shutdown: parse final h5 files on disk for authoritative counts.
# The spool->HDF5 converter runs during close_scan, so the files are
# complete once the marshal process exits.
echo ""
echo "=== final HDF5 on disk (post-convert) ==="
FINAL=$(python3 - "$DATA_DIR" <<'PY'
import h5py, json, os, sys
from pathlib import Path
root = Path(sys.argv[1])
out = {}
for side_dir in sorted(root.rglob("scan_*.h5")):
    side = side_dir.parent.name
    bucket = side_dir.parent.parent.name
    key = f"{bucket}/{side}"
    try:
        with h5py.File(side_dir, 'r') as f:
            ds = f.get('/dataset')
            acq = len(ds['data']) if ds and 'data' in ds else 0
            wf  = len(ds['waveforms']) if ds and 'waveforms' in ds else 0
            img = 0
            if ds:
                for k in ds.keys():
                    if k.startswith('image_') and 'header' in ds[k]:
                        img += len(ds[k]['header'])
            out[key] = {"acq": acq, "img": img, "wf": wf, "path": str(side_dir)}
    except Exception as e:
        out[key] = {"error": str(e), "path": str(side_dir)}
print(json.dumps(out))
PY
)
echo "$FINAL" | python3 -m json.tool 2>/dev/null || echo "$FINAL"

echo ""
echo "=== sender counts ==="
SCANNER_VOLUMES=$(grep -oE "volume [0-9]+:" "$LOG_DIR/kspace_streamer.log" | tail -1 | grep -oE "[0-9]+")
SCANNER_VOLUMES=${SCANNER_VOLUMES:-0}
SCANNER_ACQS_PER_VOLUME=$((128 * SLICES))
SCANNER_ACQS=$((SCANNER_VOLUMES * SCANNER_ACQS_PER_VOLUME))
SCANNER_WFS=$((SCANNER_VOLUMES * SLICES))
RECON_IMAGES=$(grep -oE "received [0-9]+ reconstructed image" "$LOG_DIR/kspace_streamer.log" | tail -1 | grep -oE "[0-9]+")
RECON_IMAGES=${RECON_IMAGES:-0}

echo "  kspace_streamer volumes sent:  $SCANNER_VOLUMES"
echo "  kspace_streamer acqs sent:     $SCANNER_ACQS  (volumes * 128 * slices)"
echo "  kspace_streamer wfs sent:      $SCANNER_WFS   (volumes * slices)"
echo "  kspace_streamer images back:   $RECON_IMAGES"

echo ""
echo "=== retention ==="
python3 - "$FINAL" "$SCANNER_ACQS" "$SCANNER_WFS" "$RECON_IMAGES" <<'PY'
import json, sys
final = json.loads(sys.argv[1]) if sys.argv[1] else {}
sent_acq = int(sys.argv[2])
sent_wf  = int(sys.argv[3])
sent_recon_img = int(sys.argv[4])

def pct(num, den):
    if den == 0: return "n/a"
    return f"{100.0 * num / den:.2f}%"

mode = final.get("mode", "?")
print(f"  mode: {mode}")

if mode == "dump":
    d = final.get("dump", {})
    fs = d.get("from_scanner", {})
    fr = d.get("from_reconstruction", {})
    # Post-convert counters are the retention metric. spool_records is
    # useful as a "what arrived" signal; converted_* is "what made the
    # final HDF5 artifact".
    print(f"  status                        = {d.get('conversion_status', '?')}")
    print(f"  dump/from_scanner.spool_recs  = {fs.get('spool_records', 0):>10}")
    print(f"  dump/from_scanner.converted_acq = {fs.get('converted_acq', 0):>10}  retention = {pct(fs.get('converted_acq',0), sent_acq)}")
    print(f"  dump/from_scanner.converted_wf  = {fs.get('converted_wf', 0):>10}  retention = {pct(fs.get('converted_wf',0), sent_wf)}")
    print(f"  dump/from_recon.converted_img = {fr.get('converted_img', 0):>10}  retention = {pct(fr.get('converted_img',0), sent_recon_img)}")
    print(f"  dropped_records               = {d.get('dropped_records', 0)}")
    print(f"  dropped_bytes                 = {d.get('dropped_bytes', 0)}")
    print(f"  had_overflow                  = {d.get('had_overflow', False)}")
elif mode == "live":
    l = final.get("live", {})
    fs = l.get("from_scanner", {})
    fr = l.get("from_reconstruction", {})
    print(f"  live/from_scanner.img         = {fs.get('img', 0):>10}")
    print(f"  live/from_scanner.wf          = {fs.get('wf', 0):>10}  retention = {pct(fs.get('wf',0), sent_wf)}")
    print(f"  live/from_scanner.dropped     = {fs.get('dropped', 0)}")
    print(f"  live/from_scanner.queued_jobs = {fs.get('queued_jobs', 0)}  hwm_hit={fs.get('high_watermark_hit', False)}")
    print(f"  live/from_reconstruction.img  = {fr.get('img', 0):>10}  retention = {pct(fr.get('img',0), sent_recon_img)}")
    print(f"  live/from_reconstruction.dropped = {fr.get('dropped', 0)}")
    print(f"  live/from_reconstruction.queued_jobs = {fr.get('queued_jobs', 0)}  hwm_hit={fr.get('high_watermark_hit', False)}")
PY
