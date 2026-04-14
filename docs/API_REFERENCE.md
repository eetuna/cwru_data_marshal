# CWRU Data Marshal - Complete API Reference

Complete reference for connecting external clients to the CWRU Data Marshal system.

The marshal has two transport interfaces:
- **MRD TCP** (port `--mrd-port`, default 9100) — primary interface for scanner and recon. Uses python-ismrmrd-server's 2-byte message ID framing. See [ARCHITECTURE.md](ARCHITECTURE.md) for the wire protocol.
- **HTTP** (port `--http`, default 8080) — query/control interface for viz, pose, transform, health, and dump endpoints.

---

## MRD TCP Interface (Scanner and Recon)

Scanner-side clients (kspace_streamer, image_streamer) connect via raw TCP to the marshal's `--mrd-port` and send MRD messages. The marshal forwards to recon via a second MRD TCP connection. When `--dump` is enabled, it also archives standard ISMRMRD objects to canonical H5 files. Reconstructed images are pushed back to the scanner on the same socket.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full wire protocol reference (message IDs, framing, body formats, session flow).

---

## HTTP Interface (Query and Control)

**Base URL:** `http://localhost:8080`

HTTP is not a scanner or recon data transport. Scanner data and recon return data use MRD TCP only. The HTTP API below is for non-scanner query/control clients.

---

### Query and control endpoints

#### GET /health

Health check. Returns `{"status": "ok"}`.

#### GET /image/latest

Returns a pointer to the current scan's live reconstructed-image file plus a hint for which image group to read:
```json
{
  "path": "/session-data/live/from_reconstruction/scan_2026-04-14T16:30:00.123Z.h5",
  "error": false,
  "slices": 42,
  "newest_series": 3
}
```

- `path` — current scan's live HDF5 file. Opened with the standard ISMRMRD `Dataset` reader.
- `error` — true if reconstruction is currently failing.
- `slices` — count of images appended to this scan so far.
- `newest_series` — highest `image_series_index` (volume number) seen this scan. Clients open group `image_<newest_series>` to read the newest volume's slices without probing.

When reconstruction has failed:
```json
{
  "path": "/session-data/live/from_reconstruction/latest_error.png",
  "error": true,
  "slices": 0,
  "newest_series": 0
}
```

If a scanner MRD TCP connection is active when recon fails, the scanner also
receives a valid MRD `IMAGE(1022)` failure image on that same connection.

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
[{"path": "dump/from_scanner/scan_2026-04-14T16:30:00.123Z.h5", "size": 1234567, "modified": "2026-04-14T..."}]
```

#### GET /dump/recon

List archived reconstruction HDF5 files under `dump/from_reconstruction/` (same format as `/dump/scanner`).

---

### Startup flags

| Flag | Default | Description |
|------|---------|-------------|
| `--http host:port` | `0.0.0.0:8080` | HTTP listen address (query/control endpoints) |
| `--mrd-port N` | `9100` | MRD TCP listen port (scanner connections) |
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

```
Scanner/K-Space Streamer
    │
    │ MRD TCP (port 9100)
    │ CONFIG_FILE + METADATA_XML + ACQUISITION×N + WAVEFORM + CLOSE
    v
MRI Marshal (appends to live/from_scanner/scan_<ts>.h5; with `--dump`, also dump/from_scanner/scan_<ts>.h5)
    │
    │ MRD TCP (to recon-host:recon-port)
    │ same messages forwarded
    v
Reconstruction Service (MRD TCP server)
    │
    │ MRD TCP return messages on same connection
    v
MRI Marshal (appends to live/from_reconstruction/scan_<ts>.h5; with `--dump`, also dump/from_reconstruction/scan_<ts>.h5)
    │                              │
    │ MRD TCP return messages      │ HTTP GET /image/latest -> {"path", "error", "slices", "newest_series"}
    │ pushed back to scanner       │
    v                              v
Scanner                        Viz Client (opens file, reads image_<newest_series>, renders with OpenCV)
```

Scanner data transport is MRD TCP. Query/control is HTTP. Each scan produces a new `scan_<ts>.h5` file under `live/` (and, with `--dump`, under `dump/`); per-scan files are appended while the scan is running and remain on disk afterwards. Live clients read the file at the path returned by `/image/latest` during and after the scan; `newest_series` tells them which image group is current.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full system diagram and wire protocol reference.
