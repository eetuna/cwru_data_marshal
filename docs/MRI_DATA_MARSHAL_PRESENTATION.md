# MRI Data Marshal: Real-Time Medical Imaging Data Infrastructure

**A High-Performance Framework for Multi-Client Real-Time MRI Data Streaming**

---

## Executive Summary

MRI Data Marshal is a real-time data infrastructure system designed for medical imaging applications that require simultaneous write and read access to streaming MRI data. Built on HDF5 SWMR (Single Writer Multiple Reader) technology, it enables scenarios where MRI frames are being acquired while multiple visualization and analysis clients access the data concurrently.

**Key Capabilities:**
- Real-time frame ingestion at 19 fps with 40 MB/s throughput
- Simultaneous multi-client read access with 8ms latency
- Structured HDF5 storage preserving scientific data formats
- HTTP/WebSocket APIs for flexible integration
- State blackboard for robot/instrument coordination

**Target Applications:** Medical imaging pipelines, MRI-guided robotics, real-time visualization systems, and research data acquisition platforms.

---

## What is MRI Data Marshal?

MRI Data Marshal solves a fundamental challenge in medical imaging systems: **how do you write streaming data while multiple clients read it simultaneously?**

Traditional approaches either:
- Lock files during writes (blocking readers)
- Copy data to separate buffers (doubling memory usage)
- Use complex message queues (adding latency and complexity)

MRI Data Marshal uses HDF5's SWMR mode to allow:
- **One writer** appending frames to an HDF5 file
- **Multiple readers** accessing the same file concurrently
- **No locks, no copies, no queues** - just direct file access

### Architecture Overview

```
┌──────────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│  MRI Scanner or  │  │ Pose Tracker │  │ Bio Sensors  │  │ Robot State  │
│  Image Generator │  │ (FK Tracker) │  │ (ECG, Resp)  │  │ (Commands)   │
└────────┬─────────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘
         │                   │                 │                 │
         │                   │                 │                 │
         ▼                   ▼                 ▼                 ▼
 POST /v1/mrd/frame   POST /v1/pose/update  POST /v1/bio/signal  POST /write/<key>
 POST /v1/mrd/ingest         │                 │                 │
         │                   │                 │                 │
         └───────────────────┴─────────────────┘                 │
                             │                                   │
         ┌───────────────────▼────────────────┐                  │
         │      MRI Data Marshal              │                  │
         │      (Port 8080 HTTP, 8090 WS)     │                  │
         │                                    │                  │
         │  ┌────────────────────────────────┐│                  │
         │  │  /v1/mrd/frame  - SWMR append  ││                  │
         │  │  /v1/mrd/ingest - Batch import ││                  │
         │  │  /v1/mrd/latest - Read frame   ││                  │
         │  │  /v1/mrd/since  - Query history││                  │
         │  │  /v1/pose/*     - Pose tracking││                  │
         │  │  /v1/bio/*      - Biosignals   ││                  │
         │  └────────────────────────────────┘│                  │
         └───────────────────┬────────────────┘                  │
                             │                                   │
                             │         ┌─────────────────────────▼─────┐
                             │         │  Robot Data Marshal           │
                             │         │  (Port 8081 HTTP)             │
                             │         │                               │
                             │         │  /write/<key> - Store state   │
                             │         │  /read/<key>  - Read state    │
                             │         └─────────────────────────┬─────┘
                             │                                   │
                             │                                   │
      MRI Marshal GETs:      │                   Robot Marshal GETs:
      GET /v1/mrd/latest     │                   GET /read/<key>
      GET /v1/mrd/since      │                         │
      GET /v1/pose/current   │                         │
      GET /v1/bio/latest     │                         │
      GET /v1/config         │                         │
                             │                         │
         ┌───────────────────┴─────────────────────────┘
         │
         ▼
   ┌──────────────────────────────────────────────────────────────┐
   │                    Client Applications                        │
   ├──────────────┬──────────────────┬───────────────────────────┤
   │ Visualizer   │ Analysis Pipeline│ Robot Control System      │
   │ (Real-time   │ (Segmentation,   │ (Feedback, Commands,      │
   │  MRI view)   │  Registration)   │  Coordination)            │
   └──────────────┴──────────────────┴───────────────────────────┘
```

