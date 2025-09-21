# README_MODE_A.md — Live file access

```bash
set -euo pipefail

# 0) Build
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"

# 1) Clean data + make folder
rm -rf ./data
mkdir -p ./data

# 2) Stop any old server
pkill -f "/build/marshal" >/dev/null 2>&1 || true

# 3) Start marshal in MRD sink (live mode)
./build/marshal       --http 0.0.0.0:8080       --ws   0.0.0.0:8090       --data ./data       --sink mrd       > ./data/marshal_live.log 2>&1 &

MARSHAL_PID=$!
sleep 1

# 4) Health check
curl -s http://localhost:8080/health | tee /dev/stderr

# 5) Produce two MRD files and ingest them
mkdir -p ./data/mrd
head -c 8192 </dev/urandom > ./data/mrd/sample_live_1.mrd
head -c 12288 </dev/urandom > ./data/mrd/sample_live_2.mrd
curl -s -H "Content-Type: application/octet-stream" --data-binary @./data/mrd/sample_live_1.mrd http://localhost:8080/v1/mrd/ingest | tee /dev/stderr
curl -s -H "Content-Type: application/octet-stream" --data-binary @./data/mrd/sample_live_2.mrd http://localhost:8080/v1/mrd/ingest | tee /dev/stderr

# 6) Verify MRD outputs for clients
echo "== MRD files =="
ls -l ./data/mrd
echo "== latest.json =="
cat ./data/mrd/latest.json
echo "== last 5 lines of index.jsonl =="
tail -n 5 ./data/mrd/index.jsonl || true

# 7) Stop marshal
kill "$MARSHAL_PID"
wait "$MARSHAL_PID" 2>/dev/null || true
echo "Live mode done."
```
