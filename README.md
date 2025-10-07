# CWRU Data Marshal

**What it is.** A tiny, fast HTTP/WS hub for reconstructed MRI data (ISMRMRD). It runs in exactly one of two modes:

- **Mode A — Live file access:** scanner → `marshal` → **MRD files** → clients read files  
- **Mode B — Record → Replay:**  
  - **Record now:** scanner → `marshal` → **dumpbox session** (no `/data/mrd` writes)  
  - **Replay later:** `playback` → `marshal` → **MRD files** → clients read files (same interface as live)

This keeps the data sink unambiguous: **one or the other**.

---

## Features

- 🚦 **Two explicit sinks**
  - **MRD sink** (Live): writes `/data/mrd/<timestamp>_<seq>.mrd`, maintains `index.jsonl`, `latest.json`.
  - **Dumpbox sink** (Record): writes `/data/dumpbox/<SESSION>/files/*.mrd`, with session-scoped `index.jsonl`, `latest.json`.
- 🔁 **HTTP playback** tool: re-POSTs a dumpbox session back to `/v1/mrd/ingest` to simulate the scanner; `marshal` (in MRD mode) rewrites fresh MRDs for clients.
- 🔓 **SWMR MRD streaming**: `/v1/mrd/frame` appends reconstructed images into a live HDF5 dataset so readers can follow along via HDF5 Single-Writer-Multiple-Reader semantics.
- 🧩 **Zero client code changes** between Live and Replay—clients always “read files.”
- ⚙️ **Tiny footprint**: Boost.Asio/Beast + nlohmann/json; no heavy deps.

---

## Storage layout

**Live (MRD sink)**
```
/data/mrd/
├─ 2025-09-20T01:23:45.123Z_000001.mrd
├─ 2025-09-20T01:23:47.045Z_000002.mrd
├─ index.jsonl
└─ latest.json
```

**Record (Dumpbox sink)**
```
/data/dumpbox/2025-09-20T01:23:00Z/
├─ files/
│  ├─ 2025-09-20T01:23:45.123Z_000001.mrd
│  └─ 2025-09-20T01:23:47.045Z_000002.mrd
├─ index.jsonl
└─ latest.json
```

---

## Build

### Prereqs (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install -y       build-essential cmake ninja-build pkg-config       libboost-system-dev
```

### Compile
```bash
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"
```

**Binaries**
- `build/marshal` — server (HTTP/WS)
- `build/playback` — **HTTP-only** playback (replays dumpbox sessions)

---

## Quick start

> Full, copy-pasteable runbooks are in `README_MODE_A.md` (Live) and `README_MODE_B.md` (Record→Replay).  
> Below are minimal one-liners.

### Live (Mode A) — scanner → MRD → clients
```bash
# Start server in MRD sink
./build/marshal --http 0.0.0.0:8080 --ws 0.0.0.0:8090 --data ./data --sink mrd &
sleep 1
curl -s http://localhost:8080/health    # expect: ok

# Ingest two files (dummy fallback shown)
mkdir -p ./data/mrd
head -c 8192 </dev/urandom > ./data/mrd/a.mrd
head -c 12288 </dev/urandom > ./data/mrd/b.mrd
curl -s -H "Content-Type: application/octet-stream" --data-binary @./data/mrd/a.mrd http://localhost:8080/v1/mrd/ingest
curl -s -H "Content-Type: application/octet-stream" --data-binary @./data/mrd/b.mrd http://localhost:8080/v1/mrd/ingest

# Verify client-visible files
ls -l ./data/mrd
tail -n 5 ./data/mrd/index.jsonl || true
cat ./data/mrd/latest.json
```

#### Stream frames with SWMR

```bash
# Create a trio of demo frames (float32, 2 channels, 4x3 slice)
python - <<'PY'
import numpy as np
for i, offset in enumerate([0, 100, 200]):
    (np.arange(24, dtype=np.float32) + offset).tofile(f'frame{i}.bin')
PY

for f in frame0.bin frame1.bin frame2.bin; do
  curl -fsS \
    -H 'Content-Type: application/octet-stream' \
    -H 'X-MRD-Stream: demo_stream' \
    -H 'X-MRD-Dimensions: 4x3' \
    -H 'X-MRD-Channels: 2' \
    -H 'X-MRD-Datatype: float32' \
    --data-binary @$f \
    http://localhost:8080/v1/mrd/frame | jq
done

