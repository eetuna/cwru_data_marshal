# fk_client & viz_client — Usage

## What they are (and aren’t)

- **fk_client**: a tiny program that **POSTs poses** to the server (`/v1/pose/update`) at a fixed rate. It fakes the “forward kinematics” publisher of a scanner/robot.  
  - Payload schema (what the server expects):  
    ```json
    {
      "p": [x, y, z],          // length 3, meters
      "R": [r00,r01,...,r22],  // length 9, row-major 3x3 rotation
      "source": "fk"           // (optional) string
    }
    ```
- **viz_client**: a tiny program that **watches the latest MRD** (`latest.json`) and **listens on WebSocket** for events. It fakes a live UI/dashboard.

> These are **pseudo/reference clients**: extremely small, limited error handling, designed to prove the server API + flow. A real FK publisher or UI would swap in directly using the exact same HTTP and WS interfaces.

---

## Build (once)

```bash
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"
# Binaries you'll use:
#   ./build/marshal     (the server)
#   ./build/mk_mrd      (makes sample MRD files)
#   ./build/fk_client   (pose publisher)
#   ./build/viz_client  (visualizer)
#   ./build/playback    (replay recorded sessions)
```

---

## Start the server

### Mode A — Live (MRD sink)
The server writes ingested MRDs to `./data/mrd`:

```bash
mkdir -p ./data/mrd ./data/dumpbox
./build/marshal   --http 0.0.0.0:8080   --ws   0.0.0.0:8090   --data ./data   --sink mrd   > ./data/marshal_live.log 2>&1 &

curl -fsS http://localhost:8080/health
# -> {"status":"ok","uptime_s":...}
```

### Mode B — Record (Dumpbox sink → later Replay)
The server records to `./data/dumpbox/<SESSION>/files`:

```bash
mkdir -p ./data/mrd ./data/dumpbox
./build/marshal   --http 0.0.0.0:8080   --ws   0.0.0.0:8090   --data ./data   --sink dumpbox   --dumpbox-root ./data/dumpbox   > ./data/marshal_record.log 2>&1 &

curl -fsS http://localhost:8080/health
```

---

## viz_client (watch + listen)

**Goal:** show you what a viewer UI would do—track new MRDs and display server events.

```bash
# Watch the MRD directory and connect to WebSocket
./build/viz_client --ws ws://localhost:8090/ws --data ./data/mrd
```

What you’ll see:
- On start (once `latest.json` exists):  
  `viz latest={"path":"./data/mrd/...", "seq":1, "size_bytes":..., "ts":"...", "type":"mrd"}`  
- On each new MRD ingest: another `viz latest=...` line (the client polls `latest.json`, so this works even if WS is down).
- If your server broadcasts events on WS (recommended), you’ll also see lines like:  
  `viz ws: {"type":"acq", ...}` for acquisitions and  
  `viz ws: {"type":"pose", ...}` for pose updates from fk_client.

> If you see **“WS connected …”** but no `viz ws:` lines, your server is probably not broadcasting; that’s OK for basic MRD monitoring. You can add a one-liner WS broadcast in the handlers (see “Troubleshooting” below).

---

## fk_client (pose publisher)

**Goal:** show you what a scanner/robot would do—publish FK poses over HTTP at a fixed rate.

### Minimal run

```bash
# Post 50 poses at 10 Hz to /v1/pose/update
./build/fk_client --http http://localhost:8080 --count 50 --rate 10
```

Expected server response per POST:
- `200/201/204` with a small JSON body.  
- If `viz_client` is connected and the server broadcasts poses, you’ll see `viz ws: {"type":"pose",...}` lines during FK

### Adjustments
- Hit a different server/port: `--http http://server:8080`
- Change frequency: `--rate 5` (5 Hz)
- Fewer/more messages: `--count 3`

### Curl equivalent (for quick sanity)
```bash
curl -fsS -H "Content-Type: application/json"   -d '{"p":[0,0,0],"R":[1,0,0,0,1,0,0,0,1],"source":"fk"}'   http://localhost:8080/v1/pose/update
```

---

## End-to-end quick demos

### A) Live ingest demo (Mode A)

1) **Server** (MRD sink):
   ```bash
   ./build/marshal --http 0.0.0.0:8080 --ws 0.0.0.0:8090 --data ./data --sink mrd      > ./data/marshal_live.log 2>&1 &
   ```

2) **Visualizer**:
   ```bash
   ./build/viz_client --ws ws://localhost:8090/ws --data ./data/mrd
   ```

3) **Ingest MRDs** (simulate a scanner dropping files):
   ```bash
   ./build/mk_mrd ./data/mrd/a1.h5
   ./build/mk_mrd ./data/mrd/a2.h5

   curl -fsS -H "Content-Type: application/octet-stream"      --data-binary @./data/mrd/a1.h5 http://localhost:8080/v1/mrd/ingest
   curl -fsS -H "Content-Type: application/octet-stream"      --data-binary @./data/mrd/a2.h5 http://localhost:8080/v1/mrd/ingest
   ```