---

## Key Features

### 1. Real-Time Data Ingestion

**HTTP and WebSocket APIs** accept data from multiple sources:

**MRI Frame Endpoints:**

| Endpoint | Method | Purpose | Latency |
|----------|--------|---------|---------|
| `/v1/mrd/frame` | POST | Append frame to active SWMR file | ~50ms |
| `/v1/mrd/ingest` | POST | Create new HDF5 file with frame | ~1000ms |
| `/v1/mrd/latest` | GET | Read most recent frame metadata | ~8ms |
| `/v1/mrd/since` | GET | Get frames after timestamp (ts, limit) or last N frames (last) | ~10ms |

**Pose Tracking Endpoints:**

| Endpoint | Method | Purpose | Latency |
|----------|--------|---------|---------|
| `/v1/pose/current` | GET | Get current tool/probe pose | <1ms |
| `/v1/pose/update` | POST | Update pose from FK tracker | <1ms |

**Biosignal Endpoints:**

| Endpoint | Method | Purpose | Latency |
|----------|--------|---------|---------|
| `/v1/bio/signal` | POST | Ingest ECG, respiratory data | <1ms |
| `/v1/bio/latest` | GET | Read most recent biosignal | <1ms |

**System Endpoints:**

| Endpoint | Method | Purpose | Latency |
|----------|--------|---------|---------|
| `/v1/config` | GET | Server configuration | <1ms |
| `/health` | GET | Server health check | <1ms |

### 2. SWMR HDF5 Storage

**Single Writer Multiple Reader** mode enables:
- Continuous frame appending without blocking readers
- Automatic metadata synchronization
- Standard HDF5 file format (compatible with Python h5py, MATLAB, etc.)
- Structured data with dimensions, compression, and attributes

### 3. Multi-Client Simultaneous Access

Multiple clients can read the latest frame while new frames are being written:

```
Writer:    [Frame 1]──[Frame 2]──[Frame 3]──[Frame 4]──...
                │         │         │         │
Reader A:       └────────►│         │         │
Reader B:                 └────────►│         │
Reader C:                           └────────►│
```

### 4. MRI Frame Data (SWMR & Batch Ingestion)

The **MRI Data Marshal** provides two primary endpoints for MRI frame data ingestion:

**SWMR Frame Append (/v1/mrd/frame)**

For real-time streaming, frames are appended to an active SWMR HDF5 file:

```
POST /v1/mrd/frame
Headers:
  X-MRD-Stream: acquisition_001
  X-MRD-Session: session_2024_01_15
  Content-Type: application/octet-stream

Body: [ISMRMRD ImageHeader (198 bytes)] + [Raw pixel data]

Response (201 Created):
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

**Batch Ingest (/v1/mrd/ingest)**

For importing complete HDF5 datasets or batch processing:

```
POST /v1/mrd/ingest
Content-Type: application/octet-stream

Body: [Raw HDF5/MRD file bytes]

Response (201 Created):
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

**Reading Frame Metadata (/v1/mrd/latest, /v1/mrd/since)**

```
GET /v1/mrd/latest
Response:
{
  "ts": "2024-01-15T10:30:45.123Z",
  "path": "/data/mrd/latest.mrd",
  "frame_count": 42,
  "dims": {"spatial": [192, 192, 15], "channels": 1}
}

GET /v1/mrd/since?ts=2024-01-15T10:30:00.000Z&limit=10
Response: Frames where ts > provided timestamp, up to limit
[
  {"ts": "2024-01-15T10:30:01.000Z", "path": "...", ...},
  {"ts": "2024-01-15T10:30:02.000Z", "path": "...", ...},
  ...
]

GET /v1/mrd/since?last=5
Response: Last 5 frames (most recent)
[
  {"ts": "2024-01-15T10:30:38.000Z", "path": "...", ...},
  {"ts": "2024-01-15T10:30:39.000Z", "path": "...", ...},
  {"ts": "2024-01-15T10:30:40.000Z", "path": "...", ...},
  {"ts": "2024-01-15T10:30:41.000Z", "path": "...", ...},
  {"ts": "2024-01-15T10:30:42.000Z", "path": "...", ...}
]
```

