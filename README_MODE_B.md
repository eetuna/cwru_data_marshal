set -euo pipefail

# 0) Build
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"

# 1) Clean data + make folders (safe for mounts)
mkdir -p ./data
find ./data -mindepth 1 -maxdepth 1 -exec rm -rf {} +
mkdir -p ./data/mrd ./data/dumpbox ./data/segments

# -------------------------------
# PHASE 1 — RECORD to dumpbox
# -------------------------------

# 2) Stop any old server
pkill -f "/build/marshal" >/dev/null 2>&1 || true

# 3) Start marshal in DUMPBOX sink (session auto-named)
#    WS ingest + RB/LVC + segment writer also enabled during recording.
./build/marshal \
  --http 0.0.0.0:8080 \
  --ws   0.0.0.0:8090 \
  --data ./data \
  --sink dumpbox \
  --dumpbox-root ./data/dumpbox \
  --ws-ingest on \
  --rb-capacity 2048 \
  --lvc-capacity 256 \
  --segment-root ./data/segments \
  --segment-roll-bytes $((64*1024*1024)) \
  --segment-roll-seconds 30 \
  > ./data/marshal_record.log 2>&1 &

MARSHAL_REC_PID=$!
sleep 1

# 4) Health check
curl -fsS http://localhost:8080/health | tee /dev/stderr

# 5) Produce two VALID MRD files and ingest them via HTTP (unchanged)
./build/mk_mrd ./data/mrd/tmp_record_1.h5
./build/mk_mrd ./data/mrd/tmp_record_2.h5
curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/tmp_record_1.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr
curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/tmp_record_2.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr

# 5b) WS realtime producer (feeds ring buffer/LVC + segment writer during RECORD)
WS_TARGET="/ingest" WS_SERIES=recA WS_FRAMES=3 WS_BYTES=32768 ./build/ws_producer || true

# 6) Find newest dumpbox session directory once files exist
SESSION_DIR=""
for _ in $(seq 1 25); do
  SESSION_DIR=$(ls -dt ./data/dumpbox/*/ 2>/dev/null | head -n1 || true)
  [ -n "${SESSION_DIR}" ] && ls "${SESSION_DIR}"/files/*.mrd >/dev/null 2>&1 && break
  sleep 0.2
done
[ -n "${SESSION_DIR}" ] || { echo "no dumpbox session found"; exit 1; }
echo "SESSION_DIR=${SESSION_DIR}"

# 7) Ensure index.jsonl uses RELATIVE 'files/…' entries (regen if missing/wrong)
if [ ! -s "$SESSION_DIR/index.jsonl" ] || ! grep -q '"file":"files/' "$SESSION_DIR/index.jsonl" 2>/dev/null; then
  : > "$SESSION_DIR/index.jsonl"
  i=1
  for f in "$SESSION_DIR"/files/*.mrd; do
    [ -f "$f" ] || continue
    sz=$(stat -c%s "$f")
    ts=$(date -u -d "@$(stat -c%Y "$f")" +"%Y-%m-%dT%H:%M:%S.%3NZ")
    printf '{"type":"mrd","file":"%s","seq":%d,"size_bytes":%s,"ts":"%s"}\n' \
      "files/$(basename "$f")" "$i" "$sz" "$ts" >> "$SESSION_DIR/index.jsonl"
    i=$((i+1))
  done
fi

# (Optional) Inspect
echo "== Session files =="; find "$SESSION_DIR" -maxdepth 2 -type f -print | sed 's|^|  |'
echo "== index.jsonl tail =="; tail -n 5 "$SESSION_DIR/index.jsonl" || true

# 8) Stop marshal (record phase done)
kill "$MARSHAL_REC_PID"
wait "$MARSHAL_REC_PID" 2>/dev/null || true

# -------------------------------
# PHASE 2 — REPLAY as live
# -------------------------------

# 9) Start marshal back in MRD sink (with WS ingest + RB/LVC + segment writer)
./build/marshal \
  --http 0.0.0.0:8080 \
  --ws   0.0.0.0:8090 \
  --data ./data \
  --sink mrd \
  --ws-ingest on \
  --rb-capacity 2048 \
  --lvc-capacity 256 \
  --segment-root ./data/segments \
  --segment-roll-bytes $((64*1024*1024)) \
  --segment-roll-seconds 30 \
  > ./data/marshal_replay.log 2>&1 &

MARSHAL_REP_PID=$!
sleep 1

# 10) Health check
curl -fsS http://localhost:8080/health | tee /dev/stderr

# 11) Replay the recorded session via HTTP (unchanged)
./build/playback --http http://localhost:8080 --data "$SESSION_DIR" --speed 1.0

# 11b) WS realtime producer (feeds ring buffer/LVC + segment writer during REPLAY)
WS_TARGET="/ingest" WS_SERIES=replayA WS_FRAMES=3 WS_BYTES=32768 ./build/ws_producer || true

# 12) Verify MRD outputs
echo "== MRD files (replayed) =="; ls -l ./data/mrd || true
echo "== latest.json (MRD) =="; [ -f ./data/mrd/latest.json ] && cat ./data/mrd/latest.json || echo "(none)"
echo "== index.jsonl tail (MRD) =="; [ -f ./data/mrd/index.jsonl ] && tail -n 5 ./data/mrd/index.jsonl || echo "(none)"

echo "== Segment writer: segments =="
ls -l ./data/segments || true
echo "== Segment index tail =="
[ -f ./data/segments/index.jsonl ] && tail -n 10 ./data/segments/index.jsonl || echo "(none)"

# 13) Stop marshal
kill "$MARSHAL_REP_PID"
wait "$MARSHAL_REP_PID" 2>/dev/null || true
echo "Record→Replay mode done."
