# CWRU Data Marshal - Architecture Overview

**Two independent HTTP servers coordinating real-time medical imaging and robot control.**

---

## System Diagram

```
                    ┌─────────────────────────────────────────┐
                    │         CWRU Data Marshal System        │
                    └─────────────────────────────────────────┘

┌──────────────────────────────────┐    ┌──────────────────────────────────┐
│       MRI MARSHAL                │    │      ROBOT MARSHAL               │
│       Port 8080                  │    │      Port 8081                   │
└──────────────────────────────────┘    └──────────────────────────────────┘

         ▲  │                                    ▲  │
         │  │                                    │  │
         │  ▼                                    │  ▼
    ┌────────────┐                          ┌────────────┐
    │  4 Clients │                          │ 5 Clients  │
    └────────────┘                          └────────────┘
```

---

## 1. MRI Marshal (Port 8080)

### Architecture Flow

```
┌─────────────┐
│  CLIENT     │
│ (any of 4)  │
└──────┬──────┘
       │
       │ HTTP POST/GET
       ▼
┌─────────────────────┐
│   HTTP Server       │
│   (Port 8080)       │
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│   In-Memory Cache   │    ◄──── Fast! (~100μs reads)
│   • Latest frame    │
│   • Latest ECG      │
│   • Latest pose     │
└──────┬──────────────┘
       │
       ├──────────────┬──────────────┐
       ▼              ▼              ▼
┌──────────┐   ┌──────────┐  ┌──────────┐
│ HDF5 File│   │JSONL Files│ │WebSocket │
│demo.mrd  │   │ *.jsonl   │ │Broadcast │
│(64x64x3) │   │           │ │(optional)│
└──────────┘   └──────────┘  └──────────┘
```

### Clients Connected

| Client | Connects Via | POST Endpoint | GET Endpoint | Purpose |
|--------|-------------|---------------|--------------|---------|
| **Image Streamer** | HTTP | `/v1/mrd/frame` | - | Send MRI frames |
| **ECG Client** | HTTP | `/v1/bio/signal` | - | Send heart signals |
| **Pose Client** | HTTP | `/v1/pose/update` | - | Send position data |
| **Viz Client** | HTTP | - | `/v1/mrd/latest` | Display images |

### Cache → File Flow

```
1. Client POSTs data
       ↓
2. HTTP Server receives
       ↓
3. Update In-Memory Cache (instant)
       ↓
4. Queue background write
       ↓
5. Async write to file (2-5ms)
```

**Why Cache First?** GET requests return instantly from memory (~100μs) without waiting for disk writes.

---

## 2. HTTP Endpoints - MRI Marshal

### POST /v1/mrd/frame

**Purpose:** Upload MRI frame (binary ISMRMRD format)

**Request:**
```
POST http://localhost:8080/v1/mrd/frame
Content-Type: application/octet-stream
X-MRD-Stream: demo

<binary ISMRMRD data ~64KB>
```

**Response:**
```json
{
  "type": "mrd",
  "path": "/session-data/mrd/demo.mrd",
  "stream": "demo",
  "ts": "2026-01-27T12:34:56.789Z",
  "t_ms": 1706356496789,
  "frame_index": 42,
  "flushed": true,
  "element_type": "complex64",
  "dims": {
    "x": 64,
    "y": 64,
    "z": 3,
    "channels": 1
  },
  "size_bytes": 49152,
  "seq": 42
}
```

**What Happens:**
1. Frame written to HDF5 file (`/images/data` dataset)
2. Dataset shape: `[frames, channels, z, y, x]` = `[N, 1, 3, 64, 64]`
3. Cache updated with full metadata (path, frame_index, dims, size, etc.)
4. Metadata written to `index.jsonl`
5. Response includes all image metadata so client knows dimensions before reading HDF5

---

### POST /v1/bio/signal

**Purpose:** Upload ECG or biological signal

**Request:**
```json
POST http://localhost:8080/v1/bio/signal
Content-Type: application/json

{
  "source": "ecg_monitor_1",
  "data": [1.2, 1.3, 1.1, 1.0, 0.9, ...],
  "rate_hz": 250.0
}
```

**JSON Structure:**
- `source` (string): Identifier for signal source
- `data` (array of floats): Signal samples
- `rate_hz` (number): Sampling rate in Hz

