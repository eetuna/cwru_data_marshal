# Usage Guide with Sample Clients

The repository ships three tiny client binaries that exercise all of the
interfaces exposed by the marshal:

- `fk_client` &mdash; periodic HTTP `POST /v1/pose/update` sender
- `viz_client` &mdash; watches `latest.json` on disk and the WebSocket fan-out
- `ws_producer` &mdash; streams MRD payloads over WebSocket (binary frames)

The sections below are copy-pasteable runbooks that show how to:

1. Run the marshal in **live (MRD sink) mode** and watch the outputs with the
   sample clients.
2. Run the marshal in **dumpbox (record → replay) mode**, capture a session, and
   replay it back to the MRD sink for the same clients to consume without any
   changes.

Both flows assume you are at the repository root.

---

## 0. Build everything once

```bash
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"
# Explicitly build the example clients (optional if you built the default ALL target)
cmake --build build --target marshal playback fk_client viz_client ws_producer
```

---

## Live mode (MRD sink) with sample clients

The MRD sink writes directly into `./data/mrd`. Clients should watch that
folder and the WebSocket fan-out.

### 1. Prepare a clean workspace

```bash
mkdir -p ./data
find ./data -mindepth 1 -maxdepth 1 -exec rm -rf {} +
mkdir -p ./data/mrd
```

### 2. Start the marshal in MRD sink mode

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

### 3. Smoke test the HTTP and WebSocket ingress paths

Create two valid MRD payloads with the built-in generator and ingest them via
HTTP. Then push another payload through the WebSocket producer.

```bash
# Health check
curl -fsS http://localhost:8080/health | tee /dev/stderr

# Generate sample MRDs
./build/mk_mrd ./data/mrd/sample_http_1.h5
./build/mk_mrd ./data/mrd/sample_http_2.h5

# HTTP ingest
curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/sample_http_1.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr
curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/sample_http_2.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr

# WebSocket ingest (writes another MRD file on the server side)
WS_FILE=./data/mrd/sample_http_1.h5 \
  WS_FRAMES=1 \
  ./build/ws_producer
```

### 4. Run the sample clients

**Forward kinematics client (`fk_client`)**

Posts pose updates and prints the JSON response body returned by the marshal.

```bash
./build/fk_client --http http://localhost:8080 --pretty
```

**Visualization client (`viz_client`)**

Subscribes to the WebSocket broadcast and polls `latest.json` for changes so you
can see the exact MRD metadata and pose updates the marshal emits.

```bash
./build/viz_client --ws ws://localhost:8090/ws --data ./data/mrd
```

Leave both clients running while MRDs are ingested. `viz_client` prints every
metadata broadcast coming from `/v1/mrd/ingest` and pose updates (see next step).

### 5. Send pose updates while clients are running

```bash
./build/fk_client --http http://localhost:8080 --pretty
```

The WebSocket fan-out will emit pose notifications that `viz_client` will show
immediately. You can re-run `fk_client` any time to emulate the scanner pose
stream.

### 6. Inspect live-mode outputs

```bash
echo "== MRD directory =="
ls -l ./data/mrd

echo "== latest.json =="
cat ./data/mrd/latest.json

echo "== index.jsonl (tail) =="
tail -n 5 ./data/mrd/index.jsonl
```

### 7. Shut down the marshal

```bash
kill "$MARSHAL_PID"
wait "$MARSHAL_PID" 2>/dev/null || true
```

At this point the clients can be stopped with `Ctrl+C`.

---

## Dumpbox mode (record → replay) with sample clients

Dumpbox sessions isolate scanner captures inside `./data/dumpbox/<SESSION>/`.
You can replay any captured session back into the MRD sink without changing the
clients.

### Phase 1 — Record into a dumpbox session

#### 1. Clean data directories

```bash
mkdir -p ./data
find ./data -mindepth 1 -maxdepth 1 -exec rm -rf {} +
mkdir -p ./data/mrd ./data/dumpbox
```

#### 2. Start marshal in dumpbox mode

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
curl -fsS http://localhost:8080/health | tee /dev/stderr
```

#### 3. Ingest MRDs via HTTP and WebSocket

```bash
./build/mk_mrd ./data/mrd/tmp_record_http_1.h5
./build/mk_mrd ./data/mrd/tmp_record_http_2.h5

curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/tmp_record_http_1.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr
curl -fsS -H "Content-Type: application/octet-stream" \
  --data-binary @./data/mrd/tmp_record_http_2.h5 \
  http://localhost:8080/v1/mrd/ingest | tee /dev/stderr

WS_FILE=./data/mrd/tmp_record_http_1.h5 \
  WS_FRAMES=1 \
  ./build/ws_producer
```

#### 4. Drive pose traffic during the recording window

The marshal persists pose updates in memory and rebroadcasts them to any live
WebSocket subscribers.

```bash
./build/fk_client --http http://localhost:8080 --pretty
```

Leave `viz_client` running (if you started it) to observe that even in dumpbox
mode it still receives the pose and ingest broadcasts.

#### 5. Locate the newest dumpbox session

```bash
SESSION_DIR=""
for _ in $(seq 1 25); do
  SESSION_DIR=$(ls -dt ./data/dumpbox/*/ 2>/dev/null | head -n1 || true)
  [ -n "$SESSION_DIR" ] && ls "$SESSION_DIR"/files/*.mrd >/dev/null 2>&1 && break
  sleep 0.2
done
[ -n "$SESSION_DIR" ] || { echo "no dumpbox session found"; exit 1; }
echo "SESSION_DIR=$SESSION_DIR"

find "$SESSION_DIR" -maxdepth 2 -type f -print | sed 's|^|  |'
```

`viz_client` can continue running, but the MRD files for clients will not appear
in `./data/mrd` yet—they remain inside the dumpbox session.

#### 6. Stop the marshal (recording complete)

```bash
kill "$MARSHAL_REC_PID"
wait "$MARSHAL_REC_PID" 2>/dev/null || true
```

### Phase 2 — Replay the session as live MRDs

#### 7. Restart the marshal in MRD sink mode

```bash
./build/marshal \
  --http 0.0.0.0:8080 \
  --ws   0.0.0.0:8090 \
  --data ./data \
  --sink mrd \
  > ./data/marshal_replay.log 2>&1 &
MARSHAL_REP_PID=$!
sleep 1
curl -fsS http://localhost:8080/health | tee /dev/stderr
```

If `viz_client` was still running it will reconnect automatically once the WS
endpoint is available again.

#### 8. Replay the recorded dumpbox session

```bash
./build/playback --http http://localhost:8080 --data "$SESSION_DIR" --speed 1.0
```

The playback utility replays the MRD files over HTTP so the marshal (now in MRD
mode) regenerates client-visible artifacts. The WebSocket fan-out will emit the
same ingest metadata that `viz_client` saw during the recording phase.

#### 9. Verify MRD outputs and pose continuity

```bash
echo "== MRD directory after replay =="
ls -l ./data/mrd

echo "== latest.json =="
cat ./data/mrd/latest.json

echo "== index.jsonl tail =="
tail -n 5 ./data/mrd/index.jsonl
```

Re-run `fk_client` if you want to inject fresh pose updates—the clients and
HTTP interface work exactly the same as in live mode.

#### 10. Stop the marshal

```bash
kill "$MARSHAL_REP_PID"
wait "$MARSHAL_REP_PID" 2>/dev/null || true
```

Stop any running clients with `Ctrl+C`. You now have a full dumpbox session on
disk (`$SESSION_DIR`) and the replayed MRDs in `./data/mrd` that the clients
observed during replay.

---

## Quick reference: sample client behaviours

| Binary          | Transport | Purpose                                                           | Key flags / env vars                |
|-----------------|-----------|-------------------------------------------------------------------|-------------------------------------|
| `fk_client`     | HTTP      | Posts `p`/`R` pose updates to `/v1/pose/update`, prints response. | `--http http://HOST:PORT`, `--pretty` |
| `viz_client`    | HTTP+WS   | Polls `latest.json`, subscribes to WS fan-out, logs broadcasts.   | `--ws ws://HOST:PORT/PATH`, `--data DIR` |
| `ws_producer`   | WS        | Sends binary MRD frames over WS; prints server replies.           | `WS_HOST`, `WS_PORT`, `WS_TARGET`, `WS_FILE`, `WS_FRAMES`, `WS_BYTES` |

These utilities are intentionally simple so they can be used as starting points
for more sophisticated scanner, visualization, or ingestion tooling.
