# MRI Data Marshal: Usage & API Reference

**Technical reference for integrating with and operating the MRI Data Marshal system**

---

## Entry Points

### MRI Data Marshal

The main data ingestion and storage server.

```bash
./build/marshal --http 127.0.0.1:8080 \
                --ws 127.0.0.1:8090 \
                --data ./data_mri \
                --flush-frames 1
```

### Image Streamer

Synthetic frame generator for testing and demos.

```bash
./build/image_streamer --http http://127.0.0.1:8080 \
                       --frames 300 \
                       --dt-ms 50 \
                       --size 192 \
                       --nslices 10
```

### Visualizer Client

Real-time OpenCV display with slice navigation.

```bash
./build/viz_client --http http://127.0.0.1:8080/v1/mrd/latest
```

### Robot Marshal

State blackboard for multi-client coordination.

```bash
./build/robot_marshal_demo 8081
```

---

## Configuration Parameters

### MRI Marshal Options

| Parameter | Description | Default | Example |
|-----------|-------------|---------|---------|
| `--http` | HTTP server address:port | `127.0.0.1:8080` | `0.0.0.0:9000` |
| `--ws` | WebSocket server address:port | `127.0.0.1:8090` | `0.0.0.0:9090` |
| `--data` | HDF5 storage directory | `./data` | `/mnt/fast_ssd/mri` |
| `--flush-frames` | Flush to disk after N frames | `1` | `10` (for batching) |

### Image Streamer Options

| Parameter | Description | Default | Example |
|-----------|-------------|---------|---------|
| `--http` | Marshal HTTP endpoint URL | Required | `http://127.0.0.1:8080` |
| `--frames` | Total frames to generate | `0` (infinite) | `1000` |
| `--dt-ms` | Interval between frames (ms) | `50` | `100` |
| `--size` | Frame width and height (square) | `32` | `192` |
| `--nslices` | Number of Z slices per frame | `4` | `15` |

### Visualizer Options

| Parameter | Description | Default | Example |
|-----------|-------------|---------|---------|
| `--http` | Marshal latest frame endpoint | Required | `http://127.0.0.1:8080/v1/mrd/latest` |
| `--data` | Local data directory (optional) | None | `./cached_data` |

### Robot Marshal Options

| Parameter | Description | Default | Example |
|-----------|-------------|---------|---------|
| Port (positional) | HTTP server port | Required | `8081` |

---

## HTTP API Reference

### MRI Marshal Endpoints

#### POST /v1/mrd/frame

Append a frame to the active SWMR HDF5 file.

**Request:**
```http
POST /v1/mrd/frame HTTP/1.1
Content-Type: application/octet-stream
Content-Length: 2211840

<binary frame data>
```

**Headers (optional):**
| Header | Description | Default |
|--------|-------------|---------|
| `X-Frame-Width` | Frame width in pixels | Inferred |
| `X-Frame-Height` | Frame height in pixels | Inferred |
| `X-Frame-Slices` | Number of Z slices | Inferred |
| `X-Frame-Dtype` | Data type (`float32`, `uint16`) | `float32` |

**Response:**
```json
{
  "status": "ok",
  "frame_id": 42,
  "file": "mrd/session_20240115_143022.h5"
}
```

**Error Response:**
```json
{
  "status": "error",
  "message": "SWMR write failed: file locked"
}
```

---

#### POST /v1/mrd/ingest

Create a new HDF5 file with a single frame (non-SWMR mode).

**Request:**
```http
POST /v1/mrd/ingest HTTP/1.1
Content-Type: application/octet-stream
Content-Length: 2211840

<binary frame data>
```

**Response:**
```json
{
  "status": "ok",
  "file": "mrd/ingest_20240115_143022.h5",
  "size_bytes": 2211840
}
```

**Use case:** Archival storage, one-time imports, batch processing.

**Note:** Slower than `/v1/mrd/frame` due to file creation overhead (~1 fps vs ~19 fps).

---

#### GET /v1/mrd/latest

Retrieve the most recent frame from the SWMR file.

**Request:**
```http
GET /v1/mrd/latest HTTP/1.1
Accept: application/octet-stream
```

**Response:**
```http
HTTP/1.1 200 OK
Content-Type: application/octet-stream
X-Frame-Id: 42
X-Frame-Width: 192
X-Frame-Height: 192
X-Frame-Slices: 10
X-Frame-Dtype: float32

<binary frame data>
```

**Response (no frames yet):**
```http
HTTP/1.1 204 No Content
```

---

#### GET /health

Server health check.

**Request:**
```http
GET /health HTTP/1.1
```

**Response:**
```json
{
  "status": "healthy",
  "uptime_seconds": 3600,
  "frames_written": 1000,
  "active_readers": 2
}
```

---

### Robot Marshal Endpoints

#### POST /write/{filename}

Write state data to a named file.

**Request:**
```http
POST /write/ecg_data HTTP/1.1
Content-Type: application/json

{"timestamp": 1705341022, "heart_rate": 72, "rhythm": "normal"}
```

**Response:**
```json
{
  "status": "ok",
  "entries": 1
}
```

