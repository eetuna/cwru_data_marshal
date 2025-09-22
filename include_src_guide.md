# CWRU Data Marshal — `include/` and `src/` File-by-File Guide

> This document explains the key headers in **`include/`** and the core sources in **`src/`** (plus closely related client utilities). It’s written to help a new contributor understand what each file does, how they fit together, and the most important APIs. If your local tree is missing any of the files below, just skip that section — this guide mirrors the canonical layout used in this project.

---

## High-Level Architecture (quick refresher)

- **marshal (server)** exposes:
  - **HTTP** endpoints (ingest MRDs, health, config, pose get/update, read index).
  - **WebSocket** hub (broadcasts “pose” and “acq” JSON events to subscribed clients).
  - **Sink modes**:
    - **MRD** (live) → writes to `./data/mrd` (and updates `latest.json`, `index.jsonl`).
    - **DUMPBOX** (record) → writes to `./data/dumpbox/<SESSION>/files` (with per-session `latest.json`, `index.jsonl`).
- **Clients**:
  - `fk_client` (pseudo) posts pose updates to HTTP.
  - `viz_client` (pseudo) tails `latest.json` and listens to WS; prints event JSONs.
  - `playback` replays a recorded DUMPBOX session by re-POSTing MRDs in time order.
  - `mk_mrd` generates small valid MRD files for demos/tests.

Core state is kept in **`MarshalState`**; handlers (HTTP + WS) read/write via that state.

---

# `include/` (headers)

> Headers that are shared across translation units. Public headers under `include/` are intentionally small and stable; implementation-heavy details live in `src/`.

### `include/atomic_write.hpp`
**Purpose:** Safe file writes to avoid torn writes / partial files.
- **Key API:**
  - `void atomic_write(const std::filesystem::path& dst, const void* data, size_t n);`
    - Writes to `dst.tmp`, flushes, then `rename()` to `dst` (atomic on POSIX).
- **Used by:** HTTP MRD ingest (to write MRDs, `latest.json`), any code updating index files.

### `include/common/pose.hpp`
**Purpose:** Pose representation and helpers shared by server/clients.
- **Structs:**
  - `Pose` — `{ double p[3]; double R[9]; std::string frame; std::string source; std::chrono::system_clock::time_point t; }`
  - `PoseStore` — threadsafe setter/getter for the latest pose (internally uses mutex/atomic as needed).
- **Helpers:**
  - `nlohmann::json pose_to_json(const Pose&)` — serializes a pose into JSON `{p:[…], R:[…], frame, source, ts}`.
- **Used by:** HTTP `/v1/pose/update`, `/v1/pose/current`, viz client, and any WS broadcasts of pose events.

> If your tree has other headers under `include/`, they should follow the same pattern: small, reusable declarations only; implementation in `src/`.

---

# `src/` (core server + utilities)

> The **server** is split into: **main**, **HTTP**, **WS**, and **state**. Utilities include **playback** and **mk_mrd**.

## `src/marshal_main.cpp`
**Role:** Server entry point.
- Parses CLI flags:
  - `--http host:port` (default `0.0.0.0:8080`)
  - `--ws   host:port` (default `0.0.0.0:8090`)
  - `--data DIR` (default `./data`)
  - `--sink mrd|dumpbox` (default `mrd`)
  - `--dumpbox-root DIR` (default `./data/dumpbox`)
  - `--dumpbox-session NAME` (optional explicit session name)
- Constructs `MarshalState` and sets defaults (data dir, sink mode, dumpbox root/session).
- Binds and starts:
  - **HttpServer** (handles REST endpoints).
  - **WsServer** (manages WebSocket clients + broadcast).
- Ensures required directories exist at startup, depending on sink mode.
- Prints a one-line summary: listening addresses, sink, and data path.

**Key interactions:**
- Passes `MarshalState` by reference to HTTP and WS servers so they can read/write shared state.
- Hooks `state.ws_emit` to `WsServer::broadcast()` so HTTP handlers can broadcast events to all WS clients.

## `src/marshal_state.hpp`
**Role:** Shared state between HTTP and WS layers.
- **Fields:**
  - `SinkMode sink_mode` — enum `{ MRD, DUMPBOX }`.
  - `std::string data_dir` — root for `mrd/` and `dumpbox/`.
  - `std::string dumpbox_root`, `dumpbox_session`.
  - `std::atomic<uint64_t> seq` — monotonically increasing sequence (for filenames/events).
  - `PoseStore poses` — latest pose storage.
  - `std::function<void(const std::string&)> ws_emit` — callback set by WS server, used by HTTP to broadcast JSON strings.
  - Internal book-keeping (io_context*, WS client set, start time).