**Response:**
```json
{
  "status": "ok"
}
```

**What Happens:**
1. Signal cached in memory
2. Written to `bio.jsonl` as one line:
```json
{"ts":"2026-01-27T12:34:56.789Z","source":"ecg_monitor_1","data":[1.2,1.3,...],"rate_hz":250.0}
```

---

### POST /v1/pose/update

**Purpose:** Upload position and orientation

**Request:**
```json
POST http://localhost:8080/v1/pose/update
Content-Type: application/json

{
  "p": [10.5, 20.3, 5.7],
  "R": [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
}
```

**JSON Structure:**
- `p` (array of 3 floats): Position [x, y, z] in mm
- `R` (array of 9 floats): Rotation matrix (row-major 3x3)

**Response:**
```json
{
  "status": "ok"
}
```

**What Happens:**
1. Pose cached in memory
2. Written to `poses.jsonl`

---

### GET /v1/mrd/latest

**Purpose:** Get metadata about latest MRI frame (includes dimensions, slices, image size)

**Request:**
```
GET http://localhost:8080/v1/mrd/latest
```

**Response:**
```json
{
  "type": "mrd",
  "path": "/session-data/mrd/demo.mrd",
  "stream": "demo",
  "ts": "2026-01-27T12:34:56.789Z",
  "t_ms": 1706356496789,
  "frame_index": 42,
  "flushed": true,
  "element_type": "complex64",
  "dims": {
    "x": 64,
    "y": 64,
    "z": 3,
    "channels": 1
  },
  "size_bytes": 49152,
  "seq": 42
}
```

**JSON Structure:**
- `type` (string): Always "mrd"
- `path` (string): Full path to HDF5 file
- `stream` (string): Stream ID (e.g., "demo")
- `ts` (string): ISO8601 timestamp
- `t_ms` (number): Milliseconds since epoch
- `frame_index` (number): Index in HDF5 dataset
- `flushed` (boolean): Whether data is flushed to disk
- `element_type` (string): Data type ("complex64", "float32", "int16", etc.)
- `dims` (object): Image dimensions
  - `x` (number): Width in pixels (e.g., 64)
  - `y` (number): Height in pixels (e.g., 64)
  - `z` (number): Number of slices (e.g., 3)
  - `channels` (number): Number of coil channels (typically 1)
- `size_bytes` (number): Total frame size in bytes (e.g., 49152 = 64×64×3×1×8 bytes)
- `seq` (number): Sequence number (incremental counter)

**How Client Uses This:**
1. GET `/v1/mrd/latest` → parse full metadata
2. Extract `path`, `frame_index`, and `dims.z` (number of slices)
3. Open HDF5 file at `path` in SWMR read mode
4. Read dataset `[frame_index][0][slice][0:dims.y][0:dims.x]` for each slice
5. Image dimensions: `dims.x × dims.y` pixels per slice, `dims.z` slices total

---

### GET /v1/bio/latest

**Purpose:** Get latest ECG/bio signal

**Request:**
```
GET http://localhost:8080/v1/bio/latest
```

**Response:**
```json
{
  "data": {
    "source": "ecg_monitor_1",
    "data": [1.2, 1.3, 1.1, 1.0, ...],
    "rate_hz": 250.0,
    "timestamp": "2026-01-27T12:34:56.789Z"
  }
}
```

---

### GET /v1/pose/current

**Purpose:** Get current pose

**Request:**
```
GET http://localhost:8080/v1/pose/current
```

**Response:**
```json
{
  "data": {
    "p": [10.5, 20.3, 5.7],
    "R": [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0],
    "timestamp": "2026-01-27T12:34:56.789Z"
  }
}
```

---

## 3. Image Data Structure

### HDF5 File: demo.mrd

```
/images/data
  - Type: Float32
  - Shape: [frames, channels, z, y, x]
  - Example: [1000, 1, 3, 64, 64]
           │    │  │  │   └─ Width: 64 pixels
           │    │  │  └───── Height: 64 pixels
           │    │  └──────── Slices: 3 per frame
           │    └─────────── Channels: 1 (single coil)
           └──────────────── Frames: 1000 total
```

**Size Calculation:**
- 1 frame = 1 × 3 × 64 × 64 × 4 bytes = 49,152 bytes (~48 KB)
- 1000 frames = ~48 MB

