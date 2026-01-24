# CWRU Data Marshal - Demo & API Export Guide

## What the Demo Does

The demo runs a complete simulated MRI-guided robot surgery system with:

| Component | Container | Port | Purpose |
|-----------|-----------|------|---------|
| MRI Marshal | cwru-mri-marshal | 8080 (HTTP), 8090 (WS) | Receives/stores MRI frames, ECG, poses |
| Robot Marshal | cwru-robot-marshal | 8081 | Robot state blackboard for coordination |
| Image Streamer | cwru-image-streamer | - | Generates synthetic MRI frames (10 fps) |
| ECG Client | cwru-ecg-client | - | Sends synthetic ECG waveforms (1 Hz) |
| Pose Client | cwru-pose-client | - | Sends circular trajectory poses (1 Hz) |
| Viz Client | cwru-viz-client | - | OpenCV window showing MRI slices |
| Robot Clients | cwru-catheter-tracking, cwru-controller, cwru-planning, cwru-front-end, cwru-surface-tracking | - | Read/write robot state |

---

## MRI Marshal API (Port 8080)

### Health Check
```bash
curl http://localhost:8080/health
# {"status":"ok","data":{"uptime_s":123.45}}
```

### Stream MRI Frame (Binary)
```bash
# ISMRMRD header (340 bytes) + voxel data
curl -X POST http://localhost:8080/v1/mrd/frame \
  -H "Content-Type: application/octet-stream" \
  -H "X-MRD-Stream: my_scan" \
  --data-binary @frame.bin
```

### Get Latest Frame Metadata
```bash
curl http://localhost:8080/v1/mrd/latest
# {"path":"/session-data/mrd/demo_stream-128x128x5-g0000.mrd","frame_index":42,"ts":"2024-01-15T10:30:45.123Z"}
```

### Get Frames Since Timestamp
```bash
curl "http://localhost:8080/v1/mrd/since?ts=2024-01-15T10:30:00Z&limit=100"
```

### Send ECG/Biosignal Data
```bash
curl -X POST http://localhost:8080/v1/bio/signal \
  -H "Content-Type: application/json" \
  -d '{"source":"ecg","data":[0.1,0.15,0.2,0.18],"rate_hz":250}'
```

### Get Latest ECG
```bash
curl http://localhost:8080/v1/bio/latest
```

### Send Pose Update
```bash
curl -X POST http://localhost:8080/v1/pose/update \
  -H "Content-Type: application/json" \
  -d '{"p":[12.5,8.3,-4.2],"R":[1,0,0,0,1,0,0,0,1],"frame":"scanner","source":"tracker"}'
```

### Get Current Pose
```bash
curl http://localhost:8080/v1/pose/current
```

---

## WebSocket API (Port 8090)

Connect to `ws://localhost:8090/ws` for real-time notifications.

### Subscribe to Topics
```json
{"subscribe": "mrd"}
{"subscribe": "bio"}
{"subscribe": "pose"}
```

### Notification Messages
```json
{
  "type": "mrd",
  "stream": "demo_stream",
  "frame_index": 42,
  "flushed": true,
  "ts": "2024-01-15T10:30:45.123Z",
  "t_ms": 1705315845123
}
```

---

## Robot Marshal API (Port 8081)

### List Available State Files
```bash
curl http://localhost:8081/files
```

### Read Latest State
```bash
curl http://localhost:8081/read/robot_status
curl http://localhost:8081/read/catheter_position
curl "http://localhost:8081/read/controller_state?last=10"  # last 10 entries
```

### Write State
```bash
curl -X POST http://localhost:8081/write/robot_commands \
  -H "Content-Type: application/json" \
  -d '{"sent_at":1705315845123,"client_id":"my_client","values":[1,2,3]}'
```

---

## Python Client Examples

### Send ECG Data
```python
import requests

payload = {
    "source": "ecg",
    "data": [0.1, 0.15, 0.2, 0.18, 0.12],
    "rate_hz": 250
}
resp = requests.post('http://localhost:8080/v1/bio/signal', json=payload)
print(resp.json())
```

### Send Pose Update
```python
import requests

payload = {
    "p": [12.5, 8.3, -4.2],          # Position [x, y, z]
    "R": [1,0,0, 0,1,0, 0,0,1],      # 3x3 rotation matrix (row-major)
    "frame": "scanner",
    "source": "fk_tracker"
}
resp = requests.post('http://localhost:8080/v1/pose/update', json=payload)
print(resp.json())
```

### WebSocket Listener
```python
import asyncio
import websockets
import json

async def listen():
    async with websockets.connect('ws://localhost:8090/ws') as ws:
        await ws.send(json.dumps({"subscribe": "mrd"}))
        while True:
            msg = await ws.recv()
            data = json.loads(msg)
            print(f"Frame {data.get('frame_index')}: {data.get('path')}")

asyncio.run(listen())
```

### Read Robot State
```python
import requests

resp = requests.get('http://localhost:8081/read/catheter_position')
data = resp.json()
print(data)
```

---

## C++ Client Example (ISMRMRD Frame)

