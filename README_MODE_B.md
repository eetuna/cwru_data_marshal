set -euo pipefail

# 0) Build
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"

# 1) Clean data + make folders (safe for mounts)
mkdir -p ./data
find ./data -mindepth 1 -maxdepth 1 -exec rm -rf {} +
mkdir -p ./data/mrd ./data/dumpbox

# -------------------------------
# PHASE 1 — RECORD to dumpbox
# -------------------------------

# 2) Stop any old server
pkill -f "/build/marshal" >/dev/null 2>&1 || true

# 3) Start marshal in DUMPBOX sink (session auto-named)
./build/marshal \
  --http 0.0.0.0:8080 \
  --ws   0.0.0.0:8090 \
  --data ./data \
  --sink dumpbox \
  --dumpbox-root ./data/dumpbox \
  > ./data/marshal_record.log 2>&1 &

MARSHAL_REC_PID=$!
sleep 1

# 4) Health check
curl -fsS http://localhost:8080/health | tee /dev/stderr

# 5) Produce two VALID MRD files and ingest them via HTTP
./build/mk_mrd ./data/mrd/tmp_record_1.h5
./build/mk_mrd ./data/mrd/tmp_record_2.h5
curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/tmp_record_1.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr
curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/tmp_record_2.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr

# (Optional) Append live SWMR frames to the active dumpbox session
python - <<'PY'
import numpy as np
for i, off in enumerate([0, 40, 80]):
    (np.arange(24, dtype=np.float32) + off).tofile(f'dumpbox_frame_{i}.bin')
PY

for f in dumpbox_frame_0.bin dumpbox_frame_1.bin dumpbox_frame_2.bin; do
  curl -fsS \
    -H 'Content-Type: application/octet-stream' \
    -H 'X-MRD-Stream: dumpbox_demo' \
    -H 'X-MRD-Dimensions: 4x3' \
    -H 'X-MRD-Channels: 2' \
    --data-binary @$f \
    http://localhost:8080/v1/mrd/frame | tee /dev/stderr
done

# 6) Locate the newest dumpbox session (contains files/ + metadata)
SESSION_DIR=""
for _ in $(seq 1 25); do
  SESSION_DIR=$(ls -dt ./data/dumpbox/*/ 2>/dev/null | head -n1 || true)
  [ -n "${SESSION_DIR}" ] && ls "${SESSION_DIR}"/files/*.mrd >/dev/null 2>&1 && break
  sleep 0.2
done
[ -n "${SESSION_DIR}" ] || { echo "no dumpbox session found"; exit 1; }
echo "SESSION_DIR=${SESSION_DIR}"

# (Optional) Inspect recorded outputs
echo "== Session files =="; find "$SESSION_DIR" -maxdepth 2 -type f -print | sed 's|^|  |'
echo "== index.jsonl tail =="; tail -n 5 "$SESSION_DIR/index.jsonl" || true

# 7) Stop marshal (record phase done)
kill "$MARSHAL_REC_PID"
wait "$MARSHAL_REC_PID" 2>/dev/null || true

# -------------------------------
# PHASE 2 — REPLAY as live
# -------------------------------

# 8) Start marshal back in MRD sink
./build/marshal \
  --http 0.0.0.0:8080 \
  --ws   0.0.0.0:8090 \
  --data ./data \
  --sink mrd \
  > ./data/marshal_replay.log 2>&1 &

MARSHAL_REP_PID=$!
sleep 1

# 9) Health check
curl -fsS http://localhost:8080/health | tee /dev/stderr

# 10) Replay the recorded session via HTTP
./build/playback --http http://localhost:8080 --data "$SESSION_DIR" --speed 1.0

# 11) Verify MRD outputs
echo "== MRD files (replayed) =="; ls -l ./data/mrd || true
echo "== latest.json (MRD) =="; [ -f ./data/mrd/latest.json ] && cat ./data/mrd/latest.json || echo "(none)"
echo "== index.jsonl tail (MRD) =="; [ -f ./data/mrd/index.jsonl ] && tail -n 5 ./data/mrd/index.jsonl || echo "(none)"

# 12) Stop marshal
kill "$MARSHAL_REP_PID"
wait "$MARSHAL_REP_PID" 2>/dev/null || true
echo "Record→Replay mode done."