**How to Read:**
```python
import h5py

# Open file (SWMR = concurrent read while writer is active)
f = h5py.File('/session-data/mrd/demo.mrd', 'r', swmr=True)
dset = f['/images/data']

# Refresh to see latest writes
dset.refresh()

# Read frame 42, channel 0, slice 1
slice_data = dset[42, 0, 1, :, :]  # Returns 64x64 array
```

---

## 4. Robot Marshal (Port 8081)

### Architecture Flow

```
┌─────────────┐
│  CLIENT     │
│ (any of 5)  │
└──────┬──────┘
       │
       │ HTTP GET/POST
       ▼
┌─────────────────────┐
│   HTTP Server       │
│   (Port 8081)       │
└──────┬──────────────┘
       │
       ▼
┌─────────────────────────────┐
│   Virtual Filesystem        │
│   (13 CircularBuffers)      │
│                             │
│   Each buffer:              │
│   • 1000 entries max        │
│   • In-memory only          │
│   • Thread-safe reads       │
└──────┬──────────────────────┘
       │
       ▼
┌─────────────────────┐
│ Background Writer   │
│ (Async to disk)     │
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│  files/*.json       │
│  (logs)             │
└─────────────────────┘
```

### Clients Connected

| Client | GET Endpoint | POST Endpoint | Purpose |
|--------|-------------|---------------|---------|
| **Catheter Tracking** | `/read/localization_data` | `/write/tip_position_orientation` | Compute tip position from sensors |
| **Controller** | `/read/desired_planned_motion` | `/write/forward_kinematics` | Control robot motors |
| **Planning** | `/read/tip_position_orientation`<br>`/read/user_input`<br>`/read/surface_model_parameters` | `/write/desired_planned_motion` | Plan safe motion paths |
| **Front-End** | `/read/streaming_2D_images`<br>`/read/tip_position_orientation` | `/write/user_input` | User interface |
| **Surface Tracking** | `/read/biological_signals`<br>`/read/streaming_2D_images` | `/write/surface_model_parameters` | Track heart surface |

### How Clients Coordinate

```
Iteration 1:  (Time: 0ms)
  Tracking:  Read sensors → Write tip position
  Planning:  Read tip + user → Write motion plan
  Controller: Read motion → Write motor commands

Iteration 2:  (Time: 20ms)
  Tracking:  Read sensors → Write tip position
  Planning:  Read NEW tip + user → Write NEW motion plan
  Controller: Read NEW motion → Write NEW motor commands

... (repeats at 50-80 Hz)
```

**Key:** All clients read from and write to shared channels. No direct client-to-client communication.

---

## 5. HTTP Endpoints - Robot Marshal

### GET /read/\<filename\>

**Purpose:** Read all recent entries from a data channel

**Request:**
```
GET http://localhost:8081/read/tip_position_orientation
```

**Response:**
```json
{
  "entries": [
    {
      "sent_at": 1706356496123456789,
      "values": [10.5, 20.3, 5.7, 1.0, 0.0, 0.0, 0.0]
    },
    {
      "sent_at": 1706356496143456789,
      "values": [10.6, 20.4, 5.8, 1.0, 0.0, 0.0, 0.0]
    }
  ]
}
```

**JSON Structure:**
- `entries` (array): List of recent entries (up to 1000)
- `sent_at` (number): Timestamp in nanoseconds since epoch
- `values` (array of floats): Channel-specific data

**Values Meaning by Channel:**

| Channel | Values Format |
|---------|--------------|
| `tip_position_orientation` | `[x, y, z, qw, qx, qy, qz]` (position + quaternion) |
| `localization_data` | `[x1, y1, z1, x2, y2, z2, ...]` (N sensor positions) |
| `desired_planned_motion` | `[joint1, joint2, joint3, ...]` (target joint angles) |
| `forward_kinematics` | `[joint1, joint2, joint3, ...]` (current joint angles) |
| `biological_signals` | `[ecg1, ecg2, ecg3, ...]` (ECG samples) |

---

### POST /write/\<filename\>

**Purpose:** Write entry to a data channel

**Request:**
```json
POST http://localhost:8081/write/tip_position_orientation
Content-Type: application/json

{
  "sent_at": 1706356496123456789,
  "values": [10.5, 20.3, 5.7, 1.0, 0.0, 0.0, 0.0]
}
```

