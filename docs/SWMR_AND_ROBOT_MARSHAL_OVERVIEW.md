# SWMR and MRI/Robot Marshal Interaction Overview

## 1) What is SWMR (Single-Writer Multiple-Reader)?

SWMR is an HDF5 file mode that allows **one writer** process to append data while **multiple readers** can safely access the same file **without blocking** or seeing corrupted content. It is designed for real-time workflows where data is continuously produced and consumers need low-latency access to the latest data.

**In plain terms**:
- The writer appends new frames to an HDF5 dataset.
- Readers can open the file concurrently and **refresh** to see new frames as they arrive.
- It avoids partial reads and corruption that can happen if a reader sees a file while it is being written in a non-SWMR mode.

**Why it matters here**:
The MRI marshal writes imaging frames to HDF5 and the visualizer (or external clients) need to read the newest frame right away. SWMR enables this without stopping or locking the writer, so the system can sustain real-time throughput.

**Key behaviors**:
- **Writer** periodically flushes (H5Dflush/H5Fflush) to make data visible.
- **Readers** call `refresh()` to see new data.
- SWMR is about **consistency + concurrency**, not compression or caching.

---

## 2) How the MRI Marshal and Robot Marshal Work Together

The MRI marshal and robot marshal are **separate services** that run side-by-side. They are not tightly coupled in code, but they are **coordinated by the demo/test workflows** and can be consumed together by external clients.

### Interaction Model

**MRI Marshal (this repo)**:
- Handles MRI data ingestion, HDF5 storage, and WS notifications.
- Primary endpoints: `/v1/mrd/*`, `/v1/bio/*`, `/v1/pose/*`

**Robot Marshal (demo binary)**:
- Handles robot state files and commands via HTTP.
- Files are read/written under a simple file routing system.

### How the Demo Integrates Them

The script `scripts/run_demo_simultaneous.sh` (and the non-interactive version) launches both:
- MRI marshal on `http://127.0.0.1:8080`
- Robot marshal on `http://127.0.0.1:8081`

The demo then:
1. Streams MRI frames into the MRI marshal (via `image_streamer`).
2. Sends ECG + pose updates into the MRI marshal.
3. Spins up robot clients that read/write data files in the robot marshal.
4. Shows a single visualization window that reads MRI frames.

### What an External Client Would Do

If an external system wants to use both services:
- Connect to the MRI marshal for imaging data (HTTP + WS).
- Connect to the robot marshal for robot state (HTTP read/write).
- Correlate the data by timestamps (`ts` and `t_ms` fields).

### Important Note

The services are **not inherently synchronized** by shared code; synchronization is done by:
- Matching timestamps.
- Coordinated start/stop in demos.
- Client-side logic if cross-correlation is required.

---

## 3) Practical Endpoints Summary (for External Users)

**MRI Marshal**
- `POST /v1/mrd/frame` — low-latency streaming
- `POST /v1/mrd/ingest` — bulk ingest
- `GET /v1/mrd/latest` — most recent frame metadata
- `GET /v1/mrd/since` — historical metadata
- `WS /ws` — real-time metadata broadcast

**Robot Marshal**
- `GET /read/<file>` — read robot state
- `POST /write/<file>` — write robot state

---

## 4) Summary

SWMR enables safe, concurrent reading of MRI data while it is being written. The MRI marshal uses this to serve real-time data to clients without blocking ingestion. The robot marshal is a separate service used for robot state updates; the integration between the two is operational (scripts + client workflows) rather than a single merged service. External clients can interact with both services concurrently and align them via timestamps.

