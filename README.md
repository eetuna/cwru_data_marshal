# CWRU Data Marshal

The marshal is a tiny, fast HTTP/WebSocket hub for reconstructed MRI data (ISMRMRD).
It always runs in exactly **one** of two modes:

- **Mode A – Live file access:** scanner → `marshal` → **MRD files** → clients read files.
- **Mode B – Record → Replay:**
  - **Record now:** scanner → `marshal` → **dumpbox session** (no `/data/mrd` writes).
  - **Replay later:** `playback` → `marshal` → **MRD files** → clients read files using the same interface as live.

Keeping the sink explicit makes operations predictable, observability trivial, and
failure handling much easier.

---

## Why you might want this

- 🚦 **Two explicit sinks** keep live and replay data flows separate and auditable.
- 🔁 **Playback utility** replays dumpbox sessions by re-posting them to the marshal.
- 🔓 **HDF5 SWMR streaming** (`POST /v1/ismrmrd/frame`) lets clients open growing MRD
  files safely while the marshal appends new frames.
- 🧩 **Clients stay simple:** they only ever read MRD files on disk.
- ⚙️ **No heavy dependencies:** Boost.Asio/Beast and nlohmann/json do the heavy lifting.

---

## First-time setup (10 minutes)

1. **Install build tools and SDKs** (Ubuntu/Debian):
   ```bash
   sudo apt-get update
   sudo apt-get install -y \
     build-essential cmake ninja-build pkg-config \
     libboost-all-dev libhdf5-dev libismrmrd-dev
   ```
   These match the toolchain baked into `docker/Dockerfile` so local builds
   have the HDF5, Boost, and ISMRMRD headers needed for SWMR support.
   Install the lightweight Python runtime used by the helpers:
   ```bash
   sudo apt-get install -y python3 python3-pip python3-venv python3-numpy
   python3 -m pip install --user --upgrade pip
   python3 -m pip install --user ismrmrd h5py numpy
   ```
2. **Configure + build** the binaries:
   ```bash
   mkdir -p build
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
   cmake --build build -j"$(nproc)"
   ```
3. **Pick a sink:**
   - Live MRD files: `./build/marshal --http 0.0.0.0:8080 --ws 0.0.0.0:8090 --data ./data --sink mrd`.
   - Record-only dumpbox: `./build/marshal --http ... --sink dumpbox --dumpbox-root ./data/dumpbox`.
4. **Ingest data:**
   - Upload MRD files via `POST /v1/mrd/ingest` (curl examples below).
   - Append SWMR frames via `POST /v1/ismrmrd/frame` with ISMRMRD image
     messages to stream voxels into a growing dataset that readers can follow in
     near real time.
5. **Inspect outputs:** MRD sink writes to `./data/mrd`; dumpbox sink writes to
   `./data/dumpbox/<session>/files`. Metadata is mirrored in `index.jsonl` and
   `latest.json` for simple polling clients.

The step-by-step runbooks in [`README_MODE_A.md`](README_MODE_A.md) and
[`README_MODE_B.md`](README_MODE_B.md) expand on these steps with copy/paste-able
commands, cleanup instructions, and verification tips.

Prefer containerized workflows? See [`docs/CONTAINERS.md`](docs/CONTAINERS.md) for
a deep dive into the devcontainer image, the multi-stage Dockerfile, and the
`docker compose` demo stack that spins up the marshal alongside example clients.

---

## Storage layout

**Live (MRD sink)**
```
/data/mrd/
├─ streamA.mrd
├─ streamB.mrd
├─ index.jsonl
└─ latest.json
```

**Record (Dumpbox sink)**
```
/data/dumpbox/2025-09-20T01:23:00Z/
├─ files/
│  ├─ streamA.mrd
│  └─ streamB.mrd
├─ index.jsonl
└─ latest.json
```

---

## Build

### Binaries produced

- `build/marshal` — HTTP/WebSocket server.
- `build/playback` — HTTP-only session replayer (feeds dumpbox sessions back to the marshal).
- `build/viz_client` — simple HDF5 SWMR-aware visualization client with ASCII slice preview.
- `build/image_streamer` — synthetic multi-slice generator that posts frames to the marshal.
- `build/mk_mrd` — utility that creates valid placeholder MRD files for smoke tests.

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

The marshal accepts ISMRMRD Image messages. Each POST body is an
`ISMRMRD::ImageHeader` immediately followed by the raw voxel payload. See
[`docs/API_REFERENCE.md`](docs/API_REFERENCE.md#post-v1ismrmrdframe) for the
exact layout. Two helper utilities now live in-tree:

- **C++ (`image_streamer`)** — builds with the rest of the project. Generates a
  multi-slice, single-channel float32 series with subtle temporal wobble.
- **Python (`tools/stream_image_series.py`)** — mirrors the C++ generator using
  `numpy`/`ismrmrd`. Both land their temporary `.bin` artifacts under `./data`.
  Width/height, slice count, and cadence can be overridden with flags such as
  `--nx 128 --ny 128 --nslices 8 --dt-ms 50`.

Single-frame helpers (handy for testing payloads):

```bash
# C++
./build/make_image_message --out ./data/image_message.bin

# Python (same output path)
python3 tools/make_image_message.py

curl -fsS \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-MRD-Stream: demo_stream' \
  --data-binary @./data/image_message.bin \
  http://localhost:8080/v1/ismrmrd/frame | jq
```

Continuous generator:

```bash
# C++ streaming client (Ctrl+C to stop)
./build/image_streamer --http http://localhost:8080 --stream demo_stream --nx 128 --ny 128 --nslices 8 --dt-ms 200

# Python variant (same CLI flags)
python3 tools/stream_image_series.py --http http://localhost:8080 --stream demo_stream --nx 128 --ny 128 --nslices 8 --dt-ms 200

# Follow the growing dataset (ASCII preview per slice)
./build/viz_client --ws ws://localhost:8090/ws --data ./data/mrd
```

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

## Developer note: MRD ingestion helper

Both the HTTP (`POST /v1/mrd/ingest`) and WebSocket binary ingest paths share
`include/mrd_io.hpp`.  The helper writes the payload to a temporary file,
`fsync`s it, and atomically renames it into place before appending to
`index.jsonl` and rewriting `latest.json`.  It then emits the corresponding
metadata JSON over the WebSocket fan-out.  Reusing this helper keeps the HTTP
and WebSocket behaviours identical and guarantees clients only ever see fully
flushed MRD artifacts.

## HTTP API

- **`POST /v1/mrd/ingest`** — Body: MRD bytes (`application/octet-stream`)
  - **MRD mode:** writes to `/data/mrd`, updates global `index.jsonl`/`latest.json`.
  - **Dumpbox mode:** writes to session under `/data/dumpbox/<SESSION>`, updates session `index.jsonl`/`latest.json`.
- **`POST /v1/ismrmrd/frame`** — Body: ISMRMRD Image header + voxels
  - `X-MRD-Stream`: logical identifier; the marshal will keep a matching `.mrd` file open
  - Appends into `/images/data` using HDF5 SWMR so readers can open the file while it grows
- **`GET /health`** — returns `ok`.
- **(If present) `GET /v1/mrd/since?ts=...&limit=...`** — convenience reader over `index.jsonl`.

See [`docs/API_REFERENCE.md`](docs/API_REFERENCE.md) for details, payload examples,
and error semantics aimed at new integrators.

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
