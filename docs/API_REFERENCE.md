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
  "path": "/latest/live/from_reconstruction/latest_image.h5",
  "error": false,
  "generation": 57
}
```

- `path` — stable reader-facing HDF5 file. Always a closed HDF5 atomically renamed by marshal on each incoming live IMAGE. Openable with default ISMRMRD / h5py / HDFView settings. Contents live under group `image_0`. With `--latest-dir` (the compose default: a RAM-backed dir shared with the viewer) the path is under that root; without the flag, under `--dump-dir`.
- `error` — true if reconstruction is currently failing.
- `generation` — monotonic publish counter. Remember the last value and skip re-reading the snapshot when it hasn't changed.

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

#### GET /image/latest.h5

The same closed snapshot served as **HTTP response bytes** (`Content-Type: application/x-hdf5`). Use this from clients that do not share the marshal's filesystem — anything on another machine, or a container without the volume:

```python
import io, urllib.request, h5py
r = urllib.request.urlopen("http://<marshal>:8080/image/latest.h5")
img = h5py.File(io.BytesIO(r.read()), "r")["dataset/image_0/data"]
```

- `ETag` carries the publish generation. Send `If-None-Match: <etag>` when polling; the marshal answers `304 Not Modified` until a new image is published, so only actual new images are downloaded.
- `204 No Content` before the first image; `503 Service Unavailable` while reconstruction is failing; `404` in dump mode.

#### GET /status

One-glance operational summary (read-only):

```json
{
  "mode": "live",
  "scanner_connected": true,
  "recon": {"configured": true, "target": "recon:9002", "connected": true},
  "scan": {"active": true, "file": "scan_2026-07-03T....h5"},
  "images": {"from_scanner_total": 300, "from_recon_total": 198},
  "last_image_age_s": 0.2,
  "disk_free_gb": 41.3,
  "uptime_s": 5231
}
```

`disk_free_gb` refers to the archive root (`--dump-dir`). `last_image_age_s` is null until the first snapshot publish.

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

#### Slice command channel (overview)

The marshal is a stand-in for Andrew's `slice_control` keyboard tool. It keeps six absolute numbers — `tx ty tz` (mm) and `rx ry rz` (degrees), zero at start and at every scan start — adds UI moves to them exactly as his keys do, and sends the six totals to the scanner-side `slice_agent --listen` (`dynamic-slice-position-main/agent`) as a 56-byte `SliceCommand` on TCP 9270. The agent rebuilds the rotation with its `buildRotMatrix` (`R = Rz·Ry·Rx`) and publishes to shared memory for the sequence. The channel is **off unless** `--slice-agent-host` is set (`SLICE_AGENT_HOST` in compose). See [SLICE_CONTROL_HANDOFF.md](SLICE_CONTROL_HANDOFF.md).

Common response fields:
```json
{"file": "...", "delivered": true, "agent_connected": true, "enabled": true,
 "state": {"tx": 0.0, "ty": 0.0, "tz": 1.0, "rx_deg": 10.0, "ry_deg": 0.0, "rz_deg": 0.0},
 "geometry": {"position": [...], "read_dir": [...], "phase_dir": [...], "slice_dir": [...]}}
```
`state` = the six numbers just sent; `geometry` = the axes they imply (rows of `buildRotMatrix`). `delivered` = the packet was written to a connected agent (the first command of a session waits for the lazy connect, bounded by the connect timeout). `false` with `enabled:true` = agent unreachable; the state is still advanced and re-sent when the agent appears. Every accepted request is also cached and readable at the matching `GET /read/...` (`204` until the first POST).

#### POST /write/file_slice_translation

Legacy ±1 nudge from the WebGL `±` buttons. Body:
```json
{"client_id": "webgl", "sent_at": 1706126625123456789, "values": [1]}
```
`values` must be a single-element array whose value is exactly `+1` or `-1`; anything else returns `400`. Equals `slice_delta` with `translation_mm = [0, 0, ±nudge]` (`--slice-nudge-mm`, default 1): **Andrew's PgUp / PgDn**. Returns the common fields plus `"direction"`. (`/write/file_slice_translation.json` is accepted as an alias.)

#### GET /read/file_slice_translation

Returns the last cached slice-nudge request JSON (non-consuming). `204 No Content` when no command has been posted yet. (`/read/file_slice_translation.json` is accepted as an alias.)

#### POST /write/slice_target

Absolute slice prescription from the UI: "put the slice exactly here, facing this way". Body:
```json
{"position": [12.5, -3.0, 40.0],
 "read_dir": [1,0,0], "phase_dir": [0,1,0], "slice_dir": [0,0,1]}