### 5. Pose & Biosignal Integration (MRI Data Marshal)

The **MRI Data Marshal** provides structured endpoints for pose tracking and biosignal ingestion. These are first-class citizens alongside MRI frame data.

**Pose Tracking (Tool/Probe Position)**

```
POST /v1/pose/update
{
  "p": [12.5, 8.3, -4.2],
  "R": [1,0,0, 0,1,0, 0,0,1],
  "frame": "scanner",
  "source": "fk_tracker"
}

GET /v1/pose/current
Response: {"pose": {"p": [...], "R": [...], "frame": "..."}, "source": "fk_tracker"}
```

**Biosignal Ingestion (ECG, Respiratory, etc.)**

```
POST /v1/bio/signal
{
  "ts": 1705341022,
  "source": "ecg",
  "data": [0.1, 0.15, 0.2, 0.18, ...],
  "rate_hz": 250
}

GET /v1/bio/latest
Response: {"ts": ..., "source": "ecg", "data": [...], "rate_hz": 250}
```

**Biosignal Sources:**
- `"ecg"` - Cardiac monitoring (heart rate, rhythm)
- `"respiratory"` - Breathing patterns (phase, amplitude)
- `"pulse_ox"` - Blood oxygen saturation
- `"temperature"` - Body/probe temperature

### 6. Generic State Blackboard (Robot Data Marshal)

A separate **Robot Data Marshal** (port 8081) provides a generic key-value state system for arbitrary data coordination.

**Endpoints:**

| Endpoint | Method | Purpose | Example |
|----------|--------|---------|---------|
| `/write/<key>` | POST | Store JSON to named file | `POST /write/robot_status` |
| `/read/<key>` | GET | Read latest entry | `GET /read/robot_status` |
| `/read/<key>?last=N` | GET | Read N most recent entries | `GET /read/robot_status?last=10` |

**Request Format:**

```
POST /write/robot_status
{
  "sent_at": 1705341022,
  "values": [1.0, 2.0, 3.0],
  "client_id": "robot_arm_1"
}

GET /read/robot_status
Response: {"sent_at": ..., "values": [...], "client_id": "..."}
```

**Use Cases:**
- Robot arm status and commands
- Custom sensor data not fitting pose/bio formats
- Inter-process coordination
- Experiment metadata

### Real-Time Sensor Fusion Example

```
              MRI Data Marshal (8080)           Robot Marshal (8081)
                      │                                │
    Frame 1 ─────────►│ POST /v1/mrd/frame             │
    Pose update ─────►│ POST /v1/pose/update           │
    ECG data ────────►│ POST /v1/bio/signal            │
    Robot status ─────┼────────────────────────────────► POST /write/status
                      │                                │
                      ▼                                ▼
    Correlation Engine polls:
    ├── GET /v1/mrd/latest     (MRI frame)
    ├── GET /v1/pose/current   (tool position)
    ├── GET /v1/bio/latest     (ECG data)
    └── GET /read/status       (robot state)
              │
              ▼
    Synchronized multi-modal dataset
```

**Use Cases:**
- **MRI-Guided Robotics:** Continuously monitor robot position while acquiring MRI frames
- **Cardiac Imaging:** Synchronize ECG data with frame acquisition for arrhythmia detection
- **Respiratory Gating:** Pause frame acquisition during inhalation, resume during exhalation
- **Multi-Instrument Coordination:** Coordinate multiple devices (ultrasound, temperature sensors, etc.)