**JSON Structure:**
- `sent_at` (number): Timestamp in nanoseconds
- `values` (array): Data array (format depends on channel)

**Response:**
```json
{
  "status": "ok"
}
```

**What Happens:**
1. Entry added to CircularBuffer (in-memory, instant)
2. Queued for background write to `/files/tip_position_orientation.json`
3. If buffer full (>1000 entries), oldest entry is dropped

---

### GET /

**Purpose:** List all available data channels

**Request:**
```
GET http://localhost:8081/
```

**Response:**
```html
<html>
  <body>
    <h1>Robot Marshal - Data Channels</h1>
    <ul>
      <li><a href="/read/localization_data">localization_data</a></li>
      <li><a href="/read/tip_position_orientation">tip_position_orientation</a></li>
      <li><a href="/read/desired_planned_motion">desired_planned_motion</a></li>
      <li><a href="/read/forward_kinematics">forward_kinematics</a></li>
      <li><a href="/read/biological_signals">biological_signals</a></li>
      <li><a href="/read/surface_model_parameters">surface_model_parameters</a></li>
      <li><a href="/read/user_input">user_input</a></li>
      <li><a href="/read/streaming_2D_images">streaming_2D_images</a></li>
      <li><a href="/read/3D_images">3D_images</a></li>
      ... (13 total)
    </ul>
  </body>
</html>
```

---

### POST /v1/mrd/ingest

**Purpose:** Legacy ingest endpoint (alternative to `/v1/mrd/frame`)

**Request:**
```
POST http://localhost:8080/v1/mrd/ingest
Content-Type: application/octet-stream

<binary ISMRMRD data>
```

**Response:**
```json
{
  "type": "mrd",
  "path": "/session-data/mrd/demo.mrd",
  "stream": "demo",
  "ts": "2026-01-27T12:34:56.789Z",
  "frame_index": 42,
  "dims": {"x": 64, "y": 64, "z": 3, "channels": 1},
  "size_bytes": 49152
}
```

**Note:** Functionally identical to `/v1/mrd/frame`, provided for backward compatibility.

---

### GET /v1/mrd/frame

**Purpose:** Get metadata for a specific frame

**Request:**
```
GET http://localhost:8080/v1/mrd/frame?path=/session-data/mrd/demo.mrd&index=42
```

**Query Parameters:**
- `path` (string): Full path to HDF5 file
- `index` (number): Frame index (or omit for latest)

**Response:**
```json
{
  "type": "mrd",
  "path": "/session-data/mrd/demo.mrd",
  "frame_index": 42,
  "dims": {"x": 64, "y": 64, "z": 3, "channels": 1},
  "element_type": "complex64",
  "size_bytes": 49152,
  "ts": "2026-01-27T12:34:56.789Z"
}
```

---

### GET /v1/mrd/ingest

**Purpose:** Get HDF5 file metadata for direct SWMR access

**Request:**
```
GET http://localhost:8080/v1/mrd/ingest?path=/session-data/mrd/demo.mrd
```

**Query Parameters:**
- `path` (string, optional): HDF5 file path (uses latest if omitted)

**Response:**
```json
{
  "path": "/session-data/mrd/demo.mrd",
  "dataset": "/images/data",
  "shape": [1000, 1, 3, 64, 64],
  "dtype": "complex64",
  "total_frames": 1000
}
```

**Use case:** Client wants to batch-read entire HDF5 file

---

### GET /v1/mrd/since

**Purpose:** Query frames by timestamp or get last N frames

**Request (by timestamp):**
```
GET http://localhost:8080/v1/mrd/since?ts=2026-01-27T12:00:00Z&limit=100
```

**Request (last N frames):**
```
GET http://localhost:8080/v1/mrd/since?last=10
```

**Query Parameters:**
- `ts` (string): ISO8601 timestamp (return frames after this time)
- `limit` (number): Max results (default: 100)
- `last` (number): Get last N frames (alternative to ts)

**Response:**
```json
{
  "data": [
    {
      "frame_index": 40,
      "ts": "2026-01-27T12:34:54.789Z",
      "path": "/session-data/mrd/demo.mrd"
    },
    {
      "frame_index": 41,
      "ts": "2026-01-27T12:34:55.789Z",
      "path": "/session-data/mrd/demo.mrd"
    },
    {
      "frame_index": 42,
      "ts": "2026-01-27T12:34:56.789Z",
      "path": "/session-data/mrd/demo.mrd"
    }
  ]
}
```