```
`position` = slice center in mm (scanner PCS); the three direction vectors are taken as rows 0/1/2 of `buildRotMatrix` and converted to `rx ry rz`; the result replaces the six numbers and is sent. Rejected with `400` and nothing sent when: a vector is not unit length or the three are not mutually orthogonal (tolerance 1e-3); the frame is left-handed (`read_dir × phase_dir` must equal `slice_dir`); or any `position` component exceeds `--slice-max-abs-mm` (300). Returns the common fields. This is the one path Andrew's own tool never exercises (his keys never send a header-derived pose) — see SLICE_CONTROL_HANDOFF.md.

#### GET /read/slice_target

Returns the last cached prescription (non-consuming); `204 No Content` before the first POST.

#### POST /write/slice_delta

Relative slice move — the command style for incremental UI buttons. Body (at least one field; the other defaults to zeros):
```json
{"translation_mm": [1.5, 0, -2.0], "rotation_rad": [0, 0.1, 0]}
```
Exactly `slice_control.cpp`'s key arithmetic: `translation_mm[k]` moves `(tx,ty,tz)` along **row k of `buildRotMatrix(rx,ry,rz)`** (0 = readout → arrows, 1 = phase → arrows, 2 = slice normal → PgUp/PgDn); `rotation_rad[k]` (converted to degrees) is **added to the k-th angle** (`rx` → W/S, `ry` → D/A, `rz` → E/Q). Translation is applied before the rotation in the same request. Per-command magnitude is clamped (`--slice-max-step-mm` 50, `--slice-max-step-deg` 30) and the accumulated `|tx|,|ty|,|tz|` against `--slice-max-abs-mm` (300) → `400`, nothing sent, request not cached. Returns the common fields.

#### GET /read/slice_delta

Returns the last accepted delta request (non-consuming); `204 No Content` before the first POST.

#### POST /write/slice_reset

Zero the six numbers and send them (Andrew's `0` key). Zero = the identity slice (axial through isocenter, where the agent puts the slice on connect), not the console prescription. Body ignored. Returns the common fields.

#### GET /read/slice_commanded

The six numbers last sent to the agent, the axes they imply, a `count` of commands this scan, and agent status:
```json
{"state": {"tx": 0.0, "ty": 0.0, "tz": 1.0, "rx_deg": 10.0, "ry_deg": 0.0, "rz_deg": 0.0},
 "geometry": {"position": [0,0,1], "read_dir": [1,0,0], "phase_dir": [0,0.985,-0.174], "slice_dir": [0,0.174,0.985]},
 "ts": "2026-08-25T...", "count": 3,
 "agent": {"enabled": true, "connected": true, "reconnects": 0}}
```
`204 No Content` until the first command of the scan. `GET /status` also carries a `slice_agent` block (`enabled`, `connected`, `reconnects`, `commands_this_scan`, `has_state`).

#### GET /read/slice_geometry

Position + orientation per slice index as observed in the current scan's image headers (both lanes — scanner-sent and recon-returned images). Cleared at each scan start; `204 No Content` until the first image. Response:
```json
{"latest_slice": 2,
 "slices": {"2": {"slice": 2, "position": [10.5, -20.0, 30.0],
                  "read_dir": [1,0,0], "phase_dir": [0,1,0], "slice_dir": [0,0,1],
                  "ts": "2026-07-07T..."}}}