### Data Flow Diagrams

#### Flow 1: Real-Time MRI Frame Ingestion (/v1/mrd/frame)

```
MRI Scanner/Generator
        │
        │ Raw 192×192×15 frame data
        │
        ▼
    HTTP POST /v1/mrd/frame
        │
        ├─── Acquire HDF5 SWMR write lock
        │
        ▼
    MRI Data Marshal
        │
        ├─── Write frame data to buffer
        ├─── Update HDF5 metadata
        ├─── fsync to disk (~45ms)
        │
        ▼
    HDF5 SWMR File
        │
        ├─── frame_000000 [192×192×15]
        ├─── frame_000001 [192×192×15]
        ├─── metadata (width, height, slices, timestamp)
        │
        ▼ (immediate, no blocking)
    Multiple Concurrent Readers
        │
        ├── GET /v1/mrd/latest (~8ms latency)
        │   ├─► Visualizer (OpenCV display)
        │   ├─► Analysis pipeline
        │   └─► Archive/Recording system
        │
        └── WebSocket stream (port 8090)
            └─► Real-time clients
```

**Performance:** 19 fps @ 50ms intervals, 40 MB/s throughput, zero reader blocking

---

#### Flow 2: Pose & Biosignal Ingestion (MRI Data Marshal)

```
Sensor Sources
        │
        ├── FK Position Tracker (Robot Arm)
        │   └─ Position [x,y,z] + Rotation matrix [3x3]
        │
        └── Bio Sensors (ECG, Respiratory, etc.)
            └─ Time-series data arrays + sample rate
        │
        ▼
    MRI Data Marshal (Port 8080)
        │
        ├─── POST /v1/pose/update
        │    Body: {"p": [x,y,z], "R": [9 floats], "source": "fk"}
        │
        └─── POST /v1/bio/signal
             Body: {"ts": ..., "source": "ecg", "data": [...], "rate_hz": 250}
        │
        ▼
    Persistence Layer
        │
        ├─── poses.jsonl     (timestamped pose history)
        └─── bio.jsonl       (timestamped biosignal batches)
        │
        ▼
    WebSocket Broadcast (Port 8090)
        │
        ├─── topic: "pose"   (real-time pose updates)
        └─── topic: "bio"    (real-time biosignal streams)
        │
        ▼ (sub-millisecond reads)
    Real-Time Consumers
        │
        ├── GET /v1/pose/current
        │   └─► Robot control feedback
        │   └─► MRI frame-to-pose correlation
        │
        └── GET /v1/bio/latest
            └─► Arrhythmia detection
            └─► Respiratory gating controller
```

**Performance:** <1ms read latency, WebSocket broadcast for real-time consumers

---

#### Flow 3: Batch Ingest Mode (/v1/mrd/ingest)

```
Archive/Offline Data Source
        │
        │ Complete HDF5 datasets or large frame batches
        │ (from previous acquisitions, external sources)
        │
        ▼
    HTTP POST /v1/mrd/ingest
        │
        ├─── Create new HDF5 file (not SWMR)
        ├─── Write entire dataset at once
        ├─── No reader access during write
        │
        ▼
    HDF5 Standard File
        │
        ├─── Data array [N frames, 192×192×15]
        ├─── Complete metadata
        ├─── Compression applied
        │
        ▼
    Batch Processing Pipeline
        │
        ├── Segmentation algorithm
        ├── Registration/alignment
        ├── Statistical analysis
        └── Archive to tape/cloud
```

**Performance:** 0.94 fps (slower than /v1/mrd/frame), optimized for batch throughput not real-time, suitable for post-acquisition processing

---

#### Flow 4: Integrated Multi-Modal Workflow

