# CWRU Data Marshal - Simple Overview

**Two Marshals. HTTP APIs. Real-time data coordination.**

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                                                                  │
│   MRI MARSHAL              ROBOT MARSHAL                        │
│   Port 8080                Port 8081                            │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## MRI Marshal (Port 8080)

**Purpose:** Handle medical imaging, ECG signals, and pose tracking

### Clients
- Image Streamer (generates MRI frames)
- ECG Client (sends heart signals)
- Pose Client (sends position data)
- Viz Client (displays images)

### Flow

```
┌──────────────┐                  ┌──────────────┐                  ┌──────────────┐
│   CLIENTS    │                  │ MRI MARSHAL  │                  │   STORAGE    │
│              │                  │  (Port 8080) │                  │              │
└──────────────┘                  └──────────────┘                  └──────────────┘

Image Streamer
      │
      │ POST /v1/mrd/frame
      │ (MRI frame data)
      └───────────────────────────>  HTTP Server ───────────────> demo.mrd
                                                                   (HDF5 file)

ECG Client
      │
      │ POST /v1/bio/signal
      │ (ECG data)
      └───────────────────────────>  HTTP Server ───────────────> bio.jsonl


Pose Client
      │
      │ POST /v1/pose/update
      │ (position x,y,z)
      └───────────────────────────>  HTTP Server ───────────────> poses.jsonl


Viz Client
      │
      │ GET /v1/mrd/latest
      │ (polls every 10ms)
      └───────────────────────────>  HTTP Server
                                          │
                                          │ Returns: {path, frame_index}
                                          │
      ┌───────────────────────────────────┘
      │
      └──> Read HDF5 file directly ────> demo.mrd ──> Display
```

### Endpoints

| Method | Endpoint | Purpose | Client |
|--------|----------|---------|--------|
| POST | `/v1/mrd/frame` | Upload MRI frame | Image Streamer |
| POST | `/v1/bio/signal` | Upload ECG signal | ECG Client |
| POST | `/v1/pose/update` | Upload position | Pose Client |
| GET | `/v1/mrd/latest` | Get latest frame info | Viz Client |
| GET | `/v1/bio/latest` | Get latest ECG | Any |
| GET | `/v1/pose/current` | Get current position | Any |

### Example Requests

**POST - Upload Frame:**
```bash
curl -X POST http://localhost:8080/v1/mrd/frame \
  -H "Content-Type: application/octet-stream" \
  --data-binary @frame.bin
```

**GET - Get Latest Frame:**
```bash
curl http://localhost:8080/v1/mrd/latest
# Returns: {"data": {"path": "/data/demo.mrd", "frame_index": 42}}
```

---

## Robot Marshal (Port 8081)

**Purpose:** Coordinate catheter robot control (5 clients communicating)

### Clients
- Catheter Tracking (computes tip position)
- Controller (controls motors)
- Planning (plans motion paths)
- Front-End (user interface)
- Surface Tracking (tracks heart surface)

### Flow

```
┌─────────────────┐              ┌──────────────┐              ┌─────────────────┐
│  ROBOT CLIENTS  │              │ROBOT MARSHAL │              │   DATA CHANNELS │
│                 │              │ (Port 8081)  │              │   (in-memory)   │
└─────────────────┘              └──────────────┘              └─────────────────┘

Catheter Tracking
      │
      │ GET /read/localization_data
      └───────────────────────────> HTTP Server ───────> localization_data
                                                          (CircularBuffer)
      ┌───────────────────────────────────┐
      │ Compute tip position              │
      └───────────────────────────────────┘
      │
      │ POST /write/tip_position_orientation
      └───────────────────────────> HTTP Server ───────> tip_position_orientation


Planning
      │
      │ GET /read/tip_position_orientation
      └───────────────────────────> HTTP Server ───────> tip_position_orientation
      │
      │ GET /read/user_input
      └───────────────────────────> HTTP Server ───────> user_input
      │
      ┌───────────────────────────────────┐
      │ Generate motion plan              │
      └───────────────────────────────────┘
      │
      │ POST /write/desired_planned_motion
      └───────────────────────────> HTTP Server ───────> desired_planned_motion


Controller
      │
      │ GET /read/desired_planned_motion
      └───────────────────────────> HTTP Server ───────> desired_planned_motion
      │
      ┌───────────────────────────────────┐
      │ Compute motor commands            │
      └───────────────────────────────────┘
      │
      │ POST /write/forward_kinematics
      └───────────────────────────> HTTP Server ───────> forward_kinematics


(All clients repeat at 50-80 Hz)
```

### Endpoints

| Method | Endpoint | Purpose | Example Client |
|--------|----------|---------|----------------|
| GET | `/read/<filename>` | Read data channel | All clients |
| POST | `/write/<filename>` | Write data channel | All clients |
| GET | `/` | List all channels | Browser |

### Data Channels

| Channel Name | Written By | Read By |
|--------------|------------|---------|
| `localization_data` | Sensors | Catheter Tracking |
| `tip_position_orientation` | Catheter Tracking | Planning, Front-End |
| `desired_planned_motion` | Planning | Controller |
| `forward_kinematics` | Controller | - |
| `user_input` | Front-End | Planning |
| `biological_signals` | ECG Monitor | Surface Tracking |
| `surface_model_parameters` | Surface Tracking | Planning |
| `streaming_2D_images` | MRI | Front-End, Surface |
| `3D_images` | MRI | Front-End |

### Example Requests

**GET - Read Channel:**
```bash
curl http://localhost:8081/read/tip_position_orientation
# Returns: {"entries": [{"sent_at": 1706356496123, "values": [1.0, 2.0, 3.0, 1, 0, 0, 0]}]}
```

**POST - Write Channel:**
```bash
curl -X POST http://localhost:8081/write/desired_planned_motion \
  -H "Content-Type: application/json" \
  -d '{"sent_at": 1706356496123, "values": [0.5, 0.3, 0.1, 0.2]}'
```

---

## Key Differences

| Feature | MRI Marshal | Robot Marshal |
|---------|-------------|---------------|
| **Port** | 8080 | 8081 |
| **Clients** | 4 clients | 5 clients |
| **Data Type** | Large (64KB+ frames) | Small (~1KB arrays) |
| **Storage** | HDF5 files | In-memory buffers |
| **Pattern** | Producer → Consumer | Blackboard (shared state) |
| **Rate** | 20-50 FPS | 50-80 Hz |

---

## Quick Start

```bash
# Start both marshals
docker-compose -f docker-compose.demo.yml up

# Check MRI Marshal
curl http://localhost:8080/health

# Check Robot Marshal
curl http://localhost:8081/

# View MRI latest frame
curl http://localhost:8080/v1/mrd/latest

# List robot data channels
curl http://localhost:8081/
```

---

## That's It!

**MRI Marshal:** POST data → Store in files → GET metadata → Read files directly

**Robot Marshal:** POST to channels → Store in memory → GET from channels → Repeat

Both use simple HTTP POST/GET. No complex protocols. Just REST APIs.
