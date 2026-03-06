# MRI Data Marshal - Client API Reference

**Base URL:** `http://<host>:8080`

---

## Writing Data (POST)

### Send MRI Frame (SWMR Mode)
```bash
curl -X POST http://localhost:8080/v1/mrd/frame \
  -H "X-MRD-Stream: acquisition_001" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @frame.bin
```
```json
{
  "path": "/data/mrd/acquisition_001.h5",
  "stream": "acquisition_001",
  "frame_index": 42,
  "flushed": true,
  "ts": "2024-01-15T10:30:45.123Z",
  "t_ms": 1705315845123,
  "dims": [192, 192, 15],
  "channels": 1,
  "datatype": "float",
  "size_bytes": 2211840
}
```

### Ingest MRD File (Batch Mode)
```bash
curl -X POST http://localhost:8080/v1/mrd/ingest \
  -H "Content-Type: application/octet-stream" \
  --data-binary @file.mrd
```
```json
{
  "path": "/data/mrd/2024-01-15T10:30:45.123Z_000042.mrd",
  "ts": "2024-01-15T10:30:45.123Z",
  "t_ms": 1705315845123,
  "size_bytes": 2200000,
  "type": "mrd",
  "seq": 42,
  "source": "http"
}
```

### Update Pose
```bash
curl -X POST http://localhost:8080/v1/pose/update \
  -H "Content-Type: application/json" \
  -d '{
    "p": [12.5, 8.3, -4.2],
    "R": [1,0,0, 0,1,0, 0,0,1],
    "frame": "scanner",
    "source": "fk_tracker"
  }'
```
```json
{
  "pose": {
    "p": [12.5, 8.3, -4.2],
    "R": [1,0,0, 0,1,0, 0,0,1],
    "frame": "scanner"
  },
  "ts": "2024-01-15T10:30:45.123Z",
  "t_ms": 1705315845123
}
```

### Send Biosignal Data
```bash
curl -X POST http://localhost:8080/v1/bio/signal \
  -H "Content-Type: application/json" \
  -d '{
    "source": "ecg",
    "data": [0.1, 0.15, 0.2, 0.18, 0.12],
    "rate_hz": 250
  }'
```

---

## Reading MRI Frame Data

### Get Latest Frame Metadata
```bash
curl http://localhost:8080/v1/mrd/latest
```
```json
{
  "ts": "2024-01-15T10:30:45.123Z",
  "t_ms": 1705315845123,
  "path": "/data/mrd/latest.mrd",
  "size_bytes": 2200000,
  "type": "mrd",
  "seq": 42,
  "source": "http"
}
```

### Get Last N Frames
```bash
curl "http://localhost:8080/v1/mrd/since?last=5"
```
```json
[
  {"ts": "2024-01-15T10:30:38.000Z", "path": "...", "size_bytes": 2200000, ...},
  {"ts": "2024-01-15T10:30:39.000Z", "path": "...", "size_bytes": 2200000, ...},
  {"ts": "2024-01-15T10:30:40.000Z", "path": "...", "size_bytes": 2200000, ...}
]
```

### Get Frames After Timestamp
```bash
curl "http://localhost:8080/v1/mrd/since?ts=2024-01-15T10:30:00.000Z&limit=10"
```

---

## Reading Pose Data

### Get Current Pose
```bash
curl http://localhost:8080/v1/pose/current
```
```json
{
  "pose": {
    "p": [12.5, 8.3, -4.2],
    "R": [1,0,0, 0,1,0, 0,0,1],
    "frame": "scanner"
  },
  "source": "fk_tracker",
  "ts": "2024-01-15T10:30:45.123Z"
}
```

---

## Reading Biosignal Data

### Get Latest Biosignal
```bash
curl http://localhost:8080/v1/bio/latest
```
```json
{
  "ts": "2024-01-15T10:30:45.123Z",
  "source": "ecg",
  "data": [0.1, 0.15, 0.2, 0.18, ...],
  "rate_hz": 250
}
```

---

## System Endpoints

### Health Check
```bash
curl http://localhost:8080/health
```
```json
{"status": "ok"}
```

### Server Configuration
```bash
curl http://localhost:8080/v1/config
```

---

## Client Code Examples

### Python (Reading)
```python
import requests

BASE = "http://localhost:8080"

# Get latest frame
resp = requests.get(f"{BASE}/v1/mrd/latest")
frame = resp.json()
print(f"Latest frame: {frame['path']} at {frame['ts']}")

# Get last 5 frames
resp = requests.get(f"{BASE}/v1/mrd/since", params={"last": 5})
frames = resp.json()
for f in frames:
    print(f"  {f['ts']}: {f['path']}")

# Get current pose
resp = requests.get(f"{BASE}/v1/pose/current")
pose = resp.json()
print(f"Position: {pose['pose']['p']}")

# Get latest biosignal
resp = requests.get(f"{BASE}/v1/bio/latest")
bio = resp.json()
print(f"ECG samples: {len(bio.get('data', []))}")
```