**Use case:** Replay frames from specific time period, or get recent history

---

### GET /v1/config

**Purpose:** Get marshal configuration

**Request:**
```
GET http://localhost:8080/v1/config
```

**Response:**
```json
{
  "data_dir": "/session-data",
  "default_stream": "demo",
  "http_port": 8080,
  "ws_port": 8090
}
```

---

## 6. MRI Response Fields Explained

### Element Types

The `element_type` field indicates the data format stored in HDF5:

| Type | Description | Bytes per Element |
|------|-------------|-------------------|
| `complex64` | Complex float (real + imaginary) | 8 bytes |
| `float32` | Single-precision float | 4 bytes |
| `int16` | Signed 16-bit integer | 2 bytes |
| `uint16` | Unsigned 16-bit integer | 2 bytes |

### Size Calculation

```
size_bytes = x × y × z × channels × bytes_per_element

Example (complex64):
  64 × 64 × 3 × 1 × 8 = 49,152 bytes (~48 KB per frame)
```

### Slice Number

The `dims.z` field tells you how many slices are in the frame. To read a specific slice:

```python
# Read middle slice
slice_index = dims["z"] // 2  # e.g., 3 // 2 = 1

# HDF5 read
slice_data = dataset[frame_index][0][slice_index][:][:]
# Returns array of shape [dims.y, dims.x] = [64, 64]
```

### Stream ID

The `stream` field allows multiple independent MRI scanners or sequences to write to separate files. Default is "demo".

### SWMR (Single Writer Multiple Readers)

**What is SWMR?**
- HDF5 file access mode that allows 1 writer + multiple readers simultaneously
- Writer can append new frames while readers access existing data
- No file locking or blocking between writer and readers

**How it works:**
```
Writer (MRI Marshal):
  1. Opens file with SWMR_WRITE flag
  2. Appends new frame to dataset
  3. Flushes to disk (critical!)
  4. Updates metadata

Reader (Viz Client):
  1. Opens file with SWMR_READ flag
  2. Calls dataset.refresh() to see new data
  3. Reads frame data
  4. Never blocks the writer
```

**Benefits:**
- **No copying**: Readers access file directly, no HTTP transfer of image data
- **Scalable**: 10+ readers can access same file without slowing writer
- **Fast**: Reader sees new frame within 1-2ms after write
- **Safe**: Readers always see consistent data (no partial frames)

**Requirements:**
- HDF5 1.10+
- Writer must flush after each append
- Readers must call refresh() before reading new dimensions

**Example:**
```python
# Writer (MRI Marshal does this internally)
f = h5py.File('demo.mrd', 'w', libver='latest')
f.swmr_mode = True  # Enable SWMR
dset = f.create_dataset('data', shape=(0,1,3,64,64), maxshape=(None,1,3,64,64))
dset.resize((1,1,3,64,64))  # Add frame
dset[0] = frame_data
dset.flush()  # Critical!

# Reader (Viz Client does this)
f = h5py.File('demo.mrd', 'r', swmr=True)
dset = f['data']
dset.refresh()  # See latest writes
frame = dset[0]  # Read without blocking writer
```

---

## 7. Data Channel Structures (Robot Marshal)

### Channel: tip_position_orientation

**Values:** `[x, y, z, qw, qx, qy, qz]` (7 floats)
- Position (mm): x, y, z
- Orientation (quaternion): qw, qx, qy, qz

**Example:**
```json
{
  "sent_at": 1706356496123456789,
  "values": [10.5, 20.3, 5.7, 1.0, 0.0, 0.0, 0.0]
}
```
Meaning: Tip at (10.5, 20.3, 5.7) mm, no rotation

---

### Channel: localization_data

**Values:** `[x1, y1, z1, x2, y2, z2, ..., xN, yN, zN]` (3N floats for N sensors)

**Example:**
```json
{
  "sent_at": 1706356496123456789,
  "values": [5.0, 10.0, 2.0, 15.0, 20.0, 3.0, 25.0, 30.0, 4.0]
}
```
Meaning: 3 sensors at positions (5,10,2), (15,20,3), (25,30,4)

---

### Channel: desired_planned_motion

