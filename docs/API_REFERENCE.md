# CWRU Data Marshal - Complete API Reference

Complete reference for connecting external clients to the CWRU Data Marshal system.

The marshal has two transport interfaces:
- **MRD TCP** (port `--mrd-port`, default `0`/disabled; `docker-compose.yml` sets `9100`) — primary interface for scanner and recon. Uses python-ismrmrd-server's 2-byte little-endian message ID framing (`CONFIG_FILE=1`, `CONFIG_TEXT=2`, `METADATA_XML_TEXT=3`, `CLOSE=4`, `TEXT=5`, `ACQUISITION=1008`, `IMAGE=1022`, `WAVEFORM=1026`). See [ARCHITECTURE.md](ARCHITECTURE.md) for the full wire protocol.
- **HTTP** (port `--http`, default 8080) — query/control interface for viz, pose, transform, health, and dump endpoints.

---

## MRD TCP Interface (Scanner and Recon)

Scanner-side clients connect via raw TCP to the marshal's `--mrd-port` and send MRD messages. The marshal forwards to recon ([python-ismrmrd-server](https://github.com/kspaceKelvin/python-ismrmrd-server) on MRD TCP, default port `9002`) via a second MRD TCP connection. When `--dump` is enabled, it also archives standard ISMRMRD objects to canonical H5 files. Reconstructed images are pushed back to the scanner on the same socket.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full wire protocol reference (message IDs, framing, body formats, session flow).

---

## HTTP Interface (Query and Control)

**Base URL:** `http://localhost:8080`

HTTP is not a scanner or recon data transport. Scanner data and recon return data use MRD TCP only. The HTTP API below is for non-scanner query/control clients.

---

### Query and control endpoints

#### GET /health

Health check. Returns `{"status": "ok", "uptime_s": 95}`.

#### GET /image/latest

**Live mode (default, no `--dump`):** returns a pointer to the closed companion snapshot — a stable path holding the most recently published live image update (2D slice, multi-slice stack image, or 3D volume image):
```json
{
  "path": "/session-data/live/from_reconstruction/latest_image.h5",
  "error": false
}
```

- `path` — stable reader-facing HDF5 file. Always a closed HDF5 atomically renamed by marshal on each incoming live IMAGE. Openable with default ISMRMRD / h5py / HDFView settings. Contents live under group `image_0`.
- `error` — true if reconstruction is currently failing.

When reconstruction has failed:
```json
{
  "path": "/session-data/live/from_reconstruction/latest_error.png",
  "error": true
}
```

If a scanner MRD TCP connection is active when recon fails, the scanner also receives a valid MRD `IMAGE(1022)` failure image on that same connection.

Before the current scan has published any live IMAGE, `GET /image/latest` returns `204 No Content`.

**Dump mode (`--dump`):** returns `404 Not Found` with body `{"error":"dump mode; no live snapshot"}`. Dump mode is archival-only — poll `/image/latest` only in live mode.

The per-scan history file (`<mode>/from_*/scan_<ts>.h5`) is an archival output produced on CLOSE. It is not a mid-scan reader interface in either mode. Clients that need the per-scan archive open it after the scan ends.

#### GET /debug/sinks

Per-pipeline counters for retention verification. Returns mode-specific JSON:

```json
// Live mode
{
  "mode": "live",
  "live": {
    "from_scanner": {"acq": 0, "img": 0, "wf": 1076, "open": false,
                     "dropped": 0, "queued_jobs": 0,
                     "high_watermark_hit": false},
    "from_reconstruction": { ... }
  }
}

// Dump mode
{
  "mode": "dump",
  "dump": {
    "from_scanner":        { "converted_acq": 240000, "converted_img": 0,
                             "converted_wf": 1875, "conversion_ok": true,
                             "spool_records": ..., "spool_bytes": ..., ... },
    "from_reconstruction": { ... },
    "dropped_records": 0, "dropped_bytes": 0, "had_overflow": false,
    "conversion_status": "complete"
  }
}
```

Use this to measure archival retention. `dropped_records` / `dropped` must remain zero under any non-disk-failure operating condition.

#### GET /debug/perf