# The viz client opens the resulting MRD with HDF5 SWMR and prints frame counts
./build/viz_client --ws ws://localhost:8090/ws --data ./data/mrd
```

---

## Developer note: MRD ingestion helper

Both the HTTP (`POST /v1/mrd/ingest`) and WebSocket binary ingest paths share
`include/mrd_io.hpp`.  The helper writes the payload to a temporary file,
`fsync`s it, and atomically renames it into place before appending to
`index.jsonl` and rewriting `latest.json`.  It then emits the corresponding
metadata JSON over the WebSocket fan-out.  Reusing this helper keeps the HTTP
and WebSocket behaviours identical and guarantees clients only ever see fully
flushed MRD artifacts.

### Record→Replay (Mode B)
```bash
# RECORD: start server in dumpbox sink; session auto-named
./build/marshal --http 0.0.0.0:8080 --ws 0.0.0.0:8090 --data ./data --sink dumpbox --dumpbox-root ./data/dumpbox &
sleep 1
curl -s http://localhost:8080/health

# Post two files (dummy shown)
head -c 16384 </dev/urandom > ./data/tmp1.mrd
head -c 24576 </dev/urandom > ./data/tmp2.mrd
curl -s -H "Content-Type: application/octet-stream" --data-binary @./data/tmp1.mrd http://localhost:8080/v1/mrd/ingest
curl -s -H "Content-Type: application/octet-stream" --data-binary @./data/tmp2.mrd http://localhost:8080/v1/mrd/ingest

# Locate newest session (no placeholders)
SESSION_DIR=$(ls -dt ./data/dumpbox/* | head -n1)
echo "$SESSION_DIR"
find "$SESSION_DIR" -maxdepth 2 -type f -print

# REPLAY: restart server in MRD sink and play back the session
pkill -f "/build/marshal" || true
./build/marshal --http 0.0.0.0:8080 --ws 0.0.0.0:8090 --data ./data --sink mrd &
sleep 1
./build/playback --http http://localhost:8080 --data "$SESSION_DIR" --speed 1.0

# Verify MRDs rebuilt for clients
ls -l ./data/mrd
tail -n 5 ./data/mrd/index.jsonl || true
cat ./data/mrd/latest.json
```

---

## HTTP API

- **`POST /v1/mrd/ingest`** — Body: MRD bytes (`application/octet-stream`)
  - **MRD mode:** writes to `/data/mrd`, updates global `index.jsonl`/`latest.json`.
  - **Dumpbox mode:** writes to session under `/data/dumpbox/<SESSION>`, updates session `index.jsonl`/`latest.json`.
- **`POST /v1/mrd/frame`** — Body: raw voxel bytes (`application/octet-stream`)
  - `X-MRD-Stream`: logical identifier; the marshal will keep a matching `.mrd` file open
  - `X-MRD-Dimensions`: spatial size as `XxY` or `XxYxZ`
  - `X-MRD-Channels`: optional channel count (default `1`)
  - `X-MRD-Datatype`: `float32` (default) or `uint16`
  - Appends into the `/frames` dataset using HDF5 SWMR so readers can open the file while it grows
- **`GET /health`** — returns `ok`.
- **(If present) `GET /v1/mrd/since?ts=...&limit=...`** — convenience reader over `index.jsonl`.

---

## CLI flags

**marshal**
- `--http <addr:port>` (e.g., `0.0.0.0:8080`)
- `--ws <addr:port>` (e.g., `0.0.0.0:8090`)
- `--data <path>` (root data dir)
- `--sink <mrd|dumpbox>`
- `--dumpbox-root <path>` (only in dumpbox mode; default `/data/dumpbox`)
- `--dumpbox-session <name>` (optional; auto UTC ISO if omitted)

**playback**
- `--http http://host:port` (marshal base)
- `--data /path/to/dumpbox/session` (must contain `index.jsonl` and `files/`)
- `--speed <float>` (`1.0` ≈ real-time by timestamps; omit for fastest)

---

## Repo layout (high level)

```
src/
  marshal_main.cpp       # flags, startup, sinks
  marshal_http.hpp       # /v1/mrd/ingest (+ index/latest management)
  marshal_ws.hpp         # WS broker (for clients/viz if needed)
  marshal_state.hpp      # SinkMode + state
services/
  playback/
    playback_main.cpp    # HTTP-only dumpbox → /v1/mrd/ingest
include/
  atomic_write.hpp       # safe file writes (tmp + rename)
docs/
  overview.md            # Doxygen main page
```

---

## Usage runbooks

- See `README_MODE_A.md` for Live (copy-pasteable).
- See `README_MODE_B.md` for Record→Replay (copy-pasteable).
- See `docs/USAGE_WITH_CLIENTS.md` for end-to-end walkthroughs that include the
  sample clients (`fk_client`, `viz_client`, `ws_producer`).