- **Thread-safety:** Pose set/get and WS broadcast emissions are safe when called from the HTTP handler threads.

## `src/marshal_http.hpp`
**Role:** All HTTP routing and handlers (built on Boost.Beast).
- **Endpoints:**
  - `GET /health` → `{status:"ok", uptime_s:<double>}` — no shared state required.
  - `GET /v1/config` → `{data_dir, ws_port, max_entries}` — convenience endpoint.
  - `GET /v1/pose/current` → returns last pose from `PoseStore` as JSON.
  - `POST /v1/pose/update` → accepts JSON body `{p:[3], R:[9], frame?, source?}`:
    - Validates shapes and sets `PoseStore`.
    - **Emits WS** event `{type:"pose", p:[3], R:[9], ts:…}` via `state.ws_emit(...)` (if configured).
    - Responds `{"status":"ok","pose":{...}}`.
  - `POST /v1/mrd/ingest` → accepts octet-stream body; writes an MRD file:
    - Chooses sink root:
      - **MRD**: `./data/mrd`
      - **DUMPBOX**: `./data/dumpbox/<session>/files` (ensures `<session>` exists).
    - Generates filename `{ISO8601_ms}_{seq}.mrd` using atomic `g_seq`.
    - **Atomic write** using `write_atomic()`.
    - Updates **`index.jsonl`** (append) and **`latest.json`** (replace) under the sink’s **index root**:
      - MRD mode → `./data/mrd/`
      - Dumpbox mode → `./data/dumpbox/<session>/`
    - **Emits WS** event `{type:"acq", path, seq, size_bytes, ts}` via `state.ws_emit(...)` (if configured).
    - Responds `201 Created` with the JSON entry.
  - `GET /v1/mrd/latest` → returns the current `latest.json` from MRD dir.
  - `GET /v1/mrd/since?ts=...&limit=...` → filters `index.jsonl` entries newer than `ts` (optionally limited).

- **Helpers (in this header):**
  - `iso8601_now_ms()` — RFC3339 UTC with milliseconds.
  - `iso8601_now()` — seconds precision (used by `/v1/pose/current`).
  - `ensure_dir(path)` — `create_directories` wrapper.
  - `write_atomic(dst, data, size)` — `.tmp` write + atomic rename.
  - `append_line(path, line)` — append helper for the index file.
  - `read_file_all(path, out)` — small file slurp for `latest.json`.
  - `parse_ts_limit(target, ts, limit)` — basic query parser for `/v1/mrd/since`.

- **Concurrency model:** per-connection `Session` class; async read → sync handler → async write; response lifetime is kept using `shared_ptr` in `respond()` to avoid dangling buffer issues.

## `src/marshal_ws.hpp` / `src/marshal_ws.cpp`
**Role:** WebSocket server (Boost.Beast based) and broadcast hub.
- Accepts connections on the configured WS endpoint (`--ws`).
- Tracks connected clients; each client is a `websocket::stream<tcp::socket>` (wrapped in a small session object).
- `broadcast(const std::string&)` sends a text message to all connected clients (best-effort; failures prune the client).
- On initialization, **sets** `state.ws_emit = [this](const std::string& msg){ broadcast(msg); };` so HTTP handlers can push events.

> If your project keeps WS inline with HTTP (same translation unit), the behavior will be identical: a central `broadcast` reachable via `state.ws_emit`.

## `src/playback_main.cpp`
**Role:** Replays a recorded **dumpbox session** by re-POSTing MRDs to `HTTP /v1/mrd/ingest` in index order and with optional timing.
- CLI flags:
  - `--http http://host:port[/base]` (default `http://localhost:8080`)
  - `--data <SESSION_DIR>` (path to the dumpbox session root that contains `index.jsonl` and `files/`)
  - `--speed <factor>` (1.0 realtime; 0 = fastest possible)
- Reads `<SESSION_DIR>/index.jsonl`. For each JSONL line (`{"file":"files/xxx.mrd","ts":"...","seq":...}`):
  - Builds absolute path `<SESSION_DIR>/<file>`.
  - Respects `--speed` to delay between sends (based on original timestamps).
  - HTTP POSTs the MRD to `/v1/mrd/ingest`.
- Output: progress lines `posted <file> (i/N)`; errors if files are missing or POST fails.

## `src/mk_mrd_main.cpp`
**Role:** Creates tiny **valid** MRD/HDF5 files for demos/tests.
- Outputs an `.h5` (or `.mrd`) with a minimal ISMRMRD header & dataset so ingest paths don’t reject it.
- CLI: `./build/mk_mrd <output_path>` — no options; just create the file.

