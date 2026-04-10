# CWRU Data Marshal - Complete API Reference

Complete reference for connecting external clients to the CWRU Data Marshal system.

The marshal has two transport interfaces:
- **MRD TCP** (port `--mrd-port`, default 9100) — primary interface for scanner and recon. Uses python-ismrmrd-server's 2-byte message ID framing. See [ARCHITECTURE.md](ARCHITECTURE.md) for the wire protocol.
- **HTTP** (port `--http`, default 8080) — query/control interface for viz, pose, transform, health, and dump endpoints.

---

## MRD TCP Interface (Scanner and Recon)

Scanner-side clients (kspace_streamer, image_streamer) connect via raw TCP to the marshal's `--mrd-port` and send MRD messages. The marshal archives all data and forwards to recon via a second MRD TCP connection. Reconstructed images are pushed back to the scanner on the same socket.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full wire protocol reference (message IDs, framing, body formats, session flow).

---

## HTTP Interface (Query and Control)

**Base URL:** `http://localhost:8080`

### Scanner-facing endpoints (HTTP fallback)

These endpoints accept ISMRMRD data over HTTP as a fallback for clients that don't use MRD TCP. If `--recon-host`/`--recon-port` are configured, data is also forwarded to the reconstruction service via MRD TCP.

#### POST /header

Start a new scan. Body: ISMRMRD XML header as raw bytes.

Marshal opens a new HDF5 archive at `from_scanner/scan_<timestamp>.h5`, writes the XML header, and forwards to recon. Returns 200 on success, 400 if XML is malformed.

#### POST /config

Set reconstruction config. Body: config name as plain text (e.g. `simplefft`).

Required after `/header`, before `/frame`. Forwarded to recon.

#### POST /frame

Submit one ISMRMRD message. Body: raw ISMRMRD wire bytes.

Marshal classifies the message type (ACQUISITION, IMAGE, WAVEFORM, or UNKNOWN) and archives it to the scanner HDF5 file. All types are forwarded to recon. Returns 202.

Rejected with 409 if no `/header` has been received. Rejected with 409 if no `/config` has been received.

#### POST /close

End the current scan. Empty body.

Closes both scanner and recon HDF5 archives. Forwards to recon. Clears state for the next scan.

---

### Recon-facing endpoint

#### POST /image

Receive a reconstructed image from the reconstruction service. Body: ISMRMRD image wire format (198-byte ImageHeader + uint64 attribute_string_len + attribute string + pixel data).

Marshal archives to `from_reconstruction/` HDF5 and writes a standalone file (`latest_image.bin`) for live viewing via `GET /image/latest`.

---

### Query and control endpoints

#### GET /health

Health check. Returns `{"status": "ok"}`.

#### GET /image/latest

Returns path to the latest reconstructed image file:
```json
{"path": "/session-data/from_reconstruction/latest_image.bin", "error": false}
```

When reconstruction has failed:
```json
{"path": "/session-data/from_reconstruction/latest_error.png", "error": true}
```

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

List archived scanner HDF5 files:
```json
[{"path": "from_scanner/scan_2026...h5", "size": 1234567, "modified": "2026-04-10T..."}]
```

#### GET /dump/recon

List archived reconstruction HDF5 files (same format as `/dump/scanner`).

---

### Startup flags

| Flag | Default | Description |
|------|---------|-------------|
| `--http host:port` | `0.0.0.0:8080` | HTTP listen address (query/control endpoints) |
| `--mrd-port N` | `9100` | MRD TCP listen port (scanner connections) |
| `--dump-dir path` | `./data` | Root for `from_scanner/` and `from_reconstruction/` |
| `--recon-host host` | (none) | Recon service hostname for MRD TCP forwarding |
| `--recon-port N` | (none) | Recon service port. Both `--recon-host` and `--recon-port` required. If omitted, archival-only mode. |
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
MRI Marshal (archives to from_scanner/*.h5)
    │
    │ MRD TCP (to recon-host:recon-port)
    │ same messages forwarded
    v
Reconstruction Service (MRD TCP server)
    │
    │ MRD TCP IMAGE(1022) returned on same connection
    v
MRI Marshal (archives to from_reconstruction/*.h5, writes latest_image.bin)
    │                              │
    │ MRD TCP IMAGE(1022)          │ HTTP GET /image/latest -> {"path": "..."}
    │ pushed back to scanner       │
    v                              v
Scanner                        Viz Client (opens file, renders with OpenCV)
```

Scanner data transport is MRD TCP. Query/control is HTTP. Archived HDF5 files are readable after `/close`. The standalone file (`latest_image.bin`) provides live view during the scan.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full system diagram and wire protocol reference.
