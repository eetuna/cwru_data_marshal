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
- Provides HTTP REST API for metadata access
- Broadcasts notifications via WebSocket

**Robot Marshal (Port 8081)**
- Manages robot control data exchange
- Provides file-based communication (like shared memory)
- 10+ predefined data channels (tip_position, planned_motion, etc.)
- JSON-based request/response

---

## MRI Marshal API

**Base URL:** `http://localhost:8080`

### Architecture Overview

The MRI Marshal uses a **metadata-only HTTP API**:
1. **HDF5 Files (SWMR Mode)** - Image data stored in ISMRMRD format
2. **JSONL Files** - Time-series data (ECG, poses) stored as JSON lines
3. **HTTP REST API** - Returns JSON metadata with file paths
4. **WebSocket Server** - Real-time notifications when new data arrives

### CRITICAL: HTTP API Returns Metadata Only

**The MRI Marshal HTTP API does NOT serve binary image data.**

Instead, it returns:
- File paths to .mrd HDF5 files
- Frame indices and dimensions
- Metadata (channels, datatype, total frames)

**To access actual image data:**
1. Call HTTP API to get file path and frame index
2. Use HDF5 library (h5py, HDF5 C++) to read the file directly in SWMR mode
3. Read from `/images/data` dataset

This design is faster than serving binary data over HTTP because:
- No HTTP transfer overhead for large frames
- Client can read multiple frames efficiently
- SWMR allows concurrent reads while writing continues

### SWMR (Single Writer Multiple Readers) Mode

**What is SWMR?**
- HDF5 feature that allows one writer and multiple concurrent readersls
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

---

#### 2. Get Configuration
```http
GET /v1/config
```

**Purpose:** Get marshal configuration

**Response:** JSON with configuration settings

---

#### 3. Get Latest Frame Metadata
```http
GET /v1/mrd/latest
```

**Purpose:** Get metadata about the latest available frame

**Response:**
```json
{
  "data": {
    "path": "/session-data/run_20260124/mrd/demo.mrd",
    "frame_index": 1234,
    "total_frames": 1235
  }
}
```

**Fields:**
- `path` - Path to .mrd HDF5 file (use this with HDF5 library to read frames)
- `frame_index` - Latest frame index (0-based)
- `total_frames` - Total frames written

**Example Usage:**
1. Call this endpoint to get latest frame index and file path
2. Use HDF5 library to open file at `path` in SWMR read mode
3. Read frame at `frame_index` from `/images/data` dataset

---

#### 4. Get Frame Metadata (Specific Frame)
```http
GET /v1/mrd/frame?path=<filepath>&index=<N>
```

**Query Parameters:**
- `path` (optional) - Path to .mrd file (uses latest.json if omitted)
- `index` (optional) - Frame index to query (uses latest if omitted or negative)

**Purpose:** Get metadata for a specific frame

**Response:**
```json
{
  "path": "/session-data/run_20260124/mrd/demo.mrd",
  "frame_index": 100,
  "total_frames": 1235,
  "dims": {
    "x": 64,
    "y": 64,
    "z": 3
  },
  "channels": 1,
  "datatype": "float32"
}
```

**IMPORTANT:** Returns JSON metadata only, NOT binary data. To get actual frame data, use HDF5 library to read the file at the `path` provided.

---

#### 5. Get File Metadata
```http
GET /v1/mrd/ingest?path=<filepath>
```

**Query Parameters:**
- `path` (optional) - Path to .mrd file (uses latest.json if omitted)

**Purpose:** Get metadata about the .mrd file

**Response:**
```json
{
  "path": "/session-data/run_20260124/mrd/demo.mrd",
  "filename": "demo.mrd",
  "size_bytes": 52428800
}
```

**Use Case:** Get file path for direct HDF5 access. Client can then open the HDF5 file at this path using SWMR mode to read all frames.

---

