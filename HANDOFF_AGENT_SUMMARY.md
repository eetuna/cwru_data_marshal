# Handoff: Repo Review, Entry Points, Capabilities, and Client Integration

**Repo**: `cwru_data_marshal`  
**Purpose**: Real-time MRI data streaming + robot state relay with persistent logging and WebSocket fanout.  
**Primary Binary**: `marshal` (MRI marshal), plus a separate robot marshal demo binary.  

---

## 1) What This Repo Does (High-Level)

- **MRI data ingestion and streaming** using HDF5 SWMR for real-time visualization.
- **Robot state relay** using a lightweight HTTP-based cache system.
- **Dual-marshal architecture**: MRI marshal for high-throughput imaging, robot marshal for state/commands.
- **Clients** provided for reference and testing: image streamer, visualization client, WS producer, robot clients.

---

## 2) Primary Entry Points

### Binaries (build outputs)
- `./build/marshal`: MRI data marshal (HTTP + WS)
- `./build/image_streamer`: Posts MRI frames to `/v1/mrd/frame`
- `./build/viz_client`: OpenCV viewer for `/v1/mrd/latest`
- `./build/robot_marshal_demo`: Robot marshal (simple key/value files served by HTTP)

### Scripts
- **Interactive demo**: `scripts/run_demo_simultaneous.sh`
- **Non-interactive demo**: `scripts/run_demo_simultaneous_noninteractive.sh`
- **Benchmark suite**: `scripts/benchmarks/*.sh`
  - `latency_benchmark.sh`
  - `notification_latency.sh`
  - `read_latency_bench.sh`

---

## 3) Core Capabilities

### MRI Marshal
- HTTP ingest endpoints:
  - `POST /v1/mrd/frame` for frame-by-frame streaming (low latency).
  - `POST /v1/mrd/ingest` for bulk ingestion.
- Metadata persistence:
  - Writes `index.jsonl` and `latest.json` with fsync for durability.
- HDF5 SWMR data storage:
  - Safe concurrent read/write for real-time visualization.
- WebSocket broadcasts:
  - Broadcast per-frame updates and optional topic-based events.
  - Topics include `mrd`, `mrd.ingest`, `pose`, `bio`.
- Graceful shutdown:
  - SIGTERM/SIGINT triggers flush-all with timeout guard.
- Stream management:
  - Idle stream cleanup scheduled periodically.

### Robot Marshal (Demo)
- Lightweight HTTP server storing JSON payloads in named files.
- Used by robot clients that pass data in a circular chain.

---

## 4) External Client Integration

### Basic Startup (MRI Marshal)
```bash
./build/marshal --http 127.0.0.1:8080 --ws 127.0.0.1:8090 --data ./data
```

### HTTP API Usage (MRI)
#### Post MRI frames (streaming)
```
POST /v1/mrd/frame
Headers:
  X-MRD-Stream: <stream_id>
  X-MRD-Session: <session_id> (optional)
Body:
  ISMRMRD::ImageHeader + raw payload
```

#### Bulk ingest
```
POST /v1/mrd/ingest
Body: raw MRD binary
```

#### Latest frame metadata
```
GET /v1/mrd/latest
```

#### Historical lookup
```
GET /v1/mrd/since?ts=<timestamp>&limit=<n>
GET /v1/mrd/since?last=<n>
```

#### Health
```
GET /health
```

### WebSocket Usage
Connect:
```
ws://<host>:8090/ws
```

Subscribe to topic:
```json
{"subscribe":"mrd"}
```

Messages:
- Stream updates are JSON with fields like `path`, `frame_index`, `dims`, `flushed`, `t_ms`, `ts`.

---

## 5) Data Flow Summary

1. MRI client posts frame to `POST /v1/mrd/frame`.
2. Marshal writes to HDF5 SWMR dataset and updates index/latest.
3. Marshal emits WS notification (`topic=mrd`).
4. `viz_client` polls `/v1/mrd/latest` and reads HDF5 SWMR data.

---

## 6) Known Design Choices

- **Durability tradeoff**: `index.jsonl` + `latest.json` use fsync for durability; can be a throughput bottleneck.
- **Flush policy**: batch flushes for HDF5 (default 4 frames or 50ms).
- **WebSocket**: async queue per session, now safe against UAF and empty writes.

---

## 7) What Needs Attention (Future Work)

### Performance
- Move `fsync` from ingest hot path to async/batched queue.
- Consider async HDF5 write pipeline (requires careful SWMR handling).

### Robustness & Observability
- Add more structured logging and metrics endpoints.
- Expand `/health` to include status counters and sink health.

### Client UX / Visualization
- `viz_client` now reopens HDF5 on file replacement, but could use retry backoff on refresh errors.

### Benchmarks
- `read_latency_bench.sh` now uses strict flush for single-frame SWMR; consider a flag to test production flush settings.

---

## 8) Build + Test Commands

Build:
```bash
cmake -S . -B build
cmake --build build
```

Key benchmarks:
```bash
scripts/benchmarks/latency_benchmark.sh
scripts/benchmarks/notification_latency.sh
scripts/benchmarks/read_latency_bench.sh
```

---

## 9) Repository Structure Map (Short)

- `src/`: MRI marshal core (HTTP + WS + HDF5)
- `include/`: IO helpers and shared structures
- `clients/`: reference clients (viz, streamer, ws producer)
- `scripts/`: demo and benchmark workflows
- `docs/`: architecture, API reference, guides

---

## 10) Context Notes for Next Agent

- Recent fixes: WS lifetime safety, graceful shutdown timeout, HDF5 type cleanup.
- Non-interactive demo script exists and now defaults to 30s.
- If GUI freezes, verify marshal still running and WS/HTTP latest is advancing.
- If shutdown crashes reappear, preserve `data_demo_mri/server.log` and enable core dumps for backtrace.