### Python (Writing)
```python
import requests
import struct

BASE = "http://localhost:8080"

# Send MRI frame (SWMR mode) - requires ISMRMRD header + pixel data
def send_mrd_frame(stream_id, width, height, slices, pixel_data):
    # Build minimal ISMRMRD ImageHeader (198 bytes)
    header = bytearray(198)
    struct.pack_into('<H', header, 0, 1)  # version
    struct.pack_into('<H', header, 4, 7)  # data_type (float)
    struct.pack_into('<HHH', header, 100, width, height, slices)  # matrix_size
    struct.pack_into('<H', header, 106, 1)  # channels

    resp = requests.post(f"{BASE}/v1/mrd/frame",
        headers={
            "Content-Type": "application/octet-stream",
            "X-MRD-Stream": stream_id
        },
        data=bytes(header) + pixel_data)
    return resp.json()

# Update pose
resp = requests.post(f"{BASE}/v1/pose/update", json={
    "p": [12.5, 8.3, -4.2],
    "R": [1,0,0, 0,1,0, 0,0,1],
    "frame": "scanner",
    "source": "fk_tracker"
})
print(f"Pose updated: {resp.json()}")

# Send biosignal (server generates ts)
resp = requests.post(f"{BASE}/v1/bio/signal", json={
    "source": "ecg",
    "data": [0.1, 0.15, 0.2, 0.18, 0.12],
    "rate_hz": 250
})
print(f"Biosignal sent: {resp.status_code}")

# Ingest MRD file (batch mode)
with open("file.mrd", "rb") as f:
    resp = requests.post(f"{BASE}/v1/mrd/ingest",
        headers={"Content-Type": "application/octet-stream"},
        data=f.read())
print(f"Ingested: {resp.json()['path']}")
```

### C++ (Reading) - using httplib
```cpp
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

int main() {
    httplib::Client cli("127.0.0.1", 8080);

    // Get latest frame
    auto res = cli.Get("/v1/mrd/latest");
    if (res && res->status == 200) {
        json frame = json::parse(res->body);
        std::cout << "Latest: " << frame["path"] << " at " << frame["ts"] << "\n";
    }

    // Get last 5 frames
    res = cli.Get("/v1/mrd/since?last=5");
    if (res && res->status == 200) {
        json frames = json::parse(res->body);
        for (auto& f : frames) {
            std::cout << "  " << f["ts"] << ": " << f["path"] << "\n";
        }
    }

    // Get current pose
    res = cli.Get("/v1/pose/current");
    if (res && res->status == 200) {
        json pose = json::parse(res->body);
        auto p = pose["pose"]["p"];
        std::cout << "Position: [" << p[0] << ", " << p[1] << ", " << p[2] << "]\n";
    }

    // Get latest biosignal
    res = cli.Get("/v1/bio/latest");
    if (res && res->status == 200) {
        json bio = json::parse(res->body);
        std::cout << "Biosignal source: " << bio["source"] << "\n";
    }

    return 0;
}
```

### C++ (Writing) - using httplib
```cpp
#include "httplib.h"
#include "json.hpp"
#include <fstream>

using json = nlohmann::json;

int main() {
    httplib::Client cli("127.0.0.1", 8080);

    // Update pose
    json pose_data = {
        {"p", {12.5, 8.3, -4.2}},
        {"R", {1,0,0, 0,1,0, 0,0,1}},
        {"frame", "scanner"},
        {"source", "fk_tracker"}
    };
    auto res = cli.Post("/v1/pose/update", pose_data.dump(), "application/json");
    if (res && res->status == 200) {
        std::cout << "Pose updated: " << res->body << "\n";
    }

    // Send biosignal
    json bio_data = {
        {"source", "ecg"},
        {"data", {0.1, 0.15, 0.2, 0.18, 0.12}},
        {"rate_hz", 250}
    };
    res = cli.Post("/v1/bio/signal", bio_data.dump(), "application/json");
    if (res && res->status == 200) {
        std::cout << "Biosignal sent\n";
    }

    // Ingest MRD file
    std::ifstream file("file.mrd", std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
    res = cli.Post("/v1/mrd/ingest", data, "application/octet-stream");
    if (res && res->status == 201) {
        json resp = json::parse(res->body);
        std::cout << "Ingested: " << resp["path"] << "\n";
    }

    return 0;
}
```

---

## Polling Pattern

For real-time updates, poll with timestamp tracking:

```python
import time
import requests

BASE = "http://localhost:8080"
last_ts = ""

while True:
    if last_ts:
        resp = requests.get(f"{BASE}/v1/mrd/since", params={"ts": last_ts})
    else:
        resp = requests.get(f"{BASE}/v1/mrd/since", params={"last": 1})

    frames = resp.json()
    for frame in frames:
        print(f"New frame: {frame['path']}")
        last_ts = frame['ts']

    time.sleep(0.05)  # 50ms polling interval
```

---

## Response Fields Reference

| Field | Type | Description |
|-------|------|-------------|
| `ts` | string | ISO8601 timestamp |
| `t_ms` | number | Unix epoch milliseconds |
| `path` | string | File path on server |
| `size_bytes` | number | Data size in bytes |
| `seq` | number | Sequence number |
| `source` | string | Data source (http, ws, etc.) |
| `type` | string | Data type (mrd, etc.) |