```

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
| `--latest-dir path` | (unset) | Alternate root for the transient snapshot artifacts (`latest_image.h5`, `latest_error.png`). Unset = they live under `--dump-dir` (historical layout, byte-identical behavior). The compose file points this at a shared RAM-backed dir (`/dev/shm/cwru-latest`) so per-volume snapshot I/O never touches — or stalls on — the archive disk. Archives are unaffected either way. |
| `--recon-host host` | (none) | Recon service hostname for MRD TCP forwarding |
| `--recon-port N` | `9002` | Recon service port. Used only when `--recon-host` is set. |
| `--recon-connect-timeout-ms N` | `5000` | Bound on recon DNS resolve + TCP connect. On expiry the scan proceeds archived-only (scanner receives a failure image and a marshal CLOSE). |
| `--recon-close-timeout-ms N` | `30000` | After the scanner's CLOSE is forwarded, how long to wait for recon to flush tail images and send its own CLOSE. On expiry the marshal emits its own CLOSE so the scanner never hangs. Sized for slow recons (e.g. GRAPPA on a remote VM). |
| `--ws-port N` | (none) | Optional WebSocket listener port |
| `--slice-agent-host host` | (none = channel off) | Scanner-side `slice_agent --listen` host (the MARS). Enables the slice command channel. Compose: `SLICE_AGENT_HOST`. |
| `--slice-agent-port N` | `9270` | `slice_agent` port. Compose: `SLICE_AGENT_PORT`. |
| `--slice-max-step-mm N` | `50` | Per-command clamp on relative translation components. |
| `--slice-max-step-deg N` | `30` | Per-command clamp on relative rotation components. |
| `--slice-max-abs-mm N` | `300` | Clamp on the accumulated `|tx|,|ty|,|tz|` (any path). |
| `--slice-nudge-mm N` | `1` | Millimetres per press of the legacy `±` endpoint. |
| `--slice-resend-ms N` | `2000` | After every (re)connect, re-send the last command at 10 Hz for this long (the agent publishes an identity transform on connect; this overwrites it before the next frame). |

---

## Robot Marshal API

**Base URL:** `http://localhost:8081`

### File-Based Communication Model

Robot Marshal uses a "virtual file system" where clients read/write JSON data to named endpoints.

### Endpoints

#### GET /

List available data channels (HTML).

#### GET /read/{filename}

Read latest data from a channel. `{filename}` is an exact map lookup keyed by the verbatim entries in the robot marshal's `files.json` — the full name including the `file_` prefix and `.json` extension (e.g. `/read/file_tip_position_orientation.json`).

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

The channels are exactly the entries in the robot marshal's `files.json`. Each is addressed by its verbatim name (`file_` prefix, `.json` extension).

| Group | Channels |
|-------|----------|
| Imaging | `file_3D_images.json`, `file_streaming_2D_images.json` |
| Catheter / robot | `file_tip_position_orientation.json`, `file_forward_kinematics.json`, `file_catheter_base_configuration.json`, `file_desired_planned_motion.json`, `file_localization_data.json` |
| Sensing / physio | `file_force_sensing.json`, `file_biological_signals.json`, `file_surface_model_parameters.json` |
| UI / markers | `file_user_input.json`, `file_rendered_2D_image.json`, `file_slice_translation.json`, `file_will_render_2D_image.json`, `file_will_update_texture_from_server.json`, `file_updated_texture_from_server.json`, `file_will_update_force_sensing_from_server.json`, `file_updated_force_sensing_from_server.json` |

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
curl http://localhost:8081/read/file_tip_position_orientation.json
curl -X POST http://localhost:8081/write/file_user_input.json \
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
tip = requests.get('http://localhost:8081/read/file_tip_position_orientation.json').json()
print(f"Tip: {tip['entries'][0]['values'][:3]}")
```

---

## Data Flow

At a glance: scanner data and recon returns travel over **MRD TCP**; all query/control (image pointer, pose, transform, slice nudge, health, dump listing, debug) is **HTTP**. `<mode>` is `live` (default) or `dump` (`--dump`); the two are mutually exclusive and only the selected mode's subtree is populated. The only mid-scan readable interface is `latest_image.h5` (live mode only); `scan_<ts>.h5` is an archival output finalized on CLOSE.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full system diagram, session flow, and wire-protocol reference.
