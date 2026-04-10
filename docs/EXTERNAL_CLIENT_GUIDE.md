# External Client Integration Guide

Guide for external clients (Python, C++, or other languages) to connect to the CWRU Data Marshals after loading Docker images from USB.

---

## Quick Start

After receiving the USB package and loading Docker images:

```bash
# 1. Load Docker images
docker load -i cwru_deploy/images/mri-marshal.tar
docker load -i cwru_deploy/images/robot-marshal.tar

# 2. Start marshals (standalone, no demo clients)
docker compose -f cwru_deploy/docker-compose.yml up -d

# 3. Verify services are running
curl http://localhost:8080/health       # MRI Marshal
curl http://localhost:8081/             # Robot Marshal

# 4. Connect your client to:
#    - MRI Marshal:   http://localhost:8080
#    - Robot Marshal: http://localhost:8081
```

---

## Network Configuration

### Default Port Mapping (Bridge Mode)

The Docker images expose ports to localhost by default:

| Service | Container Port | Host Port | Protocol |
|---------|---------------|-----------|----------|
| MRI Marshal | 8080 | 8080 | HTTP REST API |
| MRI Marshal | 8090 | 8090 | WebSocket |
| Robot Marshal | 8081 | 8081 | HTTP REST API |

### Connection URLs by Client Location

| Client Location | MRI Marshal | Robot Marshal |
|-----------------|-------------|---------------|
| Same machine (localhost) | `http://localhost:8080` | `http://localhost:8081` |
| Same network (IP) | `http://<host-ip>:8080` | `http://<host-ip>:8081` |
| Inside Docker container | `http://mri-marshal:8080` | `http://robot-marshal:8081` |

### Exposing to Network (Remote Clients)

To allow remote clients on the same network, the default `docker-compose.yml` already binds to `0.0.0.0`. Remote clients connect using the host machine's IP:

```bash
# Find host IP
hostname -I | awk '{print $1}'

# Remote client connects to:
# http://<host-ip>:8080   (MRI Marshal)
# http://<host-ip>:8081   (Robot Marshal)
```

---

## Running Marshals Standalone

To run only the marshals without demo clients:

### Option 1: Base docker-compose.yml (Recommended)

```bash
cd cwru_deploy
docker compose up -d
```

### Option 2: Specific Services from Full Compose

```bash
docker compose -f docker-compose.full.yml up -d mri-marshal robot-marshal
```

### Option 3: Run Individual Containers

```bash
# MRI Marshal
docker run -d --name mri-marshal \
  -p 8080:8080 -p 8090:8090 \
  -e HDF5_USE_FILE_LOCKING=FALSE \
  -v $(pwd)/data/mri_data:/data/mri_data \
  cwru/mri-marshal:latest

# Robot Marshal
docker run -d --name robot-marshal \
  -p 8081:8081 \
  cwru/robot-marshal:latest
```

---

## MRI Marshal API Reference

**Base URL:** `http://localhost:8080`

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/health` | GET | Health check |
| `/frame` | POST | Submit ECG/physiological data |
| `/health` | GET | Get latest biosignal |
| `/pose` | POST | Submit position/orientation data |
| `/pose` | GET | Get current pose |
| `/frame` | POST | Stream MRI frame (HDF5 mode) |
| `/frame` | POST | Batch ingest MRD file |
| `/image/latest` | GET | Get latest MRI frame metadata |
| `/dump/scanner` | GET | Get frames after timestamp |
| `/health` | GET | Server configuration |

### WebSocket (Real-time Streaming)

- **URL:** `ws://localhost:8090`
- Used for real-time data streaming (image frames, etc.)

---

## Robot Marshal API Reference

**Base URL:** `http://localhost:8081`

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Status check |
| `/read/{filename}` | GET | Read data from file |
| `/write/{filename}` | POST | Write data to file |

### Available Files

The robot marshal serves these data files (defined in `files.json`):

- `localization_data` - Robot localization/position
- `mult_data` - Multi-joint data
- `forward_kinematics` - FK computation results
- `catheter_pose` - Catheter position
- `robot_status` - Overall robot status
- `command` - Command input

---

## Data Formats

### ECG/Biosignal Data

```json
{
  "source": "ecg_monitor",
  "data": [0.15, 0.22, 0.18, 0.31, ...],
  "rate_hz": 100.0
}
```

| Field | Type | Description |
|-------|------|-------------|
| `source` | string | Identifier for the data source |
| `data` | array[float] | Array of signal samples |
| `rate_hz` | float | Sampling rate in Hz |

### Pose Data

