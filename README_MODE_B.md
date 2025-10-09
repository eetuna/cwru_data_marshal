# Mode B — Record then replay runbook

Follow this walkthrough to capture MRD files into a dumpbox session, then replay
that session back through the marshal so clients receive fresh MRDs exactly as if
they were produced live.

> 📦 The commands assume you are in the repository root. Adjust paths if you use
> a different data directory.

---

## Phase 0 — Build once

```bash
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"
```

## Phase 1 — Record into a dumpbox session

### 1. Prepare directories

```bash
mkdir -p ./data
find ./data -mindepth 1 -maxdepth 1 -exec rm -rf {} +
mkdir -p ./data/mrd ./data/dumpbox
```

### 2. Ensure no previous marshal instance is running

```bash
pkill -f "/build/marshal" >/dev/null 2>&1 || true
```

### 3. Start the marshal in dumpbox mode

```bash
./build/marshal \
  --http 0.0.0.0:8080 \
  --ws   0.0.0.0:8090 \
  --data ./data \
  --sink dumpbox \
  --dumpbox-root ./data/dumpbox \
  > ./data/marshal_record.log 2>&1 &

MARSHAL_REC_PID=$!
sleep 1
```

### 4. Health check

```bash
# Prints JSON {"status":"ok","uptime_s":...}
curl -fsS http://localhost:8080/health | tee /dev/stderr
```

### 5. Upload MRD samples (and optional SWMR frames)

```bash
./build/mk_mrd ./data/mrd/tmp_record_1.h5
./build/mk_mrd ./data/mrd/tmp_record_2.h5

curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/tmp_record_1.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr

curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/tmp_record_2.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr
```

(Optional) append SWMR frames to the active session by capturing an ISMRMRD
Image message (header + voxels) and saving it as `./data/image_message.bin`.
You can use the built-in helpers:

```bash
# Single frame (C++)
./build/make_image_message --out ./data/image_message.bin

# Single frame (Python)
python3 tools/make_image_message.py ./data/image_message.bin

curl -fsS \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-MRD-Stream: dumpbox_demo' \
  --data-binary @./data/image_message.bin \
  http://localhost:8080/v1/ismrmrd/frame | tee /dev/stderr
```

To continuously stream frames while recording the session, optionally override
the generated geometry or cadence (for example `--nx 128 --ny 128 --nslices 8 --dt-ms 200`):

```bash
./build/image_streamer --http http://localhost:8080 --stream dumpbox_demo --nx 128 --ny 128 --nslices 8 --dt-ms 200
# or
python3 tools/stream_image_series.py --http http://localhost:8080 --stream dumpbox_demo --nx 128 --ny 128 --nslices 8 --dt-ms 200
```

The marshal will automatically roll to a new geometry-stamped MRD if the stream
dimensions change while recording.

### 6. Locate the newest session

The loop waits for MRD files to arrive before proceeding.

```bash
SESSION_DIR=""
for _ in $(seq 1 25); do
  SESSION_DIR=$(ls -dt ./data/dumpbox/*/ 2>/dev/null | head -n1 || true)
  [ -n "${SESSION_DIR}" ] && ls "${SESSION_DIR}"/files/*.mrd >/dev/null 2>&1 && break
  sleep 0.2
done

[ -n "${SESSION_DIR}" ] || { echo "no dumpbox session found"; exit 1; }
echo "SESSION_DIR=${SESSION_DIR}"
```

Inspect what was recorded:

```bash
echo "== Session files =="; find "$SESSION_DIR" -maxdepth 2 -type f -print | sed 's|^|  |'
echo "== index.jsonl tail =="; tail -n 5 "$SESSION_DIR/index.jsonl" || true
```

### 7. Stop the marshal (recording complete)

```bash
kill "$MARSHAL_REC_PID"
wait "$MARSHAL_REC_PID" 2>/dev/null || true
# Fallback if MARSHAL_REC_PID is unavailable
pkill -f "/build/marshal" >/dev/null 2>&1 || true
```

## Phase 2 — Replay the session as live MRDs

### 8. Start the marshal back in MRD mode

```bash
./build/marshal \
  --http 0.0.0.0:8080 \
  --ws   0.0.0.0:8090 \
  --data ./data \
  --sink mrd \
  --flush-max-frames 4 \
  --flush-max-ms 50 \
  > ./data/marshal_replay.log 2>&1 &

MARSHAL_REP_PID=$!
sleep 1
# Set `--flush-max-frames 1 --flush-max-ms 0` if you must mirror the single-frame flush baseline for latency-critical replay.
```

### 9. Confirm the service is ready

```bash
# Prints JSON {"status":"ok","uptime_s":...}
curl -fsS http://localhost:8080/health | tee /dev/stderr
```

### 10. Replay the recorded session

```bash
./build/playback --http http://localhost:8080 --data "$SESSION_DIR" --speed 1.0
```

### 11. Verify replay outputs

```bash
echo "== MRD files (replayed) =="; ls -l ./data/mrd || true
echo "== latest.json (MRD) =="; [ -f ./data/mrd/latest.json ] && cat ./data/mrd/latest.json || echo "(none)"
echo "== index.jsonl tail (MRD) =="; [ -f ./data/mrd/index.jsonl ] && tail -n 5 ./data/mrd/index.jsonl || echo "(none)"
```

### 12. Shut everything down

```bash
kill "$MARSHAL_REP_PID"
wait "$MARSHAL_REP_PID" 2>/dev/null || true
# Fallback if MARSHAL_REP_PID is unavailable
pkill -f "/build/marshal" >/dev/null 2>&1 || true
echo "Record→Replay mode done."
```
