# MRI Data Marshal - Client API Reference

**Base URL:** `http://<host>:8080`

---

## Scanner-Facing Endpoints

These endpoints are called by the scanner (or scanner mock) to send data to the marshal.
Data is archived to `from_scanner/` and forwarded to the reconstruction service.

### POST /header

Start a new scan. Body is the ISMRMRD XML header as raw bytes.

```bash
curl -X POST http://localhost:8080/header \
  -H "Content-Type: application/octet-stream" \
  --data-binary @header.xml
```

**Response:** `200 OK` on success, `400` if XML is malformed.

### POST /config

Send the reconstruction config name (e.g. `simplefft`). Required after `/header`, before `/frame`.

```bash
curl -X POST http://localhost:8080/config \
  -H "Content-Type: application/octet-stream" \
  -d "simplefft"
```

**Response:** `200 OK` on success, `409` if no `/header` received yet.

### POST /frame

Send one ISMRMRD message (acquisition, image, or waveform). The marshal detects the type automatically from the wire format and archives it with the appropriate typed append (appendAcquisition, appendImage, or appendWaveform). The raw body is forwarded unmodified to the reconstruction service.

```bash
curl -X POST http://localhost:8080/frame \
  -H "Content-Type: application/octet-stream" \
  --data-binary @acquisition.bin
```

**Response:** `202 Accepted` (always, even if recon is down). `409` if no `/header` or `/config` received yet.

### POST /close

End the current scan. Finalizes all HDF5 files and clears state. Empty body.

```bash
curl -X POST http://localhost:8080/close
```

**Response:** `200 OK`.

---

## Recon-Facing Endpoints

These endpoints are called by the reconstruction service to send results back to the marshal.
Data is archived to `from_reconstruction/`.

### POST /image

Send a reconstructed image. Body is the ISMRMRD image wire format:
198-byte ImageHeader + 8-byte little-endian uint64 attribute_string_len + attribute string + pixel data.

```bash
curl -X POST http://localhost:8080/image \
  -H "Content-Type: application/octet-stream" \
  --data-binary @recon_image.bin
```

**Response:** `200 OK`. The image is archived to the recon HDF5 file and written as a standalone binary file for live visualization.

---

## Query and Control Endpoints

### GET /image/latest

Returns the path to the latest reconstructed image file for visualization.

```bash
curl http://localhost:8080/image/latest
```

**Normal response:**
```json
{"path": "/data/from_reconstruction/latest_image.bin", "error": false}
```

**When reconstruction has failed:**
```json
{"path": "/data/from_reconstruction/latest_error.png", "error": true}
```

The viz client opens the file directly. When `error` is false, the file contains raw ISMRMRD image wire bytes. When `error` is true, the file is a PNG.

### GET /transform

Returns the current slice-repositioning delta and atomically zeros it (consume-on-read).

```bash
curl http://localhost:8080/transform
```

```json
{
  "through_plane_mm": 0.0,
  "readout_mm": 0.0,
  "phase_mm": 0.0,
  "rotation_rad": 0.0
}
```

### PUT /transform

Write a new slice-repositioning delta. Overwrites any unconsumed value.

```bash
curl -X PUT http://localhost:8080/transform \
  -H "Content-Type: application/json" \
  -d '{"through_plane_mm": 2.5, "readout_mm": 0.0, "phase_mm": -1.0, "rotation_rad": 0.01}'
```

**Response:** `200 OK`.

### POST /pose

Update the cached robot pose.

```bash
curl -X POST http://localhost:8080/pose \
  -H "Content-Type: application/json" \
  -d '{"position": [12.5, 8.3, -4.2], "orientation": [1, 0, 0, 0]}'
```

**Response:** `200 OK`.

### GET /pose

Returns the latest cached pose.

```bash
curl http://localhost:8080/pose
```

```json
{"position": [12.5, 8.3, -4.2], "orientation": [1, 0, 0, 0]}
```

### GET /dump/scanner

List archived HDF5 files from the scanner.

```bash
curl http://localhost:8080/dump/scanner
```

```json
[
  {"path": "/data/from_scanner/scan_1712764800.h5", "size": 52428800, "modified": "2026-04-10T12:00:00Z"}
]
```

### GET /dump/recon

List archived HDF5 files from the reconstruction service.

```bash
curl http://localhost:8080/dump/recon
```

```json
[
  {"path": "/data/from_reconstruction/scan_1712764800.h5", "size": 10485760, "modified": "2026-04-10T12:00:05Z"}
```

### GET /health

Health check.

```bash
curl http://localhost:8080/health
```

```json
{"status": "ok"}
```

---

## Scan Lifecycle

A complete scan follows this sequence:

1. `POST /header` -- ISMRMRD XML header (starts scan, opens `from_scanner/*.h5`)
2. `POST /config` -- recon config name (e.g. `simplefft`)
3. `POST /frame` (repeated) -- acquisitions, images, waveforms
4. `POST /close` -- ends scan, closes all HDF5 files

The recon service receives forwarded `/header`, `/config`, `/frame`, and `/close` calls. It posts reconstructed images back via `POST /image`.

---

## Client Code Examples

### Python -- Sending a Scan

```python
import requests

MARSHAL = "http://localhost:8080"

# 1. Send header
with open("header.xml", "rb") as f:
    requests.post(f"{MARSHAL}/header", data=f.read())

# 2. Send config
requests.post(f"{MARSHAL}/config", data=b"simplefft")

# 3. Send frames (acquisitions)
for acq_bytes in acquisition_list:
    requests.post(f"{MARSHAL}/frame", data=acq_bytes)

# 4. Close scan
requests.post(f"{MARSHAL}/close")
```

### Python -- Polling for Images

```python
import requests
import time

MARSHAL = "http://localhost:8080"
last_path = None

while True:
    resp = requests.get(f"{MARSHAL}/image/latest").json()
    if resp["path"] != last_path:
        last_path = resp["path"]
        if resp["error"]:
            print(f"Reconstruction failed: {last_path}")
        else:
            with open(last_path, "rb") as f:
                image_bytes = f.read()
            # Decode ISMRMRD image wire format...
            print(f"New image: {last_path}")
    time.sleep(0.1)
```

### Python -- Pose Updates

```python
import requests

MARSHAL = "http://localhost:8080"

# Write pose
requests.post(f"{MARSHAL}/pose", json={
    "position": [12.5, 8.3, -4.2],
    "orientation": [1, 0, 0, 0]
})

# Read pose
pose = requests.get(f"{MARSHAL}/pose").json()
print(f"Position: {pose['position']}")
```

---

## Error Codes

| Code | Meaning |
|------|---------|
| 200  | Success |
| 202  | Accepted (frame queued, recon forwarding is async) |
| 400  | Bad request (malformed XML header, invalid JSON) |
| 409  | Conflict (frame sent before header/config, or close without scan) |
| 500  | Internal server error |