Free-running performance counters since process start. Two snapshots and a delta between them give per-second rates. Intended for FPS / throughput debugging, regression detection, and the contract tripwires below.

```json
{
  "uptime_s": 95,
  "recv": {
    "scanner_images": 0,
    "recon_images": 234,
    "scanner_waveforms": 237
  },
  "publish_attempts": {"scanner": 0, "recon": 46},
  "latest_writer": {
    "enqueued": 46,
    "coalesced": 0,
    "dropped_oldest": 0,
    "completed": 46,
    "failed": 0,
    "max_queue_depth": 1,
    "last_write_us": 3625,
    "max_write_us": 41135,
    "last_drain_lag_us": 88,
    "max_drain_lag_us": 473
  }
}
```

Field meanings:

- `recv.*` — counts of MRD wire messages observed entering marshal per lane.
- `publish_attempts.*` — counts of `publish_latest_snapshot` calls per lane.
- `latest_writer.enqueued` — jobs accepted by `LatestImageWriter::enqueue`.
- `latest_writer.coalesced` — **contract tripwire**, must remain `0`. Non-zero means a same-destination coalesce path was introduced (a regression).
- `latest_writer.dropped_oldest` — overload-backstop drops at the 64-job queue cap. Should be 0 under normal load; non-zero indicates sustained writer stall (look at `max_write_us`).
- `latest_writer.completed` — jobs that finished writing successfully. **Should equal `enqueued - dropped_oldest`** in steady state.
- `latest_writer.failed` — exceptions during `write_latest_image_h5_file`.
- `latest_writer.max_queue_depth` — high-watermark queue depth.
- `latest_writer.{last,max}_write_us` — write durations (microseconds).
- `latest_writer.{last,max}_drain_lag_us` — time from `enqueue` to the worker popping the job (microseconds). High values indicate writer is the bottleneck.

Use `latest_writer.completed/sec` as the steady-state publish rate to localize regressions.

#### GET /transform

Returns the current slice transform delta and atomically zeros it (consume-on-read):
```json
{"through_plane_mm": 1.0, "readout_mm": 0.0, "phase_mm": 0.0, "rotation_rad": 0.0}
```

#### PUT /transform

Write a new slice transform delta:
```json
{"through_plane_mm": 1.0, "readout_mm": 0.0, "phase_mm": 0.0, "rotation_rad": 0.0}
```

#### POST /write/file_slice_translation

Slice-nudge command channel used by the WebGL client to step the MRI imaging slice by ±1. Body:
```json
{"client_id": "webgl", "sent_at": 1706126625123456789, "values": [1]}
```
`values` must be a single-element array whose value is exactly `+1` or `-1`; anything else returns `400`. The latest command is cached in memory (a `ts` field with `iso8601_now_ms()` is stamped on it). On success returns `{"file": "file_slice_translation", "direction": 1}`. (`/write/file_slice_translation.json` is accepted as an alias.)

#### GET /read/file_slice_translation

Returns the last cached slice-nudge command JSON (non-consuming — the value is not cleared). Returns `204 No Content` when no command has been posted yet. (`/read/file_slice_translation.json` is accepted as an alias.)

#### POST /pose

Submit a pose update:
```json
{"position": [1.0, 2.0, 3.0], "orientation": [0.0, 0.0, 0.707, 0.707]}
```

#### GET /pose

Returns the latest cached pose:
```json
{"position": [1.0, 2.0, 3.0], "orientation": [0.0, 0.0, 0.707, 0.707]}
```

#### GET /dump/scanner

List archived scanner HDF5 files under `dump/from_scanner/` (populated only when `--dump` is on):
```json
[{"path": "dump/from_scanner/scan_2026-04-14T16:30:00.123Z.h5", "size": 1234567, "modified": 1744651800}]
```

#### GET /dump/recon

List archived reconstruction HDF5 files under `dump/from_reconstruction/` (same format as `/dump/scanner`).

---

### Startup flags

