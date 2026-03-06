# CWRU Data Marshal - Simplified Architecture

**Generated:** 2026-01-27

---

## System Overview

Two independent marshals working in parallel:
- **MRI Marshal** (Port 8080/8090): Medical imaging, bio signals, pose tracking
- **Robot Marshal** (Port 8081): Catheter control coordination

---

## MRI Marshal Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                        MRI MARSHAL (Port 8080/8090)                     │
└────────────────────────────────────────────────────────────────────────┘

┌─────────────┐         ┌─────────────┐         ┌─────────────┐
│   CLIENTS   │         │   SERVERS   │         │    STATE    │
└─────────────┘         └─────────────┘         └─────────────┘

Image Streamer ─────┐
                    │
ECG Client ─────────┼──> HTTP Server ────────────────> Marshal State
                    │    (Port 8080)                    (Shared Cache)
Pose Client ────────┤    Boost.Beast                         │
                    │                                         │
Viz Client ─────────┘                                         │
                                                              │
                          WebSocket ──────────────────────────┘
                          (Port 8090)
                          Pub/Sub
                          (optional)
                                                        │
                         ┌────────────────────────────┬─┴───────────┐
                         │                            │             │
                         ▼                            ▼             ▼
                   ┌──────────┐              ┌──────────────┐  ┌────────┐
                   │ MRD Sink │              │ JSON Writer  │  │ Caches │
                   │ (HDF5)   │              │ (Async)      │  │(latest)│
                   └────┬─────┘              └──────┬───────┘  └────────┘
                        │                           │
                        ▼                           ▼
                   ┌──────────┐              ┌──────────────┐
                   │ demo.mrd │              │  *.jsonl     │
                   │(HDF5 SWMR│              │ (index, bio, │
                   │  file)   │              │   poses)     │
                   └────┬─────┘              └──────────────┘
                        │
                        │ (Direct Read - SWMR)
                        │
                        └────────────────────────> Viz Client
```

### MRI Marshal Flow

1. **Image Streamer** → POST `/v1/mrd/frame` → HTTP Server
2. **HTTP Server** → MRD Sink → Write to **demo.mrd** (HDF5 SWMR)
3. **MRD Sink** → Update **Marshal State** cache
4. **Marshal State** → Queue JSON write → **index.jsonl**
5. **Viz Client** → Polls GET `/v1/mrd/latest` every ~10ms → Get file path + frame index
6. **Viz Client** → Direct HDF5 read (SWMR mode) → Display frame
7. **Viz Client** → Loop back to step 5 (async polling at ~100 Hz)

### Key Endpoints

**POST Endpoints:**
- `POST /v1/mrd/frame` - Upload MRI frame (ISMRMRD binary)
- `POST /v1/bio/signal` - Submit ECG/bio signal
- `POST /v1/pose/update` - Submit pose/tracking data

**GET Endpoints:**
- `GET /v1/mrd/latest` - Get latest frame metadata
- `GET /v1/bio/latest` - Get latest bio signal
- `GET /v1/pose/current` - Get current pose

**WebSocket (Optional):**
- `ws://localhost:8090/ws` - Subscribe to topics: `mrd`, `bio`, `pose`
- Note: Current viz_client uses HTTP polling instead of WebSocket

---

## Robot Marshal Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                        ROBOT MARSHAL (Port 8081)                        │
└────────────────────────────────────────────────────────────────────────┘

┌────────────────┐      ┌─────────────┐      ┌──────────────────┐
│ ROBOT CLIENTS  │      │   SERVER    │      │ VIRTUAL FILESYSTEM│
└────────────────┘      └─────────────┘      └──────────────────┘

Catheter Tracking ─┐
                   │
Controller ────────┤
                   │
Planning ──────────┼──> HTTP Server ────> 13 Data Channels:
                   │    (Port 8081)       ┌────────────────────┐
Front-End ─────────┤    cpp-httplib       │• localization_data │
                   │    Thread Pool       │• tip_position_...  │
