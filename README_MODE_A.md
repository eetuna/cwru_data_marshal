set -euo pipefail

# 0) Build
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"

# 1) Clean data + make folders (safe for mounts)
mkdir -p ./data
find ./data -mindepth 1 -maxdepth 1 -exec rm -rf {} +
mkdir -p ./data/mrd ./data/segments ./data/pipe

# 2) Stop any old server
pkill -f "/build/marshal" >/dev/null 2>&1 || true

# 3) Create named pipe for ultra-low-latency stream consumers
FIFO_PATH="./data/pipe/mrd.fifo"
[ -p "$FIFO_PATH" ] || mkfifo "$FIFO_PATH"

# (Optional) Peek a few bytes when the first frame arrives, then exit the reader.
# This proves the pipe works but won’t block the main flow forever.
( timeout 5 cat "$FIFO_PATH" | head -c 64 | hexdump -C || true ) &
PIPE_PEEK_PID=$!

# 4) Start marshal in MRD sink (live mode) with WS ingest + RB/LVC + segment writer + named pipe sink
#    - MRD sink: durable rolling files under ./data/mrd
#    - WS server (fanout) stays at :8090
#    - WS ingest enabled so producers can push frames over WebSocket too
#    - Ring buffer + last-value cache sized for fast imaging
#    - Segment writer appends binary segments and an index.jsonl under ./data/segments
#    - Named pipe sink mirrors the stream to the FIFO for the lowest-latency consumers
./build/marshal \
  --http 0.0.0.0:8080 \
  --ws   0.0.0.0:8090 \
  --data ./data \
  --sink mrd \
  --sink-namedpipe "$FIFO_PATH" \
  --ws-ingest on \
  --rb-capacity 2048 \
  --lvc-capacity 256 \
  --segment-root ./data/segments \
  --segment-roll-bytes $((64*1024*1024)) \
  --segment-roll-seconds 30 \
  > ./data/marshal_live.log 2>&1 &

MARSHAL_PID=$!
sleep 1

# 5) Health check
curl -fsS http://localhost:8080/health | tee /dev/stderr

# 6) Produce two VALID MRD files and ingest them via HTTP (unchanged)
./build/mk_mrd ./data/mrd/sample_live_1.h5
./build/mk_mrd ./data/mrd/sample_live_2.h5
curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/sample_live_1.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr
curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/sample_live_2.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr

# (Optional) WS ingest smoke test (if you have websocat)
websocat -t - ws://127.0.0.1:8090/ingest <<<'hello from client'     

# 6b) WS realtime producer (feeds segment writer)
WS_TARGET="/ingest?series=demoA&topic=mrd.acq" \
WS_FRAMES=3 \
WS_BYTES=32768 \
./build/ws_producer || true

# Inspect segment writer output
echo "== Segment writer: segments =="
ls -l ./data/segments || true
echo "== Segment index tail =="
[ -f ./data/segments/index.jsonl ] && tail -n 10 ./data/segments/index.jsonl || echo "(none)"

# 7) Verify outputs for clients (tolerant)
echo "== MRD files =="; ls -l ./data/mrd || true
echo "== latest.json =="; [ -f ./data/mrd/latest.json ] && cat ./data/mrd/latest.json || echo "(none)"
echo "== index.jsonl (MRD) tail =="; [ -f ./data/mrd/index.jsonl ] && tail -n 5 ./data/mrd/index.jsonl || echo "(none)"

echo "== Segment writer: segments =="
ls -l ./data/segments || true
echo "== Segment index tail =="
[ -f ./data/segments/index.jsonl ] && tail -n 10 ./data/segments/index.jsonl || echo "(none)"

# Give the FIFO peek a moment to consume some bytes (if any)
sleep 1
kill "$PIPE_PEEK_PID" >/dev/null 2>&1 || true

# 8) Stop marshal
kill "$MARSHAL_PID"
wait "$MARSHAL_PID" 2>/dev/null || true
echo "Live mode done."