| Flag | Default | Description |
|------|---------|-------------|
| `--http host:port` | `0.0.0.0:8080` | HTTP listen address (query/control endpoints) |
| `--mrd-port N` | `0` (disabled) | MRD TCP listen port (scanner connections). Listener only starts when a non-zero value is passed (`docker-compose.yml` passes `9100`). |
| `--dump-dir path` | `./data` | Session-data root. Holds `live/from_scanner/`, `live/from_reconstruction/`, and — when `--dump` is on — `dump/from_scanner/`, `dump/from_reconstruction/`. |
| `--dump` | off | Enable retrospective canonical ISMRMRD H5 dump writing |
| `--recon-host host` | (none) | Recon service hostname for MRD TCP forwarding |
| `--recon-port N` | `9002` | Recon service port. Used only when `--recon-host` is set. |
| `--ws-port N` | (none) | Optional WebSocket listener port |

---

## Robot Marshal API

**Base URL:** `http://localhost:8081`

### File-Based Communication Model

Robot Marshal uses a "virtual file system" where clients read/write JSON data to named endpoints.

### Endpoints

#### GET /

List available data channels (HTML).

#### GET /read/{filename}

Read latest data from a channel.

**Response:**
```json
{
  "entries": [
    {
      "sent_at": 1706126625123456789,
      "values": [2.0, 3.0, 4.0, 45.0, 45.0, 45.0]
    }
  ]
}
```

#### POST /write/{filename}

Write data to a channel.

**Request:**
```json
{
  "values": [1.0, 2.0, 3.0],
  "sent_at": 1706126625123456789
}
```

### Available Data Channels

| Channel | Description |
|---------|-------------|
| `localization_data` | Sensor positions |
| `catheter_base_configuration` | System config |
| `forward_kinematics` | FK calculations |
| `desired_planned_motion` | Motion plan |
| `tip_position_orientation` | Catheter tip pose |
| `biological_signals` | ECG/vitals |
| `surface_model_parameters` | Anatomy model |
| `user_input` | Doctor commands |
| `streaming_2D_images` | Live imaging |
| `3D_images` | 3D volumes |

---

## Example Usage

### curl

```bash
# MRI Marshal
curl http://localhost:8080/health
curl http://localhost:8080/image/latest
curl http://localhost:8080/pose
curl -X POST http://localhost:8080/pose \
  -H "Content-Type: application/json" \
  -d '{"position":[1,2,3],"orientation":[0,0,0.707,0.707]}'
curl http://localhost:8080/transform
curl -X PUT http://localhost:8080/transform \
  -H "Content-Type: application/json" \
  -d '{"through_plane_mm":1.0,"readout_mm":0,"phase_mm":0,"rotation_rad":0}'
curl -X POST http://localhost:8080/write/file_slice_translation \
  -H "Content-Type: application/json" \
  -d '{"client_id":"webgl","sent_at":1706126625123456789,"values":[1]}'
curl http://localhost:8080/read/file_slice_translation
curl http://localhost:8080/dump/scanner
curl http://localhost:8080/dump/recon

# Robot Marshal
curl http://localhost:8081/
curl http://localhost:8081/read/tip_position_orientation
curl -X POST http://localhost:8081/write/user_input \
  -H "Content-Type: application/json" \
  -d '{"values":[10,20,30],"sent_at":1706126625123456789}'
```

### Python

```python
import requests

# Get latest image path
resp = requests.get('http://localhost:8080/image/latest')
data = resp.json()
print(f"Image path: {data['path']}, error: {data['error']}")

# Submit pose
requests.post('http://localhost:8080/pose', json={
    "position": [1.0, 2.0, 3.0],
    "orientation": [0.0, 0.0, 0.707, 0.707]
})

# Read pose
pose = requests.get('http://localhost:8080/pose').json()
print(f"Position: {pose['position']}")

# Robot marshal
tip = requests.get('http://localhost:8081/read/tip_position_orientation').json()
print(f"Tip: {tip['entries'][0]['values'][:3]}")
```

---

## Data Flow

At a glance: scanner data and recon returns travel over **MRD TCP**; all query/control (image pointer, pose, transform, slice nudge, health, dump listing, debug) is **HTTP**. `<mode>` is `live` (default) or `dump` (`--dump`); the two are mutually exclusive and only the selected mode's subtree is populated. The only mid-scan readable interface is `latest_image.h5` (live mode only); `scan_<ts>.h5` is an archival output finalized on CLOSE.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full system diagram, session flow, and wire-protocol reference.