---

# Clients (closely related to `src/`)

## `clients/fk_client/fk_client_main.cpp`
**Role:** Pseudo “FK publisher” — posts JSON pose updates to `/v1/pose/update`.
- Minimal CLI:
  - `--http http://host:port` (default `http://localhost:8080`)
  - `--count N` (default 50)
  - `--rate Hz` (default 10)
- Payload schema the server requires:
  - `{"p":[x,y,z], "R":[9], "source":"fk"}`
- Opens a fresh connection per POST to avoid reuse of a closed socket. Prints server response (or error body on 400).

## `clients/viz_client/viz_client_main.cpp`
**Role:** Pseudo “visualizer” — watches **`latest.json`** and listens on **WS**.
- CLI:
  - `--ws ws://host:port/ws` (default `ws://localhost:8090/ws`)
  - `--data <mrd_dir>` (default `./data/mrd`)
- Behavior:
  - Polls `<mrd_dir>/latest.json` once a second; prints `viz latest=...` when it changes.
  - Connects to WS and prints **any** incoming JSON text as `viz ws: {...}`.
  - If WS closes, it prints a line and continues polling `latest.json`.

---

## Data & Indexing Format (for reference)

### MRD sink (`./data/mrd/`)
- `*.mrd` files (atomic writes).
- `latest.json`:
  ```json
  {"type":"mrd","path":"./data/mrd/<file>.mrd","seq":<n>,"size_bytes":<n>,"ts":"<ISO8601_ms>"}
  ```
- `index.jsonl`: one JSON object per line — same fields as `latest.json` (append-only).

### Dumpbox sink (`./data/dumpbox/<SESSION>/`)
- `files/*.mrd` — per-session files.
- `index.jsonl` — **relative** paths like `"file":"files/<name>.mrd"` for portability.
- `latest.json` — last entry for the session.

---

## Typical Control Flows

1) **Live ingest**
   - Client POSTs MRD → server writes under `./data/mrd/`, updates index, **WS emits `{type:"acq"}`**.
   - `viz_client` sees `latest.json` change and/or WS event and prints a line.

2) **Pose stream**
   - `fk_client` POSTs `{"p":[3],"R":[9]}` → server updates `PoseStore`, **WS emits `{type:"pose"}`**.
   - `viz_client` prints the WS event in real time.

3) **Record → Replay**
   - Server in dumpbox mode saves MRDs + session index.
   - Later, `playback` reads session index and re-POSTs to `/v1/mrd/ingest` while `viz_client` watches.

---

## Where to add features

- **New HTTP endpoints** → add conditionals in `marshal_http.hpp::Session::handle()`.
- **Broadcast new event types** → package as a small JSON and call `state.ws_emit(json.dump())` from the handler.
- **Change sink policy** → adjust the MRD/DUMPBOX branch in `/v1/mrd/ingest` handler.
- **Authentication / CORS** → set headers in `respond()` or before calling it (centralized place).
- **Binary WS channel** → extend `WsServer` to support binary frames, and add a separate `state.ws_emit_bin` if needed.

---

## Gotchas & Conventions

- Always use **relative paths** in dumpbox `index.jsonl` (e.g., `files/xxx.mrd`) for portability.
- Keep `write_atomic()` for any file that readers may tail (`latest.json`) to avoid partial reads.
- Prefer **one POST per connection** in tiny sample clients; in production you can reuse connections with keep-alive and robust error handling.
- Don’t delete logs while the server is running; pipe through `tee` instead if you need to rotate.
- Default data root is `./data` (not `/data`) to avoid container mount surprises.

---

## Quick Map (name → responsibility)

- **Headers**
  - `include/atomic_write.hpp` — atomic write utility
  - `include/common/pose.hpp` — pose structs + JSON helpers

- **Core**
  - `src/marshal_main.cpp` — main() setup: args, state, HTTP/WS bootstrap
  - `src/marshal_state.hpp` — shared state + WS emit hook
  - `src/marshal_http.hpp` — all HTTP endpoints + file I/O helpers
  - `src/marshal_ws.hpp/.cpp` — WS server + broadcast hub

- **Utilities**
  - `src/playback_main.cpp` — re-POST MRDs from dumpbox sessions
  - `src/mk_mrd_main.cpp` — generate valid MRD files

- **Clients**
  - `clients/fk_client/fk_client_main.cpp` — pose publisher (pseudo)
  - `clients/viz_client/viz_client_main.cpp` — visualizer (pseudo)

---
