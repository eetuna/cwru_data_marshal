# README_MODE_B.md — Record → Replay

```bash
set -euo pipefail

# 0) Build
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"

# 1) Clean data + make folder
rm -rf ./data
mkdir -p ./data
find ./data -mindepth 1 -maxdepth 1 -exec rm -rf {} +
mkdir -p ./data/dumpbox ./data/mrd
# -------------------------------
# PHASE 1 — RECORD to dumpbox
# -------------------------------

# 2) Stop any old server
pkill -f "/build/marshal" >/dev/null 2>&1 || true

# 3) Start marshal in DUMPBOX sink (session auto-named)
./build/marshal       --http 0.0.0.0:8080       --ws   0.0.0.0:8090       --data ./data       --sink dumpbox       --dumpbox-root ./data/dumpbox       > ./data/marshal_record.log 2>&1 &

MARSHAL_REC_PID=$!
sleep 1

# 4) Health check
curl -s http://localhost:8080/health | tee /dev/stderr

# 5) Produce two MRD files and ingest them
head -c 16384 </dev/urandom > ./data/tmp_record_1.mrd
head -c 24576 </dev/urandom > ./data/tmp_record_2.mrd
curl -s -H "Content-Type: application/octet-stream" --data-binary @./data/tmp_record_1.mrd http://localhost:8080/v1/mrd/ingest | tee /dev/stderr
curl -s -H "Content-Type: application/octet-stream" --data-binary @./data/tmp_record_2.mrd http://localhost:8080/v1/mrd/ingest | tee /dev/stderr

# 6) Find newest dumpbox session directory
SESSION_DIR=$(ls -dt ./data/dumpbox/* | head -n1)
echo "SESSION_DIR=$SESSION_DIR"

# 7) Inspect what was recorded
echo "== Session files =="
find "$SESSION_DIR" -maxdepth 2 -type f -print | sed 's|^|  |'
echo "== latest.json =="
cat "$SESSION_DIR/latest.json"
echo "== index.jsonl tail =="
tail -n 5 "$SESSION_DIR/index.jsonl" || true

# 8) Stop marshal (record phase done)
kill "$MARSHAL_REC_PID"
wait "$MARSHAL_REC_PID" 2>/dev/null || true

# -------------------------------
# PHASE 2 — REPLAY as live
# -------------------------------

# 9) Start marshal back in MRD sink
./build/marshal       --http 0.0.0.0:8080       --ws   0.0.0.0:8090       --data ./data       --sink mrd       > ./data/marshal_replay.log 2>&1 &

MARSHAL_REP_PID=$!
sleep 1

# 10) Health check
curl -s http://localhost:8080/health | tee /dev/stderr

# 11) Replay the recorded session via HTTP
./build/playback       --http http://localhost:8080       --data "$SESSION_DIR"       --speed 1.0

# 12) Verify MRD outputs
echo "== MRD files (replayed) =="
ls -l ./data/mrd
echo "== latest.json (MRD) =="
cat ./data/mrd/latest.json
echo "== index.jsonl tail (MRD) =="
tail -n 5 ./data/mrd/index.jsonl || true

# 13) Stop marshal
kill "$MARSHAL_REP_PID"
wait "$MARSHAL_REP_PID" 2>/dev/null || true
echo "Record→Replay mode done."
```