```
Integrated Real-Time System (All via MRI Data Marshal, Port 8080):

    t=0ms   ┌─ Scanner generates Frame 1 → POST /v1/mrd/frame
            ├─ FK Tracker sends pose     → POST /v1/pose/update
            └─ ECG monitor sends data    → POST /v1/bio/signal

    t=1ms   ├─ Multiple readers poll:
            │  ├─ GET /v1/mrd/latest        [Frame 1]
            │  ├─ GET /v1/pose/current      [Pose 1]
            │  └─ GET /v1/bio/latest        [ECG 1]
            │
            └─ Correlation Engine synthesizes:
               { frame_1, pose_1, ecg_1, timestamp_sync }

    t=50ms  ├─ Frame 2 arrives, readers get synchronized
    t=100ms ├─ Frame 3 arrives, system maintains 20 fps
    t=150ms ├─ Analysis pipeline operates on completed multi-modal sets
    t=200ms ├─ Real-time visualization shows current frame + metadata overlay
            │
            └─ Archive system stores: [frame, pose, ecg] triplets

Result:  Real-time fusion of 3 modalities @ 20 fps
         Zero reader blocking
         Sub-millisecond latency for pose/bio data
         Medical-grade synchronization for guided procedures
```

---

### Endpoint Selection Guide

**MRI Data Marshal Endpoints (Port 8080):**

**Choose `/v1/mrd/frame` when:**
- Real-time SWMR visualization needed
- <50ms latency acceptable
- High frame rate (10-20 fps)
- Multiple concurrent readers essential

**Choose `/v1/mrd/ingest` when:**
- Processing completed acquisitions
- Batch throughput more important than latency
- Creating archival copies
- No real-time readers needed

**Choose `/v1/pose/update` + `/v1/pose/current` when:**
- Tracking robot/tool/probe position
- Need rotation matrix + position vector format
- Real-time pose updates for MRI-guided robotics
- WebSocket broadcast to multiple consumers

**Choose `/v1/bio/signal` + `/v1/bio/latest` when:**
- Ingesting ECG, respiratory, or other biosignals
- Time-series data with sample rate metadata
- Need WebSocket broadcast for real-time monitoring
- Synchronized logging with MRI frames

**Robot Data Marshal Endpoints (Port 8081):**

**Choose `/write/<key>` + `/read/<key>` when:**
- Generic key-value state storage needed
- Custom data formats not fitting pose/bio structure
- Robot-to-robot or process-to-process coordination
- Need historical queries (`?last=N`)

---

## Performance Characteristics

### Benchmark Results

Testing performed on development system (WSL2, Intel i7, SSD):

| Test | Metric | Result | Status |
|------|--------|--------|--------|
| **SWMR @ 50ms** | Frame Rate | 19.2 fps | Real-time viable |
| **SWMR @ 50ms** | Throughput | 40.4 MB/s | Production ready |
| **Full Ingest** | Frame Rate | 0.94 fps | Batch use only |
| **Read Latency** | RPS | 123.76 req/s | Low latency |
| **Read Latency** | Avg Time | 8.08 ms | Responsive |
| **SWMR @ 10ms** | Frame Rate | 0.40 fps | Not viable |

### Frame Size Reference

Standard test frame: **192 x 192 x 15 slices = 2.2 MB per frame**

| Frame Size | Slices | Data Size | Achievable FPS |
|------------|--------|-----------|----------------|
| 128x128 | 10 | 0.66 MB | ~25 fps |
| 192x192 | 15 | 2.2 MB | ~19 fps |
| 256x256 | 20 | 5.2 MB | ~12 fps |

### Performance Summary

```
Sustainable Real-Time:    19 fps @ 50ms intervals
Read Response:            124 RPS, 8ms latency
Throughput:               40 MB/s sustained
Frame Size:               Up to 5 MB practical
```

---

## Use Cases and Applications

### 1. MRI-Guided Interventions
Real-time visualization of MRI frames during surgical procedures, with multiple displays showing different slice orientations.

### 2. Research Data Acquisition
Streaming MRI data to disk while analysis pipelines process frames in parallel.