**Values:** `[joint1_target, joint2_target, ..., jointN_target]` (N floats)

**Example:**
```json
{
  "sent_at": 1706356496123456789,
  "values": [0.5, 0.3, 0.1, 0.2]
}
```
Meaning: Target angles for 4 joints (in radians)

---

### Channel: biological_signals

**Values:** `[sample1, sample2, ..., sampleN]` (N floats, typically 250-1000)

**Example:**
```json
{
  "sent_at": 1706356496123456789,
  "values": [1.2, 1.3, 1.1, 1.0, 0.9, 0.8, ...]
}
```
Meaning: ECG samples (typically 250 Hz × 1 second = 250 samples)

---

## 8. Complete Example Workflows

### Workflow 1: MRI Imaging Pipeline

```
1. Image Streamer generates frame
   ↓
2. POST /v1/mrd/frame (binary ISMRMRD, ~48KB)
   ↓
3. MRI Marshal:
   - Writes to HDF5: /images/data[frame_idx][0][0:3][0:64][0:64]
   - Updates cache with full metadata
   - Queues JSONL write
   ↓
4. Viz Client polls GET /v1/mrd/latest every 10ms
   ↓
5. Response with full metadata:
   {
     "frame_index": 42,
     "path": "demo.mrd",
     "dims": {"x": 64, "y": 64, "z": 3},
     "element_type": "complex64",
     "size_bytes": 49152
   }
   ↓
6. Viz Client now knows:
   - Image is 64×64 pixels
   - 3 slices available (z=0, 1, 2)
   - Data type is complex64
   ↓
7. Viz Client opens demo.mrd (SWMR read mode)
   ↓
8. Reads middle slice: dataset[42][0][1][0:64][0:64]
   ↓
9. Displays 64×64 grayscale image (slice 2 of 3)
```

---

### Workflow 2: Robot Control Loop

```
Time: 0ms

1. Catheter Tracking:
   GET /read/localization_data
   → {"entries": [{"values": [5,10,2, 15,20,3, 25,30,4]}]}
   → Compute: tip = (18.3, 20.1, 3.5)
   POST /write/tip_position_orientation
   → {"values": [18.3, 20.1, 3.5, 1, 0, 0, 0]}

2. Planning:
   GET /read/tip_position_orientation
   → {"entries": [{"values": [18.3, 20.1, 3.5, 1, 0, 0, 0]}]}
   GET /read/user_input
   → {"entries": [{"values": [1, 25.0, 30.0, 5.0]}]}  (target position)
   → Plan path: joints = [0.5, 0.3, 0.1, 0.2]
   POST /write/desired_planned_motion
   → {"values": [0.5, 0.3, 0.1, 0.2]}

3. Controller:
   GET /read/desired_planned_motion
   → {"entries": [{"values": [0.5, 0.3, 0.1, 0.2]}]}
   → Compute motor commands
   POST /write/forward_kinematics
   → {"values": [0.48, 0.29, 0.09, 0.19]}

Time: 20ms (repeat all steps with new sensor data)
```

---

## 8. Performance & Timing

### MRI Marshal
- **POST /v1/mrd/frame:** ~1ms (includes HDF5 write)
- **GET /v1/mrd/latest:** ~100μs (cache read)
- **Frame rate:** 20-50 FPS
- **Viz client polling:** 100 Hz (every 10ms)
- **End-to-end latency:** 50-100ms

### Robot Marshal
- **POST /write/<file>:** ~300μs (CircularBuffer write)
- **GET /read/<file>:** ~200μs (CircularBuffer read)
- **Control loop rate:** 50-80 Hz (12-20ms per iteration)
- **Background disk write:** ~5ms (async, non-blocking)

---

## 9. File Formats

### HDF5 File (demo.mrd)
- **Format:** HDF5 with SWMR (Single Writer Multiple Readers)
- **Dataset:** `/images/data`
- **Type:** Float32
- **Dimensions:** `[frames, channels, z, y, x]`
- **Typical size:** 1000 frames = ~48 MB

### JSONL Files (*.jsonl)
- **Format:** JSON Lines (one JSON object per line)
- **Files:** `index.jsonl`, `bio.jsonl`, `poses.jsonl`
- **Example line:**
```json
{"ts":"2026-01-27T12:34:56.789Z","frame_index":42,"path":"demo.mrd"}
```

