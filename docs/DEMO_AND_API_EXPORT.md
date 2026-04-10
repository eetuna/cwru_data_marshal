# CWRU Data Marshal - Demo & API Export Guide

## What the Demo Does

The demo runs a complete simulated MRI-guided robot surgery system with:

| Component | Container | Port | Purpose |
|-----------|-----------|------|---------|
| MRI Marshal | cwru-mri-marshal | 8080 (HTTP) | Archives scanner data, forwards to recon, serves image paths |
| Robot Marshal | cwru-robot-marshal | 8081 | Robot state blackboard for coordination |
| K-Space Streamer | cwru-kspace-streamer | - | Generates synthetic k-space, POSTs /header+/config+/frame+/close |
| Image Streamer | cwru-image-streamer | - | Generates synthetic images, POSTs /header+/config+/frame+/close |
| Mock Recon | cwru-mock-recon | 9002 | Reconstruction service, POSTs /image back to marshal |
| ECG Client | cwru-ecg-client | - | Sends ISMRMRD waveforms via POST /frame |
| Pose Client | cwru-pose-client | - | Sends pose updates via POST /pose |
| Viz Client | cwru-viz-client | - | Polls GET /image/latest, renders with OpenCV |
| Robot Clients | cwru-robot-clients | - | Read/write robot state channels |

---

## MRI Marshal API (Port 8080)

### Health Check
```bash
curl http://localhost:8080/health
# {"status":"ok"}
```

### Get Latest Image Path
```bash
curl http://localhost:8080/image/latest
# {"path":"/session-data/from_reconstruction/latest_image.bin","error":false}
```

### Submit Pose
```bash
curl -X POST http://localhost:8080/pose \
  -H "Content-Type: application/json" \
  -d '{"position":[12.5,8.3,-4.2],"orientation":[0,0,0.707,0.707]}'
```

### Get Current Pose
```bash
curl http://localhost:8080/pose
```

### Get/Set Slice Transform
```bash
# Read (atomically zeros after read)
curl http://localhost:8080/transform

# Write
curl -X PUT http://localhost:8080/transform \
  -H "Content-Type: application/json" \
  -d '{"through_plane_mm":1.0,"readout_mm":0,"phase_mm":0,"rotation_rad":0}'
```

### List Archived Files
```bash
curl http://localhost:8080/dump/scanner
curl http://localhost:8080/dump/recon
```

---

## Robot Marshal API (Port 8081)

### List Available State Files
```bash
curl http://localhost:8081/
```

### Read Latest State
```bash
curl http://localhost:8081/read/tip_position_orientation
curl "http://localhost:8081/read/controller_state?last=10"
```

### Write State
```bash
curl -X POST http://localhost:8081/write/user_input \
  -H "Content-Type: application/json" \
  -d '{"sent_at":1705315845123,"client_id":"my_client","values":[1,2,3]}'
```

---

## Python Client Examples

### Submit Pose
```python
import requests

resp = requests.post('http://localhost:8080/pose', json={
    "position": [12.5, 8.3, -4.2],
    "orientation": [0, 0, 0.707, 0.707]
})
print(resp.json())
```

### Get Latest Image Path
```python
import requests

resp = requests.get('http://localhost:8080/image/latest')
data = resp.json()
print(f"Path: {data['path']}, Error: {data['error']}")
```

### Read Robot State
```python
import requests

resp = requests.get('http://localhost:8081/read/tip_position_orientation')
print(resp.json())
```

---

## Data Files (session-data/)

After running the demo, you will find:

| Directory | Contents |
|-----------|----------|
| `from_scanner/*.h5` | Archived scanner data (ISMRMRD HDF5, readable after /close) |
| `from_reconstruction/*.h5` | Archived recon data (ISMRMRD HDF5, readable after /close) |
| `from_reconstruction/latest_image.bin` | Latest reconstructed image (raw ISMRMRD wire bytes, live view) |

---

## Export to USB / Transfer

### Export Docker Images
```bash
# Full export with all 9 images + docs + scripts
./scripts/export_usb.sh /media/usb/cwru_marshal_deploy
```

### Copy Session Data
```bash
cp -r session-data/ /media/usb/cwru_session_$(date +%Y%m%d)/
```

---

## External Client Requirements

### Minimum Requirements
- HTTP client library (requests, curl, fetch, etc.)
- JSON parser
- Network access to marshal port (8080)

### Dependencies by Language

**Python:**
```
pip install requests
```

**C++:**
```
libcurl (HTTP)
nlohmann/json (JSON)
ismrmrd (for scanner clients)
```

---

## Quick Start: Write Your Own Client

1. **Check health:**
   ```bash
   curl http://localhost:8080/health
   ```

2. **Send pose data:**
   ```bash
   curl -X POST http://localhost:8080/pose \
     -H "Content-Type: application/json" \
     -d '{"position":[0,0,0],"orientation":[0,0,0,1]}'
   ```

3. **Read data:**
   ```bash
   curl http://localhost:8080/pose
   curl http://localhost:8080/image/latest
   curl http://localhost:8080/dump/scanner
   ```
