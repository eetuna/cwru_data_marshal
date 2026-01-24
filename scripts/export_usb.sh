#!/bin/bash
#
# USB Export Script for CWRU Data Marshal Demo
#
# Creates a portable deployment package containing:
# - All 7 Docker images for the demo (as .tar files)
# - docker-compose.demo.yml
# - demo-docker.sh launcher script
# - External client integration guide
# - README with instructions for receiver
#
# Usage:
#   ./scripts/export_usb.sh <output_directory>
#
# Example:
#   ./scripts/export_usb.sh /media/usb/cwru_marshal_demo
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Check arguments
if [ $# -ne 1 ]; then
    echo "Usage: $0 <output_directory>"
    echo ""
    echo "Example:"
    echo "  $0 /media/usb/cwru_marshal_deploy"
    exit 1
fi

OUT_DIR="$1"

# Validate output directory
if [ -e "$OUT_DIR" ]; then
    echo "Error: Output directory already exists: $OUT_DIR"
    echo "Please remove it first or choose a different location."
    exit 1
fi

echo "============================================"
echo "CWRU Data Marshal - USB Export Script"
echo "============================================"
echo ""
echo "Output directory: $OUT_DIR"
echo "Project root: $PROJECT_ROOT"
echo ""

# Create directory structure
echo "[1/6] Creating directory structure..."
mkdir -p "$OUT_DIR"/{images,docs}

# Build Docker images
echo ""
echo "[2/6] Building Docker images..."
cd "$PROJECT_ROOT"
docker compose -f docker-compose.demo.yml build

# Verify all 7 images were built
REQUIRED_IMAGES=(
    "cwru/mri-marshal"
    "cwru/robot-marshal"
    "cwru/image-streamer"
    "cwru/ecg-client"
    "cwru/pose-client"
    "cwru/viz-client"
    "cwru/robot-clients"
)

echo "Verifying images..."
for img in "${REQUIRED_IMAGES[@]}"; do
    if ! docker images | grep -q "$img"; then
        echo "Error: $img image not found after build"
        exit 1
    fi
    echo "  ✓ $img"
done

# Save Docker images to tar files
echo ""
echo "[3/6] Exporting Docker images (this may take several minutes)..."
docker save -o "$OUT_DIR/images/cwru-demo-images.tar" \
    cwru/mri-marshal:latest \
    cwru/robot-marshal:latest \
    cwru/image-streamer:latest \
    cwru/ecg-client:latest \
    cwru/pose-client:latest \
    cwru/viz-client:latest \
    cwru/robot-clients:latest

# Get total size
TOTAL_SIZE=$(du -h "$OUT_DIR/images/cwru-demo-images.tar" | cut -f1)
echo "  ✓ Saved all 7 images: $TOTAL_SIZE"

# Copy demo files
echo ""
echo "[4/6] Copying demo configuration files..."
cp "$PROJECT_ROOT/docker-compose.demo.yml" "$OUT_DIR/"
cp "$PROJECT_ROOT/.env.demo" "$OUT_DIR/"
cp "$PROJECT_ROOT/scripts/demo-docker.sh" "$OUT_DIR/"
cp "$PROJECT_ROOT/scripts/demo-persistent.sh" "$OUT_DIR/"
chmod +x "$OUT_DIR/demo-docker.sh"
chmod +x "$OUT_DIR/demo-persistent.sh"
echo "  ✓ docker-compose.demo.yml"
echo "  ✓ .env.demo"
echo "  ✓ demo-docker.sh"
echo "  ✓ demo-persistent.sh"

# Copy documentation
echo ""
echo "[5/6] Copying documentation..."
if [ -f "$PROJECT_ROOT/docs/EXTERNAL_CLIENT_GUIDE.md" ]; then
    cp "$PROJECT_ROOT/docs/EXTERNAL_CLIENT_GUIDE.md" "$OUT_DIR/docs/"
    echo "  ✓ EXTERNAL_CLIENT_GUIDE.md"
fi
if [ -f "$PROJECT_ROOT/docs/DEMO_AND_API_EXPORT.md" ]; then
    cp "$PROJECT_ROOT/docs/DEMO_AND_API_EXPORT.md" "$OUT_DIR/docs/"
    echo "  ✓ DEMO_AND_API_EXPORT.md"
fi

# Create comprehensive API reference
echo "  ✓ Creating API_REFERENCE.md..."
cat > "$OUT_DIR/docs/API_REFERENCE.md" <<'APIEOF'
# CWRU Data Marshal - Complete API Reference

This document provides a complete reference for connecting external clients to the CWRU Data Marshal system.

## Table of Contents

1. [System Architecture](#system-architecture)
2. [MRI Marshal API](#mri-marshal-api)
3. [Robot Marshal API](#robot-marshal-api)
4. [WebSocket Notifications](#websocket-notifications)
5. [Example Client Code](#example-client-code)
6. [Data Formats](#data-formats)

---

## System Architecture

### Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                     CWRU Data Marshal System                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ┌───────────────┐         ┌────────────────┐                   │
│  │  MRI Marshal  │         │ Robot Marshal  │                   │
│  │  Port: 8080   │         │  Port: 8081    │                   │
│  │  WS: 8090     │         │                │                   │
│  └───────┬───────┘         └────────┬───────┘                   │
│          │                          │                            │
│  ┌───────▼────────┐         ┌───────▼────────┐                 │
│  │  MRI Data      │         │  Robot Data    │                 │
│  │  - Images      │         │  - Positions   │                 │
│  │  - ECG         │         │  - Control     │                 │
│  │  - Poses       │         │  - Tracking    │                 │
│  │  (HDF5/JSONL)  │         │  (JSON)        │                 │
│  └────────────────┘         └────────────────┘                  │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
         ▲                            ▲
         │                            │
    ┌────┴────┐                  ┌────┴────┐
    │ Your    │                  │ Your    │
    │ MRI     │                  │ Robot   │
    │ Client  │                  │ Client  │
    └─────────┘                  └─────────┘
```

### Components

**MRI Marshal (Port 8080, WebSocket 8090)**
- Manages MRI imaging data (ISMRMRD format)
- Stores ECG/biological signals (JSONL)
- Stores pose/tracking data (JSONL)
- Provides HTTP REST API for data access
- Broadcasts notifications via WebSocket

**Robot Marshal (Port 8081)**
- Manages robot control data exchange
- Provides file-based communication (like shared memory)
- 10+ predefined data channels (tip_position, planned_motion, etc.)
- JSON-based request/response

---

## Complete System Dataflow

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          DATA PRODUCERS                                  │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                           │
│  ┌──────────────┐   ┌──────────┐   ┌──────────┐                        │
│  │Image Streamer│   │ECG Client│   │Pose Client│                       │
│  │(MRI Scanner) │   │(Biosensor)│   │(Tracker)  │                       │
│  └──────┬───────┘   └────┬─────┘   └────┬──────┘                        │
│         │                │              │                                │
│         │ POST MRD       │ POST bio     │ POST poses                    │
│         │ 50fps          │ 1 Hz         │ 10 Hz                         │
│         ▼                ▼              ▼                                │
│  ┌─────────────────────────────────────────────┐                        │
│  │        MRI MARSHAL (Port 8080)              │                        │
│  │  ┌──────────────────────────────────────┐   │                        │
│  │  │  HTTP Server (REST API)              │   │                        │
│  │  │  - POST /v1/mrd (receive frames)     │   │                        │
│  │  │  - GET /v1/mrd/latest (metadata)     │   │                        │
│  │  │  - GET /v1/mrd/latest/frame (binary) │   │                        │
│  │  │  - POST/GET /v1/mrd/bio, poses       │   │                        │
│  │  └────────────┬─────────────────────────┘   │                        │
│  │               │                              │                        │
│  │               ▼                              │                        │
│  │  ┌──────────────────────────────────────┐   │                        │
│  │  │  Data Storage Layer                  │   │                        │
│  │  │  ┌────────────┐  ┌────────────────┐  │   │                        │
│  │  │  │ HDF5 Files │  │ JSONL Files    │  │   │                        │
│  │  │  │ (SWMR mode)│  │ (time-series)  │  │   │                        │
│  │  │  │ /images/   │  │ bio.jsonl      │  │   │                        │
│  │  │  │   data     │  │ poses.jsonl    │  │   │                        │
│  │  │  └────────────┘  └────────────────┘  │   │                        │
│  │  └──────────────────────────────────────┘   │                        │
│  │               │                              │                        │
│  │               ▼                              │                        │
│  │  ┌──────────────────────────────────────┐   │                        │
│  │  │  WebSocket Server (Port 8090)        │   │                        │
│  │  │  Broadcasts: {"type":"mrd_update",   │   │                        │
│  │  │              "frame_index": 1234}    │   │                        │
│  │  └────────────┬─────────────────────────┘   │                        │
│  └───────────────┼─────────────────────────────┘                        │
│                  │                                                       │
│                  │ ws://localhost:8090/ws                                │
│                  │ {"subscribe":"mrd"}                                   │
└──────────────────┼───────────────────────────────────────────────────────┘
                   │
                   │
┌──────────────────┼───────────────────────────────────────────────────────┐
│                  │        DATA CONSUMERS                                  │
├──────────────────┼───────────────────────────────────────────────────────┤
│                  │                                                        │
│                  │                                                        │
│         ┌────────▼────────┐                                              │
│         │  WebSocket       │                                              │
│         │  Notification    │                                              │
│         │  (New frame!)    │                                              │
│         └────────┬─────────┘                                              │
│                  │                                                        │
│                  ▼                                                        │
│    ┌──────────────────────────────────────┐                              │
│    │  YOUR MRI CLIENTS                    │                              │
│    │  ┌────────────┐   ┌────────────────┐ │                              │
│    │  │ Viz Client │   │ Your Custom    │ │                              │
│    │  │ (OpenCV)   │   │ Python Client  │ │                              │
│    │  └─────┬──────┘   └────────┬───────┘ │                              │
│    │        │                   │         │                              │
│    │        │ GET /v1/mrd/latest/frame    │                              │
│    │        │ (binary data)     │         │                              │
│    │        │                   │         │                              │
│    │        ▼                   ▼         │                              │
│    │   [Display]          [Process/Save] │                              │
│    └──────────────────────────────────────┘                              │
│                                                                           │
└───────────────────────────────────────────────────────────────────────────┘


┌───────────────────────────────────────────────────────────────────────────┐
│                     ROBOT CONTROL SYSTEM                                   │
├───────────────────────────────────────────────────────────────────────────┤
│                                                                            │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────┐                 │
│  │  Planning    │   │  Controller  │   │  Tracking    │                 │
│  │  Client      │   │  Client      │   │  Client      │                 │
│  │              │   │              │   │              │                 │
│  │ Computes     │   │ Sends motor  │   │ Reads sensor │                 │
│  │ path         │   │ commands     │   │ data         │                 │
│  └──────┬───────┘   └──────┬───────┘   └──────┬───────┘                 │
│         │                  │                  │                          │
│         │ POST /write/     │ POST /write/     │ GET /read/               │
│         │ desired_motion   │ user_input       │ localization             │
│         │                  │                  │                          │
│         ▼                  ▼                  ▼                          │
│  ┌───────────────────────────────────────────────────┐                   │
│  │     ROBOT MARSHAL (Port 8081)                     │                   │
│  │  ┌─────────────────────────────────────────────┐  │                   │
│  │  │  HTTP Server                                │  │                   │
│  │  │  - GET /read/<filename>                     │  │                   │
│  │  │  - POST /write/<filename>                   │  │                   │
│  │  │  - GET / (list all files)                   │  │                   │
│  │  └──────────────┬──────────────────────────────┘  │                   │
│  │                 │                                  │                   │
│  │                 ▼                                  │                   │
│  │  ┌─────────────────────────────────────────────┐  │                   │
│  │  │  In-Memory "Virtual File System"           │  │                   │
│  │  │                                             │  │                   │
│  │  │  tip_position_orientation: {values:[...]}  │  │                   │
│  │  │  desired_planned_motion:   {values:[...]}  │  │                   │
│  │  │  user_input:               {values:[...]}  │  │                   │
│  │  │  localization_data:        {values:[...]}  │  │                   │
│  │  │  surface_model_parameters: {values:[...]}  │  │                   │
│  │  │  ... (10+ more channels)                   │  │                   │
│  │  │                                             │  │                   │
│  │  └─────────────────────────────────────────────┘  │                   │
│  └───────────────────────────────────────────────────┘                   │
│                         ▲                                                 │
│                         │                                                 │
│                         │ GET /read/tip_position                          │
│                         │                                                 │
│                   ┌─────┴──────┐                                          │
│                   │ YOUR ROBOT │                                          │
│                   │ CLIENT     │                                          │
│                   └────────────┘                                          │
│                                                                            │
└────────────────────────────────────────────────────────────────────────────┘

KEY CONCEPTS:

1. MRI MARSHAL PORTS:
   - 8080: HTTP REST API (GET/POST data)
   - 8090: WebSocket (real-time notifications)

2. ROBOT MARSHAL PORTS:
   - 8081: HTTP REST API (read/write shared data)

3. DATA FLOW PATTERNS:

   a) MRI Streaming (Push-Pull):
      Producer → POST data → Marshal writes to disk
      Consumer ← GET data ← Marshal reads from disk

   b) Robot Control (Shared Memory):
      All clients read/write to same "virtual files"
      No disk I/O - all in-memory JSON

4. SWMR ENABLES:
   - Writer (image-streamer) never waits for readers
   - Readers (viz-client) never block writer
   - Multiple readers can access same file simultaneously

5. WEBSOCKET PURPOSE:
   - Avoid polling overhead
   - Instant notification when new frame arrives
   - Client subscribes once, gets push updates
   - Low latency (~1ms vs 10-20ms HTTP polling)

6. WHY TWO MARSHALS?
   - MRI: Large persistent data, file-based storage
   - Robot: Small ephemeral data, fast in-memory exchange
   - Different use cases → different architectures
```

---

## MRI Marshal API

**Base URL:** `http://localhost:8080`

### Architecture Overview

The MRI Marshal uses a **hybrid storage architecture**:
1. **HDF5 Files (SWMR Mode)** - Image data stored in ISMRMRD format
2. **JSONL Files** - Time-series data (ECG, poses) stored as JSON lines
3. **HTTP REST API** - Provides access to both storage types
4. **WebSocket Server** - Real-time notifications when new data arrives

### SWMR (Single Writer Multiple Readers) Mode

**What is SWMR?**
- HDF5 feature that allows one writer and multiple concurrent readers
- Writer (image-streamer) continuously appends new frames
- Readers (viz-client, your clients) can access growing file without blocking writer
- No file locking conflicts - critical for real-time streaming

**How it works:**
1. Writer opens file in SWMR write mode, appends frames incrementally
2. Readers open file in SWMR read mode, call `H5Drefresh()` to see new data
3. Readers never block writer, writer never blocks readers
4. Enables real-time access to data being actively written

**Why this matters:**
- Traditional HDF5: Must close/reopen file to see new data
- SWMR: See new data immediately without reopening
- Perfect for live MRI streaming scenarios

### Metadata vs. Binary Data Transfer

The MRI Marshal provides **two ways** to access frame data:

**1. Metadata Only** (`/v1/mrd/latest`)
- Returns JSON with file path and frame index
- Lightweight (~100 bytes)
- Use for: Checking if new data exists, getting frame count

**2. Full Binary Data** (`/v1/mrd/latest/frame`)
- Returns raw float32 pixel data
- Heavy (~50-500 KB depending on dimensions)
- Use for: Actual visualization, image processing

This separation allows efficient polling for new data without transferring large binary payloads.

---

### Endpoints

#### 1. Health Check
```http
GET /health
```

**Purpose:** Verify marshal is running and responsive

**Response:**
```json
{
  "status": "ok"
}
```

**Use Case:** Container health checks, client startup validation

---

#### 2. Get Latest MRI Frame Metadata
```http
GET /v1/mrd/latest
```

**Purpose:** Get metadata about the latest available frame WITHOUT downloading the actual image data

**How it works internally:**
1. Marshal reads `session-data/mrd/latest.json` file (updated by image-streamer)
2. Extracts file path and current frame index
3. Returns lightweight JSON response

**Response:**
```json
{
  "data": {
    "path": "/session-data/run_20260124_183456/mrd/demo_stream-64x64x3-g0000.mrd",
    "frame_index": 1234,
    "total_frames": 1235
  }
}
```

**Fields explained:**
- `path` - Absolute path to HDF5 file containing the frame
- `frame_index` - Index of latest frame (0-based)
- `total_frames` - Total number of frames written so far

**Use Cases:**
- Polling loop: Check if new frames arrived without downloading data
- Frame counting: Track how many frames have been captured
- File discovery: Find which HDF5 file contains current data

**Typical workflow:**
```python
# Poll every 20ms
while True:
    meta = requests.get('http://localhost:8080/v1/mrd/latest').json()
    if meta['data']['frame_index'] != last_frame:
        # New frame! Now fetch the actual data
        fetch_frame_data()
    time.sleep(0.02)
```

---

#### 3. Get Latest Frame Full Data
```http
GET /v1/mrd/latest/frame
```

**Purpose:** Download the complete binary image data for the latest frame

**How it works internally:**
1. Marshal reads `latest.json` to get file path and frame index
2. Opens HDF5 file in **SWMR read mode** (doesn't block writer)
3. Calls `H5Drefresh()` to see latest data from active writer
4. Reads HDF5 dataset `/images/data` at specified frame index
5. Extracts dimensions from dataset shape
6. Streams raw float32 binary data to HTTP response

**Response Headers:**
```
X-MRD-Frame-Index: 1234
X-MRD-Total-Frames: 1235
X-MRD-Dimensions: 64,64,3,1,1  (nx,ny,nz,channels,frames)
Content-Type: application/octet-stream
```

**Header fields explained:**
- `X-MRD-Frame-Index` - Which frame this is (for verification)
- `X-MRD-Total-Frames` - Total frames in file
- `X-MRD-Dimensions` - Array dimensions: width, height, slices, coil channels, temporal frames
- `Content-Type: application/octet-stream` - Binary data (not JSON)

**Response Body:**
- Raw IEEE 754 float32 binary data
- **No compression, no encoding** - direct memory dump
- Size in bytes: `nx × ny × nz × channels × 4 bytes`
- Example: 64×64×3 = 49,152 bytes

**Data layout:**
```
[frame0_channel0_slice0_row0_col0, col1, col2, ... colN,
 frame0_channel0_slice0_row1_col0, col1, col2, ... colN,
 ...
 frame0_channel0_sliceN_rowN_colN]
```

**Why binary instead of JSON?**
- JSON: 64×64×3 frame = ~500 KB (as text numbers)
- Binary: 64×64×3 frame = ~49 KB (4 bytes per pixel)
- **10x smaller, 100x faster to parse**

**Use Cases:**
- Visualization: Display frames in real-time viewer
- Processing: Run image analysis algorithms
- Recording: Save frames to disk for offline analysis

**Typical workflow:**
```python
# Efficient: Parse binary directly into numpy array
response = requests.get('http://localhost:8080/v1/mrd/latest/frame')
dims = response.headers['X-MRD-Dimensions'].split(',')
nx, ny, nz = int(dims[0]), int(dims[1]), int(dims[2])

# Convert binary to numpy array
data = np.frombuffer(response.content, dtype=np.float32)
frame = data.reshape((nz, ny, nx))  # 3D volume
```

---

#### 4. Get ISMRMRD Header
```http
GET /v1/mrd/latest/header
```

**Response:**
```json
{
  "acquisitionSystemInformation": {
    "systemVendor": "CWRU",
    "systemModel": "Demo",
    "systemFieldStrength_T": 1.5,
    "receiverChannels": 1
  },
  "encoding": [{
    "encodedSpace": {
      "matrixSize": {"x": 64, "y": 64, "z": 3},
      "fieldOfView_mm": {"x": 256.0, "y": 256.0, "z": 30.0}
    }
  }]
}
```

---

#### 5. Get ECG/Biological Signals
```http
GET /v1/mrd/bio
```

**Response:**
```json
{
  "entries": [
    {
      "timestamp": "2026-01-24T18:23:45.123Z",
      "heart_rate_bpm": 72,
      "ecg_value": 0.523
    }
  ]
}
```

---

#### 6. Get Pose Data
```http
GET /v1/mrd/poses
```

**Response:**
```json
{
  "entries": [
    {
      "timestamp": "2026-01-24T18:23:45.123Z",
      "position": [1.2, 3.4, 5.6],
      "orientation": [0.0, 0.0, 0.707, 0.707]
    }
  ]
}
```

---

#### 7. Post ECG Data (External Client)
```http
POST /v1/mrd/bio
Content-Type: application/json

{
  "heart_rate_bpm": 75,
  "ecg_value": 0.623
}
```

---

#### 8. Post Pose Data (External Client)
```http
POST /v1/mrd/poses
Content-Type: application/json

{
  "position": [1.0, 2.0, 3.0],
  "orientation": [0.0, 0.0, 1.0, 0.0]
}
```

---

## Robot Marshal API

**Base URL:** `http://localhost:8081`

### File-Based Communication Model

Robot Marshal uses a "virtual file system" where clients read/write JSON data to named endpoints.

### Available Files (Data Channels)

| Endpoint | Description | Read By | Written By |
|----------|-------------|---------|------------|
| `/read/localization_data` | Sensor positions | catheter-tracking | (external) |
| `/read/catheter_base_configuration` | System config | multiple | front-end |
| `/read/forward_kinematics` | FK calculations | catheter-tracking | (external) |
| `/read/desired_planned_motion` | Motion plan | controller | planning |
| `/read/tip_position_orientation` | Catheter tip pose | controller, planning | catheter-tracking |
| `/read/biological_signals` | ECG/vitals | controller, planning | (external/MRI) |
| `/read/surface_model_parameters` | Anatomy model | planning | surface-tracking |
| `/read/user_input` | Doctor commands | controller, planning | front-end |
| `/read/streaming_2D_images` | Live imaging | front-end, surface | (external/MRI) |
| `/read/3D_images` | 3D volumes | front-end, planning | (external/MRI) |
| `/write/tip_position_orientation` | Catheter position | | catheter-tracking |
| `/write/user_input` | Commands | | front-end |
| `/write/surface_model_parameters` | Surface model | | surface-tracking |
| `/write/desired_planned_motion` | Motion plan | | planning |

### Endpoints

#### 1. Read Data from Channel
```http
GET /read/<filename>
```

**Example:**
```http
GET /read/tip_position_orientation
```

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

---

#### 2. Write Data to Channel
```http
POST /write/<filename>
Content-Type: application/json

{
  "values": [1.0, 2.0, 3.0],
  "sent_at": 1706126625123456789
}
```

**Response:**
```json
{
  "status": "ok",
  "message": "Data written successfully"
}
```

---

#### 3. List Available Files
```http
GET /
```

**Response:** HTML page listing all available read/write endpoints

---

## WebSocket Notifications

**URL:** `ws://localhost:8090/ws`

### Protocol

#### Subscribe to MRD Notifications
```json
{"subscribe": "mrd"}
```

#### Notification Format
```json
{
  "type": "mrd_update",
  "frame_index": 1234,
  "timestamp": "2026-01-24T18:23:45.123Z"
}
```

### Use Case
Real-time notifications when new MRI frames arrive, instead of polling.

---

## Example Client Code

### Python: Get Latest MRI Frame

```python
import requests
import numpy as np
import struct

# Get frame metadata
response = requests.get('http://localhost:8080/v1/mrd/latest')
data = response.json()['data']
frame_idx = data['frame_index']
print(f"Latest frame: {frame_idx}")

# Get frame binary data
response = requests.get('http://localhost:8080/v1/mrd/latest/frame')
nx = int(response.headers['X-MRD-Dimensions'].split(',')[0])
ny = int(response.headers['X-MRD-Dimensions'].split(',')[1])
nz = int(response.headers['X-MRD-Dimensions'].split(',')[2])

# Parse binary float32 data
frame_data = np.frombuffer(response.content, dtype=np.float32)
frame_data = frame_data.reshape((nz, ny, nx))
print(f"Frame shape: {frame_data.shape}")
```

---

### Python: WebSocket Real-time Updates

```python
import asyncio
import websockets
import json

async def listen_mrd():
    uri = "ws://localhost:8090/ws"
    async with websockets.connect(uri) as websocket:
        # Subscribe
        await websocket.send(json.dumps({"subscribe": "mrd"}))

        # Listen for updates
        async for message in websocket:
            data = json.loads(message)
            print(f"New frame: {data['frame_index']}")

asyncio.run(listen_mrd())
```

---

### Python: Robot Marshal Communication

```python
import requests

# Read catheter tip position
response = requests.get('http://localhost:8081/read/tip_position_orientation')
data = response.json()
tip_position = data['entries'][0]['values'][:3]  # x, y, z
print(f"Tip position: {tip_position}")

# Write user command
command = {
    "values": [10.0, 20.0, 30.0, 0.0, 0.0, 90.0],  # target position + orientation
    "sent_at": int(time.time() * 1e9)  # nanoseconds
}
requests.post('http://localhost:8081/write/user_input', json=command)
```

---

### C++: Get Latest Frame

```cpp
#include <curl/curl.h>
#include <nlohmann/json.hpp>

// HTTP GET helper
std::string http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    std::string response;
    // ... (curl setup)
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return response;
}

int main() {
    // Get latest frame info
    auto response = http_get("http://localhost:8080/v1/mrd/latest");
    auto json = nlohmann::json::parse(response);

    uint64_t frame_idx = json["data"]["frame_index"];
    std::string path = json["data"]["path"];

    std::cout << "Frame: " << frame_idx << std::endl;
    std::cout << "Path: " << path << std::endl;
}
```

---

## Data Formats

### MRD (ISMRMRD HDF5)

**File:** `session-data/mrd/*.mrd`

**Structure:**
```
/images/data         # Dataset [frames, channels, z, y, x] float32
/images/header       # XML header (ISMRMRD format)
```

**Access:**
- Via HTTP API (recommended for external clients)
- Direct HDF5 read with SWMR mode (advanced)

---

### Bio Data (JSONL)

**File:** `session-data/mrd/bio.jsonl`

**Format:** One JSON object per line
```json
{"timestamp": "2026-01-24T18:23:45.123Z", "heart_rate_bpm": 72, "ecg_value": 0.523}
{"timestamp": "2026-01-24T18:23:46.123Z", "heart_rate_bpm": 73, "ecg_value": 0.612}
```

---

### Pose Data (JSONL)

**File:** `session-data/mrd/poses.jsonl`

**Format:**
```json
{"timestamp": "2026-01-24T18:23:45.123Z", "position": [1.0, 2.0, 3.0], "orientation": [0, 0, 0.707, 0.707]}
```

---

## Common Workflows

### Workflow 1: Real-time MRI Viewer

1. Connect to WebSocket: `ws://localhost:8090/ws`
2. Subscribe: `{"subscribe":"mrd"}`
3. On notification → GET `/v1/mrd/latest/frame`
4. Parse binary data → Display

### Workflow 2: Robot Control Loop

1. Read current position: GET `/read/tip_position_orientation`
2. Read target: GET `/read/desired_planned_motion`
3. Compute control: (your algorithm)
4. Write command: POST `/write/user_input`
5. Sleep 5-10ms, repeat

### Workflow 3: Custom Data Injection

1. POST ECG data: `/v1/mrd/bio`
2. POST pose data: `/v1/mrd/poses`
3. MRI Marshal stores it alongside imaging data

---

## Performance Considerations

- **MRI Frames:** Generated at 20-50 fps (configurable via IMAGE_INTERVAL)
- **Robot Clients:** Run at ~50-80 Hz (limited by HTTP latency)
- **WebSocket:** Near-zero latency for notifications
- **Binary Frame Data:** ~50-500 KB per frame depending on dimensions

## Security Note

**This demo has NO authentication!** For production:
- Add API keys
- Use HTTPS/WSS
- Implement access control
- Add rate limiting

---

**For more details, see EXTERNAL_CLIENT_GUIDE.md**
APIEOF

echo "  ✓ API_REFERENCE.md"

# Create README for receiver
echo ""
echo "[6/6] Creating deployment README..."
cat > "$OUT_DIR/README.md" <<'EOF'
# CWRU Data Marshal - Demo Package

This package contains a complete, self-contained demo of the CWRU Data Marshal system.

## Contents

```
.
├── images/
│   └── cwru-demo-images.tar   # All 7 Docker images (single file)
├── docs/
│   ├── API_REFERENCE.md         # Complete API documentation
│   ├── EXTERNAL_CLIENT_GUIDE.md # External client integration guide
│   └── DEMO_AND_API_EXPORT.md   # Demo + API export guide (if available)
├── docker-compose.demo.yml    # Service orchestration
├── .env.demo                  # Configuration settings
├── demo-docker.sh             # Quick 30-second demo
├── demo-persistent.sh         # Persistent demo (services stay running)
└── README.md                  # This file
```

## What's Included

The demo includes:
- **2 Marshals**: MRI Marshal (HTTP + WebSocket) and Robot Marshal
- **3 Mock Data Generators**: Image streamer, ECG client, Pose client
- **5 Robot Clients**: Catheter tracking, Controller, Planning, Front-end, Surface tracking
- **1 Visualization Client**: Real-time MRI image viewer (optional, requires X11)

**Note:** All components are pre-built and packaged as Docker images. Source code is available at https://github.com/cwru-mercis/cwru_data_marshal for developers who wish to modify or rebuild components.

## Prerequisites

The receiving machine must have:
- Docker Engine (version 20.10 or later)
- Docker Compose (version 2.0 or later)
- X11 display server (for visualization client - optional)

### Installing Docker

**Ubuntu/Debian:**
```bash
curl -fsSL https://get.docker.com -o get-docker.sh
sudo sh get-docker.sh
sudo usermod -aG docker $USER
# Log out and back in for group changes
```

**Other platforms:** See https://docs.docker.com/get-docker/

## Quick Start

### 1. Load Docker Images

```bash
cd /path/to/this/directory

# Load all demo images (single file with 7 images)
docker load -i images/cwru-demo-images.tar

# Verify all images loaded
docker images | grep cwru
```

Expected output (7 images):
```
cwru/mri-marshal        latest
cwru/robot-marshal      latest
cwru/image-streamer     latest
cwru/ecg-client         latest
cwru/pose-client        latest
cwru/viz-client         latest
cwru/robot-clients      latest
```

### 2. Run the Demo

```bash
# Run 30-second demo with all services
./demo-docker.sh

# The demo will show:
# - ECG data streaming
# - Pose tracking data
# - Image frames being generated
# - Robot operations count
# - Visualization window (if X11 available)
```

### 3. Access Generated Data

**Data is automatically visible in the `session-data/` directory!**

```bash
# View generated files
ls -lh session-data/mrd/

# Expected files:
# - demo_stream-64x64x5-g0000.mrd  (MRI image data)
# - bio.jsonl                      (ECG data log)
# - poses.jsonl                    (Pose tracking log)
# - index.jsonl                    (Frame metadata)
# - latest.json                    (Latest frame pointer)
```

**Note:** By default, `CLEANUP_DATA=true` clears this directory after each demo run. To keep data between runs, edit `.env.demo` and set `CLEANUP_DATA=false`.

### 4. Verify External Client Access

While demo is running, test the marshal APIs:

```bash
# Check MRI Marshal health
curl -s http://localhost:8080/health

# Get latest MRI frame metadata
curl -s http://localhost:8080/v1/mrd/latest | jq

# Get MRI header (field strength, dimensions)
curl -s http://localhost:8080/v1/mrd/latest/header | jq '.acquisitionSystemInformation.systemFieldStrength_T'

# Check Robot Marshal
curl -s http://localhost:8081/

# Read catheter tip position
curl -s http://localhost:8081/read/tip_position_orientation | jq
```

## Service Information

### Marshal Endpoints (for External Clients)

- **MRI Marshal HTTP:** http://localhost:8080
- **MRI Marshal WebSocket:** ws://localhost:8090
- **Robot Marshal HTTP:** http://localhost:8081

### Complete API Documentation

**See `docs/API_REFERENCE.md` for:**
- Complete endpoint reference for both marshals
- Example client code (Python, C++)
- WebSocket notification protocol
- Data formats (HDF5, JSONL)
- Common workflows and use cases

**Also see:**
- `docs/EXTERNAL_CLIENT_GUIDE.md` - External client integration guide
- `docs/DEMO_AND_API_EXPORT.md` - Demo and API export guide (if available)

### Demo Configuration

Edit `.env.demo` to change:
- `DEMO_DURATION` - Demo run time (default: 30s, 0 = infinite)
- `IMAGE_WIDTH` / `IMAGE_HEIGHT` - Image dimensions (default: 64x64)
- `IMAGE_SLICES` - Slices per volume (default: 5)
- `IMAGE_INTERVAL` - Image streaming rate (default: 50ms = 20fps)
- `ECG_INTERVAL` - ECG sample rate (default: 0.5s)
- `POSE_INTERVAL` - Pose update rate (default: 0.1s)
- `ENABLE_VIZ` - Show visualization window (default: true, requires X11)
- `CLEANUP_DATA` - Remove session-data/ after demo (default: true, set false to keep data)

### Persistent Demo (Keep Services Running)

For development/testing where you want services to keep running:

```bash
./demo-persistent.sh

# Services will start and stay running even after monitoring stops
# Re-run the script to attach monitor again
# To stop: docker compose -f docker-compose.demo.yml down
```

### Manual Control - Run Each Service Separately

**Perfect for connecting your own clients!** Run each service in its own terminal:

**Terminal 1: MRI Marshal**
```bash
docker compose -f docker-compose.demo.yml up mri-marshal
```

**Terminal 2: Robot Marshal**
```bash
docker compose -f docker-compose.demo.yml up robot-marshal
```

**Terminal 3: Image Streamer (sends MRI data)**
```bash
docker compose -f docker-compose.demo.yml up image-streamer
```

**Terminal 4: ECG Client**
```bash
docker compose -f docker-compose.demo.yml up ecg-client
```

**Terminal 5: Pose Client**
```bash
docker compose -f docker-compose.demo.yml up pose-client
```

**Terminal 6: Robot Clients (all 5 together)**
```bash
docker compose -f docker-compose.demo.yml up robot-clients
```

**Terminal 7: Viz Client (optional)**
```bash
docker compose -f docker-compose.demo.yml --profile viz up viz-client
```

**Benefits:**
- See each service's logs in real-time
- Stop/restart individual services easily (Ctrl+C in that terminal)
- Connect your own custom clients while demo is running
- Test individual components in isolation

### Other Manual Commands

```bash
# Start all services in background (daemon mode)
docker compose -f docker-compose.demo.yml up -d

# Stop all services
docker compose -f docker-compose.demo.yml down

# View logs from all services
docker compose -f docker-compose.demo.yml logs -f

# View logs from specific service
docker compose -f docker-compose.demo.yml logs -f mri-marshal

# Check status
docker compose -f docker-compose.demo.yml ps

# Restart a specific service
docker compose -f docker-compose.demo.yml restart mri-marshal
```

## Troubleshooting

### Docker daemon not running
```bash
sudo systemctl start docker
sudo usermod -aG docker $USER  # Log out and back in after this
```

### Port already in use
```bash
sudo lsof -i :8080  # Find process using port
docker compose -f docker-compose.demo.yml down  # Stop demo services
```

### Visualization client not showing
- Ensure `ENABLE_VIZ=true` in `demo-docker.sh`
- Check X11 is available: `echo $DISPLAY` should show `:0` or similar
- WSL2 users: Install X server on Windows (VcXsrv, Xming, or use WSLg)

### Containers not healthy
```bash
docker compose -f docker-compose.demo.yml logs mri-marshal
docker compose -f docker-compose.demo.yml logs robot-marshal
```

## Known Limitations

- **Viz client display FPS**: Shows ~15 fps due to Docker X11 forwarding overhead
  - Core system (marshals) operates at full 20+ fps
  - Only affects display, not data ingestion or client APIs
  - See `docs/EXTERNAL_CLIENT_GUIDE.md` for details

## Cleanup

```bash
# Stop all services
docker compose -f docker-compose.demo.yml down

# Remove all demo images
docker rmi cwru/mri-marshal:latest cwru/robot-marshal:latest \
    cwru/image-streamer:latest cwru/ecg-client:latest \
    cwru/pose-client:latest cwru/viz-client:latest \
    cwru/robot-clients:latest
```

## Support

For issues or questions:
- Review documentation: `docs/EXTERNAL_CLIENT_GUIDE.md`
- Check logs: `docker compose -f docker-compose.demo.yml logs`
- Project repository: https://github.com/cwru-mercis/cwru_data_marshal

---

**Package created:** $(date)
**Requirements:** Docker Engine 20.10+, Docker Compose 2.0+
EOF

# Summary
echo ""
echo "============================================"
echo "Export Complete!"
echo "============================================"
echo ""
echo "Package location: $OUT_DIR"
echo "Package size: $(du -sh "$OUT_DIR" | cut -f1)"
echo ""
echo "Contents:"
echo "  ✓ 7 Docker images ($TOTAL_SIZE)"
echo "  ✓ docker-compose.demo.yml (orchestration)"
echo "  ✓ .env.demo (configuration)"
echo "  ✓ demo-docker.sh (30-second demo)"
echo "  ✓ demo-persistent.sh (keep services running)"
echo "  ✓ API_REFERENCE.md (complete API docs)"
echo "  ✓ EXTERNAL_CLIENT_GUIDE.md (if available)"
echo "  ✓ DEMO_AND_API_EXPORT.md (if available)"
echo "  ✓ README with setup instructions"
echo ""
echo "Usage modes:"
echo "  1. Quick demo: ./demo-docker.sh"
echo "  2. Persistent: ./demo-persistent.sh"
echo "  3. Manual per-service: docker compose -f docker-compose.demo.yml up <service>"
echo ""
echo "Next steps:"
echo "  1. Copy $OUT_DIR to USB drive"
echo "  2. On receiving machine:"
echo "     - Load images: docker load -i images/cwru-demo-images.tar"
echo "     - Run demo: ./demo-docker.sh"
echo "     - Or run services separately to connect your own clients"
echo ""