4) **Pose stream (optional):**
   ```bash
   ./build/fk_client --http http://localhost:8080 --count 10 --rate 5
   ```

Expected in `viz_client`:
- `viz latest=...` for each MRD
- If server broadcasts: `viz ws: {"type":"pose",...}` lines during FK

---

### B) Record → Replay demo (Mode B)

1) **Record** (dumpbox sink):
   ```bash
   ./build/marshal --http 0.0.0.0:8080 --ws 0.0.0.0:8090      --data ./data --sink dumpbox --dumpbox-root ./data/dumpbox      > ./data/marshal_record.log 2>&1 &
   ```

2) **Send some MRDs** (these become your recorded session):
   ```bash
   ./build/mk_mrd ./data/mrd/r1.h5
   ./build/mk_mrd ./data/mrd/r2.h5
   curl -fsS -H "Content-Type: application/octet-stream" --data-binary @./data/mrd/r1.h5 http://localhost:8080/v1/mrd/ingest
   curl -fsS -H "Content-Type: application/octet-stream" --data-binary @./data/mrd/r2.h5 http://localhost:8080/v1/mrd/ingest
   ```

3) **Find the latest session & ensure `index.jsonl` uses relative paths** (only needed once per session):
   ```bash
   SESSION=$(ls -dt ./data/dumpbox/*/ | head -n1)
   : > "$SESSION/index.jsonl"; i=1
   for f in "$SESSION"/files/*.mrd; do
     sz=$(stat -c%s "$f"); ts=$(date -u -d "@$(stat -c%Y "$f")" +"%Y-%m-%dT%H:%M:%S.%3NZ")
     printf '{"type":"mrd","file":"%s","seq":%d,"size_bytes":%s,"ts":"%s"}\n'        "files/$(basename "$f")" "$i" "$sz" "$ts" >> "$SESSION/index.jsonl"
     i=$((i+1))
   done
   ```

4) **Switch server to MRD sink (live target)**:
   ```bash
   pkill -f /build/marshal || true
   ./build/marshal --http 0.0.0.0:8080 --ws 0.0.0.0:8090 --data ./data --sink mrd      > ./data/marshal_replay.log 2>&1 &
   ```

5) **Run viz** (watches live MRD dir):
   ```bash
   ./build/viz_client --ws ws://localhost:8090/ws --data ./data/mrd
   ```

6) **Replay recorded session (simulates a client posting)**:
   ```bash
   ./build/playback --http http://localhost:8080 --data "$SESSION" --speed 1.0
   ```

Expected in `viz_client`:
- `viz latest=...` lines as MRDs are replayed
- If server broadcasts: `viz ws: {"type":"acq",...}` from ingest; poses only if you recorded/broadcast them during recording (or if playback also posts poses separately).

---

## Troubleshooting (fast)

- **viz prints the first `latest` but nothing else**
  - Ingest another MRD; you should see a new `viz latest=...`.
  - If you don’t, make sure *everything* points to `--data ./data` (not `/data`) and that `viz_client` uses `--data ./data/mrd`.

- **“WS connected …” but no `viz ws:` lines**
  - Server may not be broadcasting events. Add a one-liner in handlers:
    - In `/v1/pose/update` (after validation):  
      ```cpp
      state.ws_emit(nlohmann::json({{"type","pose"},{"p",p},{"R",R},{"ts",iso8601_now_ms()}}).dump());
      ```
    - In `/v1/mrd/ingest` (after write):  
      ```cpp
      state.ws_emit(nlohmann::json({{"type","acq"},{"path",out},{"seq",seq},{"size_bytes",size},{"ts",ts}}).dump());
      ```
  - Make sure `state.ws_emit` is wired to your WS server’s broadcast once at startup.

- **fk_client returns 400**  
  - The server expects **`p` length 3** and **`R` length 9** (flat row-major).  
  - Quick curl test (should be OK):
    ```bash
    curl -fsS -H "Content-Type: application/json"       -d '{"p":[0,0,0],"R":[1,0,0,0,1,0,0,0,1]}'       http://localhost:8080/v1/pose/update
    ```

- **“end of stream” / crashes after errors**  
  - Use the current clients (they open a fresh connection per POST and handle shutdown); don’t reuse a broken socket.

---

## How to replace with real apps

- Your **real FK publisher** should:
  - POST to `POST /v1/pose/update` (10–100 Hz typical)
  - Body: `{ "p":[x,y,z], "R":[9], "source":"<your-name>" }`
  - Content-Type: `application/json`
- Your **real UI** should:
  - Watch (or request) `/v1/mrd/latest` and/or `/v1/mrd/since?ts=...&limit=...`
  - Connect to the WS endpoint (e.g., `ws://host:8090/ws`) and display JSON events (`type=acq|pose|…`)
  - Optionally pull `/v1/pose/current` for a snapshot

The pseudo clients here are intentionally tiny so you can see the **exact HTTP request**, **WebSocket JSON**, and filesystem effects your real apps should rely on.