---

#### GET /read/{filename}

Read the latest entry from a state file.

**Request:**
```http
GET /read/ecg_data HTTP/1.1
```

**Response:**
```json
{
  "status": "ok",
  "data": {"timestamp": 1705341022, "heart_rate": 72, "rhythm": "normal"}
}
```

---

#### GET /read/{filename}?last=N

Read the N most recent entries.

**Request:**
```http
GET /read/ecg_data?last=10 HTTP/1.1
```

**Response:**
```json
{
  "status": "ok",
  "count": 10,
  "data": [
    {"timestamp": 1705341022, "heart_rate": 72},
    {"timestamp": 1705341021, "heart_rate": 71},
    ...
  ]
}
```

---

## Visualizer Controls

Interactive keyboard controls when the OpenCV window is focused:

| Key | Action | Description |
|-----|--------|-------------|
| UP Arrow | Previous slice | Navigate to lower Z index |
| DOWN Arrow | Next slice | Navigate to higher Z index |
| ESC | Exit | Close visualizer window |

**Display Information:**
- Current slice number shown in window title
- Frame counter displayed on image
- Grayscale rendering of MRI data

---

## Client Integration Examples

### Python Client (Reading Frames)

```python
import requests
import numpy as np

def get_latest_frame(marshal_url="http://127.0.0.1:8080"):
    """Fetch the latest MRI frame from the marshal."""
    response = requests.get(f"{marshal_url}/v1/mrd/latest")

    if response.status_code == 204:
        return None  # No frames available yet

    response.raise_for_status()

    # Parse frame dimensions from headers
    width = int(response.headers.get("X-Frame-Width", 192))
    height = int(response.headers.get("X-Frame-Height", 192))
    slices = int(response.headers.get("X-Frame-Slices", 10))
    dtype = response.headers.get("X-Frame-Dtype", "float32")

    # Convert binary data to numpy array
    data = np.frombuffer(response.content, dtype=dtype)
    frame = data.reshape((slices, height, width))

    return frame

# Usage
frame = get_latest_frame()
if frame is not None:
    print(f"Frame shape: {frame.shape}")
    print(f"Frame range: [{frame.min():.2f}, {frame.max():.2f}]")
```

### Python Client (Streaming Frames)

```python
import requests
import numpy as np
import time

def stream_frames(marshal_url="http://127.0.0.1:8080", width=192, height=192, slices=10):
    """Generate synthetic frames and POST to marshal."""
    for i in range(100):
        # Generate synthetic frame (replace with real MRI data)
        frame = np.random.rand(slices, height, width).astype(np.float32)

        # POST to marshal
        response = requests.post(
            f"{marshal_url}/v1/mrd/frame",
            data=frame.tobytes(),
            headers={
                "Content-Type": "application/octet-stream",
                "X-Frame-Width": str(width),
                "X-Frame-Height": str(height),
                "X-Frame-Slices": str(slices),
                "X-Frame-Dtype": "float32"
            }
        )

        if response.status_code == 200:
            print(f"Frame {i} sent successfully")
        else:
            print(f"Frame {i} failed: {response.text}")

        time.sleep(0.050)  # 50ms interval

stream_frames()
```

### C++ Client (Using cpp-httplib)

```cpp
#include <httplib.h>
#include <vector>
#include <iostream>

int main() {
    httplib::Client cli("127.0.0.1", 8080);

    // Read latest frame
    auto res = cli.Get("/v1/mrd/latest");
    if (res && res->status == 200) {
        int width = std::stoi(res->get_header_value("X-Frame-Width"));
        int height = std::stoi(res->get_header_value("X-Frame-Height"));
        int slices = std::stoi(res->get_header_value("X-Frame-Slices"));

        std::cout << "Frame: " << width << "x" << height << "x" << slices << std::endl;

        // Access raw data
        const float* data = reinterpret_cast<const float*>(res->body.data());
        size_t num_pixels = width * height * slices;
        // Process data...
    }

    return 0;
}
```

### Curl Examples

```bash
# Health check
curl http://127.0.0.1:8080/health

# Get latest frame (save to file)
curl -o frame.bin http://127.0.0.1:8080/v1/mrd/latest

# Post a frame from file
curl -X POST \
  -H "Content-Type: application/octet-stream" \
  -H "X-Frame-Width: 192" \
  -H "X-Frame-Height: 192" \
  -H "X-Frame-Slices: 10" \
  --data-binary @frame.bin \
  http://127.0.0.1:8080/v1/mrd/frame

# Robot marshal: write state
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"position": [1.0, 2.0, 3.0]}' \
  http://127.0.0.1:8081/write/robot_pose

# Robot marshal: read state
curl http://127.0.0.1:8081/read/robot_pose
```

---

## Performance Tuning

### For Real-Time Display (~20 fps)

Optimized for immediate visualization:

```bash
# Marshal: flush every frame for real-time SWMR reads
./build/marshal --http 127.0.0.1:8080 --flush-frames 1

# Streamer: 50ms interval is sustainable
./build/image_streamer --http http://127.0.0.1:8080 \
                       --dt-ms 50 \
                       --size 128 \
                       --nslices 10
```