```json
{
  "p": [12.5, 8.3, -4.2],
  "R": [1, 0, 0, 0, 1, 0, 0, 0, 1],
  "frame": "scanner",
  "source": "fk_tracker"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `p` | array[3] | Position [x, y, z] in mm |
| `R` | array[9] | 3x3 rotation matrix (row-major, flattened) |
| `frame` | string | Reference frame name |
| `source` | string | Data source identifier |

### Robot Marshal Data

```json
{
  "client_id": "catheter_tracking",
  "sent_at": 1705315845123456789,
  "values": [1.0, 2.0, 3.0, 4.5, 6.7]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `client_id` | string | Identifier for the sending client |
| `sent_at` | int64 | Timestamp in nanoseconds |
| `values` | array[float] | Arbitrary numeric values |

---

## Python Client Examples

### Minimal Example (No Dependencies)

Uses only Python standard library:

```python
#!/usr/bin/env python3
"""Minimal client using only standard library."""
import json
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError

MRI_MARSHAL = "http://localhost:8080"
ROBOT_MARSHAL = "http://localhost:8081"

def post_json(url, data):
    """POST JSON data to URL."""
    req = Request(
        url,
        data=json.dumps(data).encode('utf-8'),
        headers={'Content-Type': 'application/json'},
        method='POST'
    )
    try:
        with urlopen(req, timeout=5.0) as resp:
            return resp.status == 200, resp.read().decode()
    except (HTTPError, URLError) as e:
        return False, str(e)

def get_json(url):
    """GET JSON from URL."""
    try:
        with urlopen(url, timeout=5.0) as resp:
            return json.loads(resp.read().decode())
    except (HTTPError, URLError) as e:
        return {"error": str(e)}

# Send ECG data
ecg_data = {
    "source": "my_ecg_monitor",
    "data": [0.1, 0.2, 0.15, 0.25, 0.18],
    "rate_hz": 250.0
}
ok, resp = post_json(f"{MRI_MARSHAL}/frame", ecg_data)
print(f"ECG sent: {ok}")

# Send pose update
pose_data = {
    "p": [10.0, 20.0, 30.0],
    "R": [1, 0, 0, 0, 1, 0, 0, 0, 1],
    "frame": "scanner",
    "source": "my_tracker"
}
ok, resp = post_json(f"{MRI_MARSHAL}/pose", pose_data)
print(f"Pose sent: {ok}")

# Read robot data
robot_data = get_json(f"{ROBOT_MARSHAL}/read/localization_data")
print(f"Robot data: {robot_data}")

# Write robot data
robot_cmd = {
    "client_id": "my_client",
    "sent_at": 1705315845123456789,
    "values": [1.0, 2.0, 3.0]
}
ok, resp = post_json(f"{ROBOT_MARSHAL}/write/localization_data", robot_cmd)
print(f"Robot write: {ok}")
```

### With requests Library

```python
#!/usr/bin/env python3
"""Client using requests library (pip install requests)."""
import requests
import time

MRI_MARSHAL = "http://localhost:8080"
ROBOT_MARSHAL = "http://localhost:8081"

# Health checks
print("MRI Marshal:", requests.get(f"{MRI_MARSHAL}/health").json())
print("Robot Marshal:", requests.get(f"{ROBOT_MARSHAL}/").text[:50])

# Send ECG stream
for i in range(10):
    resp = requests.post(f"{MRI_MARSHAL}/frame", json={
        "source": "ecg_monitor",
        "data": [0.1 + i*0.01] * 100,  # 100 samples
        "rate_hz": 100.0
    })
    print(f"ECG {i+1}: {resp.status_code}")
    time.sleep(1.0)

# Send continuous pose updates
import math
t = 0
while True:
    x = 50 * math.cos(t)
    y = 50 * math.sin(t)

    resp = requests.post(f"{MRI_MARSHAL}/pose", json={
        "p": [x, y, 100.0],
        "R": [1, 0, 0, 0, 1, 0, 0, 0, 1],
        "frame": "scanner",
        "source": "my_tracker"
    })
    print(f"Pose: [{x:.1f}, {y:.1f}] - {resp.status_code}")

    t += 0.1
    time.sleep(0.1)
```

### Reading Latest Data

```python
#!/usr/bin/env python3
"""Read data from marshals."""
import requests

MRI = "http://localhost:8080"

# Get latest biosignal
bio = requests.get(f"{MRI}/health").json()
print(f"Latest ECG: {len(bio.get('data', []))} samples from {bio.get('source')}")

# Get current pose
pose = requests.get(f"{MRI}/pose").json()
print(f"Current pose: {pose.get('pose', {}).get('p')}")

# Get latest MRI frame
frame = requests.get(f"{MRI}/image/latest").json()
print(f"Latest frame: {frame.get('path')} at {frame.get('ts')}")

# Poll for new frames
import time
last_ts = ""
while True:
    params = {"ts": last_ts} if last_ts else {"last": 1}
    frames = requests.get(f"{MRI}/dump/scanner", params=params).json()
    for f in frames:
        print(f"New frame: {f['path']}")
        last_ts = f['ts']
    time.sleep(0.05)
```

---

## C++ Client Examples

### Using httplib.h (Header-Only)

Download [httplib.h](https://github.com/yhirose/cpp-httplib) and [json.hpp](https://github.com/nlohmann/json):

```cpp
// client.cpp
// Compile: g++ -std=c++17 client.cpp -o client -lpthread
#include "httplib.h"
#include "json.hpp"
#include <iostream>

using json = nlohmann::json;

int main() {
    // Connect to MRI Marshal
    httplib::Client mri("localhost", 8080);
    mri.set_connection_timeout(5);

    // Connect to Robot Marshal
    httplib::Client robot("localhost", 8081);
    robot.set_connection_timeout(5);

    // Health check
    auto res = mri.Get("/health");
    if (res && res->status == 200) {
        std::cout << "MRI Marshal: " << res->body << "\n";
    }

    // Send ECG data
    json ecg = {
        {"source", "cpp_ecg_client"},
        {"data", {0.1, 0.2, 0.15, 0.25, 0.18}},
        {"rate_hz", 250.0}
    };
    res = mri.Post("/frame", ecg.dump(), "application/json");
    if (res && res->status == 200) {
        std::cout << "ECG sent successfully\n";
    }

    // Send pose update
    json pose = {
        {"p", {10.0, 20.0, 30.0}},
        {"R", {1, 0, 0, 0, 1, 0, 0, 0, 1}},
        {"frame", "scanner"},
        {"source", "cpp_tracker"}
    };
    res = mri.Post("/pose", pose.dump(), "application/json");
    if (res && res->status == 200) {
        std::cout << "Pose sent: " << res->body << "\n";
    }

    // Read robot data
    res = robot.Get("/read/localization_data");
    if (res && res->status == 200) {
        json data = json::parse(res->body);
        std::cout << "Robot data: " << data.dump(2) << "\n";
    }

    // Write robot data
    json robot_data = {
        {"client_id", "cpp_client"},
        {"sent_at", 1705315845123456789},
        {"values", {1.0, 2.0, 3.0, 4.5}}
    };
    res = robot.Post("/write/localization_data", robot_data.dump(), "application/json");
    if (res && res->status == 200) {
        std::cout << "Robot write successful\n";
    }

    // Get latest MRI frame
    res = mri.Get("/image/latest");
    if (res && res->status == 200) {
        json frame = json::parse(res->body);
        std::cout << "Latest frame: " << frame["path"] << "\n";
    }

    return 0;
}
```

### Continuous Streaming Loop

```cpp
#include "httplib.h"
#include "json.hpp"
#include <chrono>
#include <thread>
#include <cmath>

using json = nlohmann::json;

int main() {
    httplib::Client cli("localhost", 8080);

    auto start = std::chrono::steady_clock::now();

    while (true) {
        auto now = std::chrono::steady_clock::now();
        double t = std::chrono::duration<double>(now - start).count();

        // Generate circular trajectory
        double x = 50.0 * std::cos(0.5 * t);
        double y = 50.0 * std::sin(0.5 * t);
        double z = 100.0;

        // Rotation matrix (identity for simplicity)
        json pose = {
            {"p", {x, y, z}},
            {"R", {1, 0, 0, 0, 1, 0, 0, 0, 1}},
            {"frame", "scanner"},
            {"source", "cpp_trajectory"}
        };

        auto res = cli.Post("/pose", pose.dump(), "application/json");
        if (res && res->status == 200) {
            std::cout << "t=" << t << " pos=[" << x << ", " << y << ", " << z << "]\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}
```

---

## curl Examples (Testing)

### MRI Marshal

```bash
# Health check
curl http://localhost:8080/health

# Send ECG data
curl -X POST http://localhost:8080/frame \
  -H "Content-Type: application/json" \
  -d '{"source": "test", "data": [0.1, 0.2, 0.3], "rate_hz": 100}'

# Send pose
curl -X POST http://localhost:8080/pose \
  -H "Content-Type: application/json" \
  -d '{"p": [10, 20, 30], "R": [1,0,0,0,1,0,0,0,1], "frame": "scanner"}'

# Get latest biosignal
curl http://localhost:8080/health

# Get current pose
curl http://localhost:8080/pose

# Get latest MRI frame
curl http://localhost:8080/image/latest
```

### Robot Marshal

```bash
# Status check
curl http://localhost:8081/

# Read localization data
curl http://localhost:8081/read/localization_data

# Write data
curl -X POST http://localhost:8081/write/localization_data \
  -H "Content-Type: application/json" \
  -d '{"client_id": "test", "sent_at": 123456789, "values": [1.0, 2.0]}'

# Read forward kinematics
curl http://localhost:8081/read/forward_kinematics
```

---

## HDF5 Data Files

The MRI Marshal stores image data in HDF5 format with canonical ISMRMRD HDF5 format.

### File Location

```bash
# Inside container
/data/mri_data/

# On host (if using volume mount)
./data/mri_data/
```

### Reading HDF5 Files

```python
#!/usr/bin/env python3
"""Read HDF5 files from MRI Marshal."""
import h5py
import numpy as np

# Open in HDF5 mode for concurrent reading
with h5py.File("./data/mri_data/acquisition_001.h5", "r", hdf5=True) as f:
    # List datasets
    print("Datasets:", list(f.keys()))

    # Read image data
    if "images" in f:
        images = f["images"][:]
        print(f"Images shape: {images.shape}")

    # Read metadata
    if "metadata" in f:
        for key in f["metadata"].attrs:
            print(f"  {key}: {f['metadata'].attrs[key]}")
```

### HDF5 Considerations

- Set `HDF5_USE_FILE_LOCKING=FALSE` environment variable
- Open files with `hdf5=True` for reading while marshal is writing
- Use `dataset.refresh()` to see latest data in long-running readers

---

## Authentication & Security

**Current Implementation:** No authentication required.

The marshals are designed for internal network use in a controlled lab/clinical environment. For production deployment:

1. **Network isolation:** Run on isolated network segment
2. **Firewall:** Restrict port access to known client IPs
3. **TLS:** Consider nginx reverse proxy for HTTPS
4. **VPN:** Require VPN for remote access

---

## Known Limitations

### Visualization Client (viz_client) Display Performance

The viz_client runs in a Docker container and uses X11 forwarding to display images. Due to X11 forwarding overhead, the **display window** shows ~15 fps even though:
- Image streamer is sending at 20+ fps ✓
- Marshals are receiving all frames at full rate ✓
- Viz client is processing frames internally at 30+ fps ✓
- X11 display rendering to host causes the bottleneck ✗

**This is a known Docker X11 limitation and does NOT affect:**
- **External client integration** (marshals serve data at full 20+ fps via HTTP/WebSocket)
- Data ingestion pipeline (all frames processed correctly)
- Production deployments (visualization is optional for demos)

**Context**: The original demo ran viz_client natively on the host, achieving 19-20 fps display. The containerized version prioritizes ease of deployment over display performance.

**Workarounds if higher display FPS is needed:**
1. Run viz_client natively on host (requires C++ build toolchain)
2. Use WebSocket to stream data to external visualization tool
3. Accept 15 fps display as acceptable for demo verification (recommended)

**For USB handoff/external client use cases**: This limitation is not relevant, as external clients connect directly to the marshals which operate at full performance.

---

## Troubleshooting

### Connection Refused

```bash
# Check if containers are running
docker ps | grep cwru

# Check logs
docker logs cwru-mri-marshal
docker logs cwru-robot-marshal

# Verify ports
netstat -tlnp | grep -E '8080|8081|8090'
```

### Health Check Fails

```bash
# MRI Marshal
curl -v http://localhost:8080/health

# Robot Marshal
curl -v http://localhost:8081/
```

### Permission Denied on Data Files

```bash
# Fix ownership
sudo chown -R $USER:$USER ./data
chmod -R 755 ./data
```

### HDF5 Locking Errors

```bash
# Set environment variable before running client
export HDF5_USE_FILE_LOCKING=FALSE
python3 my_client.py
```

### Container Name Resolution (from other containers)

Ensure containers are on the same network:

```yaml
# docker-compose.yml
services:
  my-client:
    networks:
      - cwru-net

networks:
  cwru-net:
    external: true  # Use existing network from marshal compose
```

---

## Reference Implementations

Full working client implementations are available:

| Client | Language | Location |
|--------|----------|----------|
| ECG Mock | Python | [clients/mocks/ecg_client.py](../clients/mocks/ecg_client.py) |
| Pose Mock | Python | [clients/mocks/pose_client.py](../clients/mocks/pose_client.py) |
| Image Streamer | C++ | `mri_data_marshal_worktree/clients/` |
| Viz Client | C++ | `mri_data_marshal_worktree/clients/` |
| Robot Clients | C++ | `robot_data_marshal_worktree/clients/` |

---

## Summary Checklist

1. [ ] Docker images loaded (`docker images | grep cwru`)
2. [ ] Marshals running (`docker compose up -d`)
3. [ ] Health checks pass (`curl localhost:8080/health`)
4. [ ] Network access configured (localhost or IP)
5. [ ] Client connects to correct endpoints
6. [ ] Data format matches API specification
7. [ ] HDF5 locking disabled if reading data files