#### 6. Get Historical Frames
```http
GET /v1/mrd/since?ts=<timestamp>&limit=<N>
GET /v1/mrd/since?last=<N>
```

**Query Parameters:**
- `ts` (timestamp) + `limit` (optional) - Get frames after timestamp, up to limit
- `last` (number) - Get last N frames
- Must provide either `ts` OR `last`

**Purpose:** Query historical frame metadata

**Response:**
```json
{
  "frames": [
    {"frame_index": 1230, "timestamp": "..."},
    {"frame_index": 1231, "timestamp": "..."}
  ]
}
```

---

#### 7. Get Current Pose
```http
GET /v1/pose/current
```

**Purpose:** Get latest pose/tracking data

**Response:**
```json
{
  "timestamp": "2026-01-24T18:23:45.123Z",
  "position": [1.2, 3.4, 5.6],
  "orientation": [0.0, 0.0, 0.707, 0.707]
}
```

---

#### 8. Post Pose Update
```http
POST /v1/pose/update
Content-Type: application/json

{
  "position": [1.0, 2.0, 3.0],
  "orientation": [0.0, 0.0, 1.0, 0.0]
}
```

**Purpose:** Submit new pose/tracking data

---

#### 9. Get Latest Biological Signal
```http
GET /v1/bio/latest
```

**Purpose:** Get most recent ECG/biological signal data

**Response:**
```json
{
  "timestamp": "2026-01-24T18:23:45.123Z",
  "heart_rate_bpm": 72,
  "ecg_value": 0.523
}
```

---

#### 10. Post Biological Signal
```http
POST /v1/bio/signal
Content-Type: application/json

{
  "heart_rate_bpm": 75,
  "ecg_value": 0.623
}
```

**Purpose:** Submit ECG/biological signal data

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

### Python: Get Latest MRI Frame (Using HDF5)

```python
import requests
import h5py
import numpy as np

# Step 1: Get file path and latest frame index via HTTP
response = requests.get('http://localhost:8080/v1/mrd/latest')
data = response.json()['data']
file_path = data['path']
frame_idx = data['frame_index']
print(f"Latest frame: {frame_idx} in file: {file_path}")

# Step 2: Open HDF5 file in SWMR read mode
with h5py.File(file_path, 'r', swmr=True) as f:
    # Refresh to see latest data
    dset = f['/images/data']
    dset.refresh()

    # Read specific frame: shape is [frame, channel, z, y, x]
    frame_data = dset[frame_idx, 0, :, :, :]  # channel 0, all slices

    print(f"Frame shape: {frame_data.shape}")
    print(f"Frame dtype: {frame_data.dtype}")

    # Process/display frame_data as needed
    # frame_data is a numpy array of shape [z, y, x]
```

**Why This Approach:**
- HTTP call is fast (only returns ~100 bytes of JSON)
- HDF5 read is direct file access (no HTTP overhead)
- Can read multiple frames efficiently in a loop
- SWMR allows reading while data is being written

---

### Python: Read ALL Frames from File

```python
import requests
import h5py
import numpy as np

# Get file path
response = requests.get('http://localhost:8080/v1/mrd/ingest')
file_path = response.json()['path']

# Open file in SWMR mode and read all frames
with h5py.File(file_path, 'r', swmr=True) as f:
    dset = f['/images/data']
    dset.refresh()

    total_frames = dset.shape[0]
    print(f"Reading {total_frames} frames from {file_path}")

    for i in range(total_frames):
        frame = dset[i, 0, :, :, :]  # channel 0, all slices
        # Process each frame
        print(f"Frame {i}: {frame.shape}")
```

---

### Python: WebSocket Real-time Updates

