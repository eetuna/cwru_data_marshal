# CWRU Data Marshal - Complete API Reference

Complete reference for connecting external clients to the CWRU Data Marshal system.

---

## MRI Marshal API

**Base URL:** `http://localhost:8080`

### Scanner-facing endpoints

These endpoints receive data from the scanner (or scanner mock) and archive it. If `--recon-url` is configured, data is also forwarded to the reconstruction service.

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
| `--http host:port` | `0.0.0.0:8080` | HTTP listen address |
| `--dump-dir path` | `./data` | Root for `from_scanner/` and `from_reconstruction/` |
| `--recon-url url` | (none) | Reconstruction service URL. If omitted, archival-only mode. |
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
    |
    | POST /header + /config + /frame (repeated) + /close
    v
MRI Marshal (archives to from_scanner/*.h5)
    |
    | forwards /header + /config + /frame + /close to recon
    v
Reconstruction Service
    |
    | POST /image (reconstructed image)
    v
MRI Marshal (archives to from_reconstruction/*.h5, writes latest_image.bin)
    |
    | GET /image/latest -> {"path": "...latest_image.bin"}
    v
Viz Client (opens file, decodes ISMRMRD image, renders with OpenCV)
```

Archived HDF5 files (`from_scanner/`, `from_reconstruction/`) are readable after `/close`. The standalone file (`latest_image.bin`) provides live view during the scan.