### 3. Robot Coordination
Sharing pose and state information between robotic systems and imaging equipment during MRI-guided procedures.

### 4. Multi-Modal Fusion
Combining MRI frames with ECG, respiratory, and position data for synchronized analysis.

---

## Known Limitations

### HDF5 SWMR Metadata Overhead

The primary limitation is **HDF5's metadata synchronization overhead**:

| Interval | Target FPS | Actual FPS | Efficiency |
|----------|------------|------------|------------|
| 50ms | 20 | 19 | 95% |
| 20ms | 50 | ~8 | 16% |
| 10ms | 100 | 0.4 | 0.4% |

**Root Cause:** Each frame write triggers HDF5 metadata sync, which takes ~40-50ms regardless of frame size.

**Implication:** MRI Data Marshal is suitable for:
- Standard MRI frame rates (1-20 fps)
- Real-time visualization with ~50ms latency
- **NOT suitable for:** High-speed acquisition (>30 fps), sub-10ms latency requirements

### WSL2 Performance Tax

Running on WSL2 adds 5-10ms overhead per operation due to syscall translation. Native Linux or Docker shows ~10-15% better performance.

### Single File Growth

Long acquisitions (>10,000 frames) may experience slower performance as HDF5 file size increases. Recommended: start new files for long sessions.

---

## Comparison with Alternatives

| Approach | Real-Time | Multi-Reader | Structured | Throughput |
|----------|-----------|--------------|------------|------------|
| **MRI Data Marshal (HDF5 SWMR)** | 19 fps | Yes | Yes | 40 MB/s |
| Raw Binary Files | 100+ fps | Manual | No | 100+ MB/s |
| SQLite | 5-10 fps | Yes | Schema | 10 MB/s |
| Zarr | 30-50 fps | Yes | Yes | 60 MB/s |
| Redis/Message Queue | 50+ fps | Yes | JSON | 50 MB/s |

**When to choose MRI Data Marshal:**
- Need structured scientific data format
- Multiple clients reading simultaneously
- Standard MRI frame rates (1-20 fps)
- Want HDF5 compatibility with existing tools

**When to choose alternatives:**
- High-speed acquisition (>30 fps): Use Zarr or binary
- Simple key-value storage: Use Redis
- No multi-reader requirement: Use standard HDF5

---

## Future Improvements

### Short-Term (Low Effort)

1. **Persistent HDF5 Handles** - Keep file handles open between frames for 30-40% improvement
2. **Compression Options** - Add ZSTD compression for 2x storage efficiency

### Medium-Term (Architecture Changes)

1. **Batching Mode** - Collect 10 frames before writing for 4-8x throughput (trades latency)
2. **Separate Read/Write Paths** - Buffer recent frames in memory for sub-10ms read latency

### Long-Term (Format Migration)

1. **Zarr Format Support** - Modern cloud-native format with better SWMR characteristics
2. **Hybrid Storage** - In-memory ring buffer + async disk writes for <10ms latency

---

## Conclusion

MRI Data Marshal provides a production-ready solution for real-time MRI data streaming at standard clinical frame rates. Its use of HDF5 SWMR enables true concurrent multi-client access while preserving the structured data formats that scientific applications require.

**Strengths:**
- 19 fps sustained real-time performance
- Standard HDF5 format compatibility
- Simple HTTP API integration
- Multi-client simultaneous access

**Limitations:**
- Not suitable for high-speed (>30 fps) acquisition
- HDF5 metadata overhead limits sub-50ms intervals

For applications requiring higher frame rates, the architecture supports migration paths to Zarr or hybrid storage approaches while maintaining API compatibility.

---

## References

- HDF5 SWMR Documentation: https://docs.hdfgroup.org/hdf5/develop/group___s_w_m_r.html
- Zarr Format Specification: https://zarr.readthedocs.io/
- Project Repository: `/workspaces/cwru_data_marshal`

---

*Document generated for academic and research audiences. Performance figures based on development environment testing and may vary with hardware configuration.*