```python
import asyncio
import websockets
import json
import requests
import h5py

async def listen_mrd():
    uri = "ws://localhost:8090/ws"
    async with websockets.connect(uri) as websocket:
        # Subscribe
        await websocket.send(json.dumps({"subscribe": "mrd"}))

        # Listen for updates
        async for message in websocket:
            notification = json.loads(message)
            frame_idx = notification['frame_index']
            print(f"New frame notification: {frame_idx}")

            # Get file path
            resp = requests.get('http://localhost:8080/v1/mrd/latest')
            file_path = resp.json()['data']['path']

            # Read the new frame via HDF5
            with h5py.File(file_path, 'r', swmr=True) as f:
                dset = f['/images/data']
                dset.refresh()
                frame_data = dset[frame_idx, 0, :, :, :]
                # Process frame_data...

asyncio.run(listen_mrd())
```

---

### Python: Robot Marshal Communication

```python
import requests
import time

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

### C++: Get Latest Frame (Using HDF5)

```cpp
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <H5Cpp.h>
#include <iostream>

// HTTP GET helper (simplified)
std::string http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    std::string response;
    // ... (curl setup code)
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return response;
}

int main() {
    // Get file path and frame index via HTTP
    auto response = http_get("http://localhost:8080/v1/mrd/latest");
    auto json = nlohmann::json::parse(response);

    std::string file_path = json["data"]["path"];
    uint64_t frame_idx = json["data"]["frame_index"];

    std::cout << "File: " << file_path << std::endl;
    std::cout << "Frame: " << frame_idx << std::endl;

    // Open HDF5 file in SWMR mode
    H5::H5File file(file_path, H5F_ACC_RDONLY | H5F_ACC_SWMR_READ);
    H5::DataSet dataset = file.openDataSet("/images/data");

    // Get dimensions
    H5::DataSpace dataspace = dataset.getSpace();
    hsize_t dims[5];
    dataspace.getSimpleExtentDims(dims);

    // Read specific frame [frame_idx, channel=0, :, :, :]
    hsize_t offset[5] = {frame_idx, 0, 0, 0, 0};
    hsize_t count[5] = {1, 1, dims[2], dims[3], dims[4]};
    dataspace.selectHyperslab(H5S_SELECT_SET, count, offset);

    // Allocate buffer
    std::vector<float> buffer(dims[2] * dims[3] * dims[4]);
    H5::DataSpace memspace(3, &dims[2]);
    dataset.read(buffer.data(), H5::PredType::NATIVE_FLOAT, memspace, dataspace);

    std::cout << "Read frame with " << buffer.size() << " pixels" << std::endl;

    return 0;
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
- Via HTTP API to get file path and metadata
- Direct HDF5 read with SWMR mode for actual image data

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
3. On notification → GET `/v1/mrd/latest` for file path
4. Open HDF5 file in SWMR mode → Read frame → Display

### Workflow 2: Batch Processing All Frames

1. GET `/v1/mrd/ingest` to get file path
2. Open HDF5 file in SWMR read mode
3. Read all frames from `/images/data` dataset
4. Process and save results

### Workflow 3: Robot Control Loop

1. Read current position: GET `/read/tip_position_orientation`
2. Read target: GET `/read/desired_planned_motion`
3. Compute control: (your algorithm)
4. Write command: POST `/write/user_input`
5. Sleep 5-10ms, repeat

### Workflow 4: Custom Data Injection

1. POST ECG data: `/v1/bio/signal`
2. POST pose data: `/v1/pose/update`
3. MRI Marshal stores it alongside imaging data

---

## Performance Considerations

- **MRI Frames:** Generated at 20-50 fps (configurable via IMAGE_INTERVAL)
- **Robot Clients:** Run at ~50-80 Hz (limited by HTTP latency)
- **WebSocket:** Near-zero latency for notifications
- **HDF5 SWMR:** Direct file access is faster than HTTP for large binary data

## Security Note

**This demo has NO authentication!** For production:
- Add API keys
- Use HTTPS/WSS
- Implement access control
- Add rate limiting

---

**For additional details, see EXTERNAL_CLIENT_GUIDE.md**