### Robot Log Files (files/*.json)
- **Format:** Standard JSON array
- **Example:**
```json
[
  {"sent_at": 1706356496123456789, "values": [10.5, 20.3, 5.7]},
  {"sent_at": 1706356496143456789, "values": [10.6, 20.4, 5.8]}
]
```

---

## 9. Key Technologies Explained

### HDF5 (Hierarchical Data Format 5)

**What is it?**
- Binary file format optimized for large scientific datasets
- Organizes data in hierarchical structure (like filesystem)
- Supports compression, chunking, and parallel I/O

**Why use it for MRI?**
- **Efficient**: Store 1000s of frames in single file
- **Fast random access**: Jump to any frame instantly
- **SWMR support**: Concurrent read/write
- **Standard**: Used by major MRI vendors and research institutions

**Structure in this system:**
```
demo.mrd (HDF5 file)
├── /images/
│   └── data (dataset)
│       Shape: [frames, channels, z, y, x]
│       Type: complex64 or float32
│       Chunks: (1, 1, 3, 64, 64)
```

### ISMRMRD (MRI Raw Data Standard)

**What is it?**
- Standard format for raw MRI k-space data
- Developed by National Institutes of Health (NIH)
- Includes acquisition parameters, trajectories, and metadata

**Why use it?**
- **Vendor-neutral**: Works with Siemens, GE, Philips scanners
- **Complete**: Contains all info needed for reconstruction
- **Flexible**: Supports arbitrary trajectories and sequences

**What this system does:**
- Receives ISMRMRD binary in POST body
- Extracts image data and dimensions
- Stores in HDF5 with metadata preserved

### CircularBuffer (Robot Marshal)

**What is it?**
- Fixed-size ring buffer (1000 entries max)
- Lock-free reads for high performance
- Oldest entry discarded when full

**Why use it?**
- **Bounded memory**: Never grows beyond 1000 entries
- **Fast**: Sub-microsecond read/write
- **Recent data**: Keeps last 1000 samples (at 80 Hz = 12.5 seconds)

**How it works:**
```
Buffer: [0] [1] [2] ... [999]
Write pointer: 42
Read: Returns entries [0..42]

After 1000 writes:
Write pointer: 1042 → wraps to 42
Oldest entry (43) is overwritten
Read: Returns last 1000 entries [43..1042]
```

### Blackboard Pattern (Robot Marshal)

**What is it?**
- Coordination pattern where clients communicate via shared state
- No direct client-to-client communication
- Marshal acts as "blackboard" where all data is posted

**Why use it?**
- **Decoupled**: Clients don't need to know about each other
- **Flexible**: Easy to add/remove clients
- **Debuggable**: All data visible in one place

**Example:**
```
Planning writes motion plan → Blackboard
Controller reads motion plan ← Blackboard
(Planning and Controller never directly communicate)
```

### Async I/O (Both Marshals)

**What is it?**
- Background threads handle disk writes
- Main thread never blocks waiting for disk
- Queue-based architecture

**Why use it?**
- **Responsive**: API responds in <1ms even during disk write
- **Smooth**: No frame drops due to disk latency
- **Safe**: Queue ensures all data eventually written

**How it works:**
```
HTTP Thread:
  1. Receive POST
  2. Update cache (instant)
  3. Queue write request
  4. Return 200 OK

Background Thread:
  1. Wait for queue
  2. Dequeue write
  3. Write to disk (5-10ms)
  4. Loop
```

---

## Summary

**MRI Marshal:**
- 4 clients → HTTP POST/GET → Cache → HDF5/JSONL files
- Image dimensions: 64×64 pixels, 3 slices, ~48KB per frame
- Fast reads from cache (~100μs), async writes to disk
- **SWMR**: Multiple readers access HDF5 while writer appends
- **ISMRMRD**: Standard MRI raw data format

**Robot Marshal:**
- 5 clients → HTTP POST/GET → 13 CircularBuffers → files/*.json
- Data: 7-float arrays (positions, orientations, joint angles)
- All coordination via shared in-memory channels
- **Blackboard**: Clients coordinate via shared state
- **CircularBuffer**: Fixed 1000-entry ring buffers per channel

**Both:**
- Simple HTTP REST APIs
- JSON request/response
- Real-time operation (20-80 Hz)
- **Async I/O**: Background disk writes don't block API
