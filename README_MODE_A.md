set -euo pipefail

# 0) Build
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"

# 1) Clean data + make folders (safe for mounts)
mkdir -p ./data
find ./data -mindepth 1 -maxdepth 1 -exec rm -rf {} +
mkdir -p ./data/mrd

# 2) Stop any old server
pkill -f "/build/marshal" >/dev/null 2>&1 || true

# 3) Start marshal in MRD sink (live mode)
#    - MRD files land in ./data/mrd
#    - HTTP on :8080, WebSocket fan-out on :8090
./build/marshal \
  --http 0.0.0.0:8080 \
  --ws   0.0.0.0:8090 \
  --data ./data \
  --sink mrd \
  > ./data/marshal_live.log 2>&1 &

MARSHAL_PID=$!
sleep 1

# 4) Health check
curl -fsS http://localhost:8080/health | tee /dev/stderr

# 5) Produce two VALID MRD files and ingest them via HTTP (unchanged)
./build/mk_mrd ./data/mrd/sample_live_1.h5
./build/mk_mrd ./data/mrd/sample_live_2.h5
curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/sample_live_1.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr
curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/sample_live_2.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr

# 5b) Append streaming frames into a live SWMR MRD (float32 4x3x1, 2 channels)
python - <<'PY'
import numpy as np
for i, off in enumerate([0, 100, 200]):
    (np.arange(24, dtype=np.float32) + off).tofile(f'frame_{i}.bin')
PY

for f in frame_0.bin frame_1.bin frame_2.bin; do
  curl -fsS \
    -H 'Content-Type: application/octet-stream' \
    -H 'X-MRD-Stream: live_demo' \
    -H 'X-MRD-Dimensions: 4x3' \
    -H 'X-MRD-Channels: 2' \
    -H 'X-MRD-Datatype: float32' \
    --data-binary @$f \
    http://localhost:8080/v1/mrd/frame | tee /dev/stderr
done

# 6) (Optional) WebSocket ingest smoke test (requires websocat)
# Send the MRD payload as a single binary frame; the server will write a new file
# and broadcast the ingest metadata back to any subscribers.
if command -v websocat >/dev/null; then
  websocat -b ws://127.0.0.1:8090/ < ./data/mrd/sample_live_1.h5 || true
fi

# 7) Verify outputs for clients (tolerant)
echo "== MRD files =="; ls -l ./data/mrd || true
echo "== latest.json =="; [ -f ./data/mrd/latest.json ] && cat ./data/mrd/latest.json || echo "(none)"
echo "== index.jsonl (MRD) tail =="; [ -f ./data/mrd/index.jsonl ] && tail -n 5 ./data/mrd/index.jsonl || echo "(none)"

# 8) Stop marshal
kill "$MARSHAL_PID"
wait "$MARSHAL_PID" 2>/dev/null || true
echo "Live mode done."
