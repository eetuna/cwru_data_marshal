set -euo pipefail

# 0) Build
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"

# 1) Clean data + make folder (safe for mounts)
mkdir -p ./data
find ./data -mindepth 1 -maxdepth 1 -exec rm -rf {} +
mkdir -p ./data/mrd

# 2) Stop any old server
pkill -f "/build/marshal" >/dev/null 2>&1 || true

# 3) Start marshal in MRD sink (live mode)
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

# 5) Produce two VALID MRD files and ingest them
./build/mk_mrd ./data/mrd/sample_live_1.h5
./build/mk_mrd ./data/mrd/sample_live_2.h5
curl -fsS -H "Content-Type: application/octet-stream" --data-binary @./data/mrd/sample_live_1.h5 http://localhost:8080/v1/mrd/ingest | tee /dev/stderr
curl -fsS -H "Content-Type: application/octet-stream" --data-binary @./data/mrd/sample_live_2.h5 http://localhost:8080/v1/mrd/ingest | tee /dev/stderr

# 6) Verify MRD outputs for clients (tolerant if files not produced)
echo "== MRD files =="; ls -l ./data/mrd || true
echo "== latest.json =="; [ -f ./data/mrd/latest.json ] && cat ./data/mrd/latest.json || echo "(none)"
echo "== last 5 lines of index.jsonl =="; [ -f ./data/mrd/index.jsonl ] && tail -n 5 ./data/mrd/index.jsonl || echo "(none)"

# 7) Stop marshal
kill "$MARSHAL_PID"
wait "$MARSHAL_PID" 2>/dev/null || true
echo "Live mode done."