**Expected:** ~19 fps display, ~8ms read latency

### For Maximum Throughput

Optimized for data ingestion (sacrifices real-time):

```bash
# Marshal: batch 10 frames before flush
./build/marshal --http 127.0.0.1:8080 --flush-frames 10

# Streamer: can request faster intervals (will be limited by HDF5)
./build/image_streamer --http http://127.0.0.1:8080 \
                       --dt-ms 20 \
                       --size 256 \
                       --nslices 20
```

**Expected:** 4-8 fps effective, 40-80 MB/s throughput, 100ms+ latency

### For Large Frame Sizes

Optimized for high-resolution MRI:

```bash
# Use smaller flush intervals to prevent memory buildup
./build/marshal --http 127.0.0.1:8080 --flush-frames 1

# Larger frames, slower intervals
./build/image_streamer --http http://127.0.0.1:8080 \
                       --dt-ms 100 \
                       --size 512 \
                       --nslices 32

# Frame size: 512x512x32 = 33.5 MB per frame
```

**Expected:** ~10 fps due to HDF5 overhead, works but slower

---

## Data Format Reference

### HDF5 File Structure

```
session_YYYYMMDD_HHMMSS.h5
├── frames/
│   ├── frame_000000  [H x W x S float32]
│   ├── frame_000001  [H x W x S float32]
│   └── ...
├── metadata/
│   ├── timestamp     [N int64]
│   ├── width         [scalar int32]
│   ├── height        [scalar int32]
│   └── slices        [scalar int32]
└── SWMR_ENABLED     [attribute: bool]
```

### Reading HDF5 with Python (h5py)

```python
import h5py
import numpy as np

with h5py.File("data_mri/mrd/session.h5", "r", swmr=True) as f:
    # Refresh to see latest SWMR data
    f.id.refresh()

    # Read frame count
    frame_count = len(f["frames"])
    print(f"Frames: {frame_count}")

    # Read latest frame
    latest = f["frames"][f"frame_{frame_count-1:06d}"][:]
    print(f"Shape: {latest.shape}")
```

### Reading HDF5 with MATLAB

```matlab
% Open file in SWMR mode
file = H5F.open('data_mri/mrd/session.h5', 'H5F_ACC_RDONLY', 'H5P_DEFAULT');

% Read frame
frame = h5read('data_mri/mrd/session.h5', '/frames/frame_000000');
disp(size(frame));

% Close
H5F.close(file);
```

---

## WebSocket API (Port 8090)

For real-time streaming without polling.

### Connection

```javascript
const ws = new WebSocket('ws://127.0.0.1:8090/ws');

ws.onmessage = (event) => {
    const metadata = JSON.parse(event.data);
    // Process metadata notification...
    console.log(metadata.type, metadata.ts);
};
```

### Message Format

JSON metadata notifications broadcast on subscribed topics (e.g., `mrd`, `pose`, `bio`).

**Example notification:**
```json
{
  "type": "mrd",
  "stream_id": "cardiac_scan",
  "seq": 42,
  "ts": "2026-01-15T10:30:45.123Z"
}
```

Clients read actual frame data via SWMR HDF5 file access, not via WebSocket.

---

## File Organization

```
/workspaces/cwru_data_marshal/
├── build/                      # Compiled binaries
│   ├── marshal                 # MRI data marshal server
│   ├── image_streamer          # Synthetic frame generator
│   ├── viz_client              # OpenCV visualizer
│   └── robot_marshal_demo      # State blackboard server
├── src/                        # Core library source
├── clients/                    # Client application source
│   ├── image_streamer/
│   └── viz_client/
├── scripts/
│   ├── run_demo_simultaneous.sh    # Full demo script
│   ├── run_demo_manual.sh          # Step-by-step demo
│   └── benchmarks/
│       ├── mri_marshal_stress_test.sh
│       └── swmr_continuous_bench.sh
├── data_mri/                   # MRI HDF5 storage (created at runtime)
│   └── mrd/
├── data_robot/                 # Robot state storage (created at runtime)
└── docs/                       # Documentation
```

---

## Error Codes Reference

| HTTP Code | Meaning | Cause |
|-----------|---------|-------|
| 200 | Success | Operation completed |
| 201 | Created | File ingested successfully |
| 202 | Accepted | K-space queued for async reconstruction |
| 204 | No Content | No data available yet (empty cache) |
| 400 | Bad Request | Invalid frame data, unknown format, or missing parameters |
| 500 | Server Error | SWMR write failed, disk full, etc. |
| 501 | Not Implemented | Reconstruction service not configured |
| 502 | Bad Gateway | Reconstruction service failed |
| 503 | Service Unavailable | Server overloaded or shutting down |

---

*For architecture overview, see [MRI_DATA_MARSHAL_PRESENTATION.md](MRI_DATA_MARSHAL_PRESENTATION.md)*
*For performance optimization, see [IMPROVEMENTS_AND_OPTIMIZATION.md](IMPROVEMENTS_AND_OPTIMIZATION.md)*