```cpp
#include <ismrmrd/ismrmrd.h>
#include <curl/curl.h>

// Create frame
ISMRMRD::Image<float> img(128, 128, 5, 1);
ISMRMRD::ImageHeader& head = img.getHead();
head.matrix_size[0] = 128;
head.matrix_size[1] = 128;
head.matrix_size[2] = 5;

// Fill voxel data
float* data = img.getDataPtr();
for (size_t i = 0; i < 128*128*5; i++) {
    data[i] = /* your value */;
}

// Pack binary: header + voxels
std::vector<uint8_t> body(sizeof(ISMRMRD::ImageHeader) + 128*128*5*sizeof(float));
memcpy(body.data(), &head, sizeof(ISMRMRD::ImageHeader));
memcpy(body.data() + sizeof(ISMRMRD::ImageHeader), data, 128*128*5*sizeof(float));

// POST to marshal
CURL* curl = curl_easy_init();
curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:8080/v1/mrd/frame");
curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());

struct curl_slist* headers = NULL;
headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
headers = curl_slist_append(headers, "X-MRD-Stream: my_scan");
curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

curl_easy_perform(curl);
curl_easy_cleanup(curl);
```

---

## Data Files (session-data/)

After running the demo, you'll find:

| File | Contents |
|------|----------|
| `mrd/*.mrd` | HDF5 MRI frame data (ISMRMRD format) |
| `mrd/index.jsonl` | Frame metadata index (JSON lines) |
| `mrd/latest.json` | Latest frame pointer |
| `mrd/bio.jsonl` | ECG/biosignal data (JSON lines) |
| `mrd/poses.jsonl` | Pose updates (JSON lines) |

---

## Export to USB / Transfer

### Copy Session Data
```bash
# Copy all session data
cp -r session-data/ /media/usb/cwru_session_$(date +%Y%m%d)/

# Or just the MRD files
cp session-data/mrd/*.mrd /media/usb/
```

### Export Demo Configuration
```bash
# Copy config files needed to run elsewhere
cp .env.demo /media/usb/
cp docker-compose.demo.yml /media/usb/
cp scripts/demo-docker.sh /media/usb/
```

### Export Docker Images
```bash
# Save images for offline transfer
docker save cwru/mri-marshal:latest | gzip > /media/usb/mri-marshal.tar.gz
docker save cwru/robot-marshal:latest | gzip > /media/usb/robot-marshal.tar.gz
docker save cwru/image-streamer:latest | gzip > /media/usb/image-streamer.tar.gz
docker save cwru/ecg-client:latest | gzip > /media/usb/ecg-client.tar.gz
docker save cwru/pose-client:latest | gzip > /media/usb/pose-client.tar.gz
docker save cwru/viz-client:latest | gzip > /media/usb/viz-client.tar.gz
docker save cwru/robot-clients:latest | gzip > /media/usb/robot-clients.tar.gz

# Load on another machine
docker load < /media/usb/mri-marshal.tar.gz
# ... etc
```

### Export All Images at Once
```bash
#!/bin/bash
# save-all-images.sh
IMAGES="cwru/mri-marshal cwru/robot-marshal cwru/image-streamer cwru/ecg-client cwru/pose-client cwru/viz-client cwru/robot-clients"
for img in $IMAGES; do
    name=$(echo $img | tr '/' '-')
    echo "Saving $img..."
    docker save $img:latest | gzip > "${name}.tar.gz"
done
```

---

## External Client Requirements

To write your own client that interacts with the marshals:

### Minimum Requirements
- HTTP client library (requests, curl, fetch, etc.)
- JSON parser
- Network access to marshal ports (8080, 8081, 8090)

### For MRI Frame Streaming
- ISMRMRD library (or manual binary packing)
- Understanding of ISMRMRD ImageHeader struct (340 bytes)

### For Real-time Notifications
- WebSocket client library

### Dependencies by Language

**Python:**
```
pip install requests websockets
pip install ismrmrd  # optional, for MRI frames
```

**C++:**
```
libcurl, boost::beast (HTTP)
nlohmann/json (JSON)
ismrmrd (MRI frames)
```

**JavaScript/Node:**
```
npm install axios ws
```

**MATLAB:**
```matlab
% Built-in: webread, webwrite, websocket (R2021a+)
```

---

## Quick Start: Write Your Own Client

1. **Check health:**
   ```bash
   curl http://localhost:8080/health
   ```

2. **Send data:**
   ```bash
   # ECG
   curl -X POST http://localhost:8080/v1/bio/signal \
     -H "Content-Type: application/json" \
     -d '{"source":"ecg","data":[0.5],"rate_hz":1}'

   # Pose
   curl -X POST http://localhost:8080/v1/pose/update \
     -H "Content-Type: application/json" \
     -d '{"p":[0,0,0],"R":[1,0,0,0,1,0,0,0,1],"frame":"world","source":"test"}'
   ```

3. **Read data:**
   ```bash
   curl http://localhost:8080/v1/bio/latest
   curl http://localhost:8080/v1/pose/current
   curl http://localhost:8080/v1/mrd/latest
   ```

4. **Subscribe to updates:**
   ```bash
   websocat ws://localhost:8090/ws
   # then type: {"subscribe":"mrd"}
   ```
