# Mode A — Live MRD sink runbook

This guide walks a new operator through building the marshal, starting it in
live MRD mode, uploading example MRD files, and streaming SWMR frames that
clients can visualize immediately.

> 💡 Tip: You can copy/paste the commands in each block directly into a shell.
> Run them from the repository root unless noted otherwise.

---

## 0. Build the project

```bash
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"
```

## 1. Reset the data directory (safe for bind mounts)

```bash
mkdir -p ./data
find ./data -mindepth 1 -maxdepth 1 -exec rm -rf {} +
mkdir -p ./data/mrd
```

## 2. Stop any previous marshal instance

```bash
pkill -f "/build/marshal" >/dev/null 2>&1 || true
```

## 3. Start the marshal in MRD mode (live)

```bash
./build/marshal \
  --http 0.0.0.0:8080 \
  --ws   0.0.0.0:8090 \
  --data ./data \
  --sink mrd \
  > ./data/marshal_live.log 2>&1 &

MARSHAL_PID=$!
sleep 1
```

## 4. Verify the service is healthy

```bash
curl -fsS http://localhost:8080/health | tee /dev/stderr
```

## 5. Generate valid MRD samples and ingest them

```bash
./build/mk_mrd ./data/mrd/sample_live_1.h5
./build/mk_mrd ./data/mrd/sample_live_2.h5

curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/sample_live_1.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr

curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/sample_live_2.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr
```

## 6. (Optional) Append streaming frames via SWMR
Capture or synthesize an ISMRMRD Image message (header + voxels) and save it as
`image_message.bin`, then stream it into a live MRD file identified by
`live_demo`. See [`docs/API_REFERENCE.md`](docs/API_REFERENCE.md#post-v1ismrmrdframe)
for a Python helper snippet:

# Creates ./image_message.bin in CWD
./build/make_image_message --out image_message.bin
ls -l image_message.bin

```bash
curl -fsS \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-MRD-Stream: live_demo' \
  --data-binary @image_message.bin \
  http://localhost:8080/v1/ismrmrd/frame | tee /dev/stderr
```

## 7. (Optional) Smoke-test WebSocket ingestion

If you have [`websocat`](https://github.com/vi/websocat) installed, you can send
an MRD payload over the `/ws` endpoint and watch the marshal broadcast the
resulting metadata:

```bash
if command -v websocat >/dev/null; then
  websocat -b ws://127.0.0.1:8090/ < ./data/mrd/sample_live_1.h5 || true
fi
```

## 8. Inspect outputs like a client would

```bash
echo "== MRD files =="; ls -l ./data/mrd || true
echo "== latest.json =="; [ -f ./data/mrd/latest.json ] && cat ./data/mrd/latest.json || echo "(none)"
echo "== index.jsonl (MRD) tail =="; [ -f ./data/mrd/index.jsonl ] && tail -n 5 ./data/mrd/index.jsonl || echo "(none)"
```

## 9. Cleanly stop the marshal

```bash
kill "$MARSHAL_PID"
wait "$MARSHAL_PID" 2>/dev/null || true
echo "Live mode done."
```