Surface Tracking ──┘                      │• desired_motion    │
                                          │• forward_kinematics│
        ▲                                 │• bio_signals       │
        │                                 │• surface_model     │
        │ GET/POST                        │• user_input        │
        │ (50-80 Hz)                      │• 2D_images         │
        │                                 │• 3D_images         │
        └─────────────────────────────────│  ... (4 more)      │
                                          └─────────┬──────────┘
                                                    │
                                             CircularBuffers
                                             (1000 entries each)
                                                    │
                                                    ▼
                                          ┌──────────────────┐
                                          │ Background Writer│
                                          │ (Async Thread)   │
                                          └─────────┬────────┘
                                                    │
                                                    ▼
                                          ┌──────────────────┐
                                          │  files/*.json    │
                                          │    (logs)        │
                                          └──────────────────┘
```

### Robot Marshal Flow (Control Loop @ 50-80 Hz)

**Iteration N:**

1. **Catheter Tracking**:
   - GET `/read/localization_data` → Compute tip position
   - POST `/write/tip_position_orientation`

2. **Planning**:
   - GET `/read/tip_position_orientation`
   - GET `/read/user_input`
   - GET `/read/surface_model_parameters`
   - Generate motion plan
   - POST `/write/desired_planned_motion`

3. **Controller**:
   - GET `/read/desired_planned_motion`
   - Compute inverse kinematics
   - POST `/write/forward_kinematics`

4. **Front-End**:
   - GET `/read/streaming_2D_images`
   - GET `/read/tip_position_orientation`
   - Render UI, capture user input
   - POST `/write/user_input`

5. **Surface Tracking**:
   - GET `/read/biological_signals`
   - GET `/read/streaming_2D_images`
   - 3D reconstruction
   - POST `/write/surface_model_parameters`

All clients communicate through the **Robot Marshal** (blackboard pattern).

### Key Endpoints

**Read Endpoint:**
- `GET /read/<filename>` - Read from data channel
  - Returns: `{"entries": [{"sent_at": timestamp, "values": [...]}]}`

**Write Endpoint:**
- `POST /write/<filename>` - Write to data channel
  - Body: `{"sent_at": timestamp, "values": [...]}`

**List Endpoint:**
- `GET /` - HTML page listing all 13 data channels

---

## Data Channels (Robot Marshal)

| Channel | Purpose | Readers | Writers |
|---------|---------|---------|---------|
| `localization_data` | Sensor positions | Catheter Tracking | Sensors |
| `tip_position_orientation` | Catheter tip pose (x,y,z,q) | Planning, Front-End | Catheter Tracking |
| `desired_planned_motion` | Motion plan targets | Controller | Planning |
| `forward_kinematics` | Joint angles | - | Controller |
| `biological_signals` | ECG, vitals | Surface Tracking | ECG Monitor |
| `surface_model_parameters` | Heart surface model | Planning | Surface Tracking |
| `user_input` | Doctor commands | Planning | Front-End |
| `streaming_2D_images` | Live 2D imaging | Front-End, Surface | MRI |
| `3D_images` | 3D volumes | Front-End | MRI |
| `catheter_base_configuration` | System config | Tracking, Controller | Config |

---

## Key Differences

| Feature | MRI Marshal | Robot Marshal |
|---------|-------------|---------------|
| **Port** | 8080 (HTTP), 8090 (WS) | 8081 (HTTP) |
| **Language** | C++ (Boost.Beast) | C++ (cpp-httplib) |
| **Storage** | HDF5 SWMR | JSON files |
| **Data Pattern** | Metadata-only API + direct file access | Full data in API responses |
| **Notification** | WebSocket pub/sub | Polling (GET) |
| **Data Size** | Large (64KB+ per frame) | Small (~1-10KB) |
| **Rate** | 20-50 FPS | 50-80 Hz |
| **Clients** | 4 (streamer, ECG, pose, viz) | 5 (tracking, controller, planning, frontend, surface) |
| **Coordination** | Event-driven (WebSocket) | Blackboard (shared state) |

---

## Performance

### MRI Marshal
- **POST /v1/mrd/frame**: ~1ms (64KB frame)
- **GET /v1/mrd/latest**: ~100μs
- **WebSocket broadcast**: ~500μs
- **HDF5 SWMR write**: ~2ms
- **End-to-end latency**: 50-100ms

### Robot Marshal
- **GET /read/<file>**: ~200μs
- **POST /write/<file>**: ~300μs
- **Control loop rate**: 50-80 Hz (12-20ms per iteration)
- **In-memory cache hit**: ~50μs

---

## Technology Stack

| Component | MRI Marshal | Robot Marshal |
|-----------|-------------|---------------|
| HTTP | Boost.Beast | cpp-httplib |
| WebSocket | Boost.Beast | - |
| Storage | HDF5 1.12+ (SWMR) | JSON files |
| Data Format | ISMRMRD | JSON |
| Threading | Boost.Asio + custom | Thread pool |
| Build | CMake + Docker | CMake + Docker |

---

## Use Cases

### Use Case 1: Real-Time MRI Visualization
1. Image Streamer generates frame → POST to MRI Marshal
2. MRI Marshal writes to HDF5, updates in-memory cache
3. Viz Client polls GET `/v1/mrd/latest` at ~100 Hz
4. Viz Client gets new frame index → reads HDF5 directly (SWMR)
5. Viz Client displays frame (50-100ms total latency)

### Use Case 2: Catheter Control Loop
1. All 5 robot clients run in parallel at 50-80 Hz
2. Each client reads from its configured channels
3. Each client processes data (FK, IK, planning, etc.)
4. Each client writes results back to Robot Marshal
5. Loop repeats (blackboard coordination pattern)

### Use Case 3: ECG-Gated Imaging
1. ECG Client posts signal → MRI Marshal
2. MRI Marshal broadcasts on `bio` topic
3. Image Streamer subscribes, triggers at R-peak
4. Synchronized imaging with cardiac cycle

---

## File Locations

```
.worktrees/
├── mri_data_marshal/              # MRI Marshal
│   ├── src/marshal_main.cpp       # Entry point
│   ├── src/marshal_http.hpp       # HTTP handlers
│   ├── src/marshal_ws.hpp         # WebSocket server
│   ├── src/marshal_state.hpp      # Shared state
│   ├── src/mrd_sink.cpp           # HDF5 writer
│   └── clients/                   # 4 client implementations
│
└── robot_data_marshal/            # Robot Marshal
    ├── server.cpp                 # Main server
    ├── circularBuffer.hpp         # Lock-free buffer
    ├── files.json                 # Data channel config
    ├── file_routes.json           # Client routing
    └── client-*.cpp               # 5 robot clients
```

---

## Deployment

```bash
# Start all services
docker-compose -f docker-compose.demo.yml up

# Verify MRI Marshal
curl http://localhost:8080/health

# Verify Robot Marshal
curl http://localhost:8081/

# View WebSocket
wscat -c ws://localhost:8090/ws
> {"subscribe":"mrd"}
```

---

## Summary

- **MRI Marshal**: High-performance medical imaging with HDF5 SWMR and WebSocket pub/sub
- **Robot Marshal**: Lightweight blackboard coordination for catheter control at 50-80 Hz
- **Architecture**: Dual marshals working independently with specialized clients
- **Performance**: Sub-100ms imaging latency, real-time control loop
- **Scalability**: Multiple concurrent readers via SWMR, circular buffers for high-frequency writes

---

**Document Version:** 1.0
**Generated by:** Claude Code Architecture Analysis
