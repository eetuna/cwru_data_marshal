# CWRU Data Marshal - Complete System Diagram

**All components, connections, and endpoints in one unified view**

---

```
┌─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                              CWRU DATA MARSHAL SYSTEM                                                                │
│                                          (MRI-Guided Robotic Surgery Platform)                                                       │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘


┌────────────────────────────────────────────────┐                    ┌────────────────────────────────────────────────┐
│           MRI MARSHAL SUBSYSTEM                │                    │          ROBOT MARSHAL SUBSYSTEM               │
│              (Port 8080/8090)                  │                    │              (Port 8081)                       │
└────────────────────────────────────────────────┘                    └────────────────────────────────────────────────┘


┌─────────────────┐                                                   ┌─────────────────┐
│ Image Streamer  │                                                   │ Catheter        │
│ (C++)           │                                                   │ Tracking Client │
└────────┬────────┘                                                   └────────┬────────┘
         │                                                                     │
         │ POST /v1/mrd/frame                                                 │ GET /read/localization_data
         │ (ISMRMRD binary ~48KB)                                             │ (sensor positions)
         │ Header: X-MRD-Stream: demo                                         │
         │                                                                     │ POST /write/tip_position_orientation
         │                                                                     │ (x,y,z,qw,qx,qy,qz)
         ▼                                                                     ▼
┌─────────────────────────────────────┐                         ┌─────────────────────────────────────┐
│                                     │                         │                                     │
│     MRI MARSHAL HTTP SERVER         │                         │    ROBOT MARSHAL HTTP SERVER        │
│        (Boost.Beast)                │                         │        (cpp-httplib)                │
│        Port 8080                    │                         │        Port 8081                    │
│                                     │                         │                                     │
└──┬────────────────────────────────┬─┘                         └──┬────────────────────────────────┬─┘
   │                                │                              │                                │
   │                                │                              │                                │
   ▲                                ▲                              ▲                                ▲
   │                                │                              │                                │
   │ POST /v1/bio/signal            │ POST /v1/pose/update         │ GET /read/user_input           │ GET /read/desired_planned_motion
   │ (ECG samples)                  │ (position + rotation)        │ (doctor commands)              │ (motion targets)
   │ {source, data[], rate_hz}      │ {p:[x,y,z], R:[...]}         │                                │
   │                                │                              │ POST /write/desired_planned_   │ POST /write/forward_kinematics
   │                                │                              │ motion (joint targets)         │ (joint angles)
   │                                │                              │                                │
┌──┴────────┐              ┌────────┴───┐                   ┌──────┴──────┐              ┌─────────┴──────┐
│ ECG Client│              │Pose Client │                   │  Planning   │              │   Controller   │
│ (Python)  │              │ (Python)   │                   │   Client    │              │     Client     │
└───────────┘              └────────────┘                   └─────────────┘              └────────────────┘


         │                                │                              │                                │
         │                                │                              │                                │
         └────────────┬───────────────────┘                              └────────────┬───────────────────┘
                      │                                                                │
                      ▼                                                                ▼
         ┌──────────────────────────┐                                    ┌──────────────────────────┐
         │                          │                                    │                          │
         │   MARSHAL STATE          │                                    │   VIRTUAL FILESYSTEM     │
         │   (Shared Cache)         │                                    │   (13 CircularBuffers)   │
         │                          │                                    │                          │
         │ • latest_mrd_json        │                                    │ Each buffer: 1000 entries│
         │   {path, frame_index,    │                                    │                          │
         │    dims:{x,y,z},         │                                    │ Channels:                │
         │    element_type,         │                                    │ • localization_data      │
         │    size_bytes, seq}      │                                    │ • tip_position_orient... │
         │                          │                                    │ • desired_planned_motion │
         │ • latest_bio_json        │                                    │ • forward_kinematics     │
         │   {source, data[],       │                                    │ • user_input             │
         │    rate_hz, timestamp}   │                                    │ • biological_signals     │
         │                          │                                    │ • surface_model_params   │
         │ • latest_pose_json       │                                    │ • streaming_2D_images    │
         │   {p:[x,y,z],            │                                    │ • 3D_images              │
         │    R:[...], timestamp}   │                                    │ ... (4 more)             │
         │                          │                                    │                          │
         └──┬────────────────────┬──┘                                    └──┬────────────────────┬──┘
            │                    │                                          │                    │
            │                    │                                          │                    │
            ▼                    ▼                                          ▼                    ▼
   ┌──────────────┐    ┌──────────────┐                          ┌──────────────┐    ┌──────────────┐
   │              │    │              │                          │              │    │              │
   │  MRD Sink    │    │ JSON Writer  │                          │ Background   │    │ Read Handler │
   │  (HDF5 SWMR) │    │ Thread       │                          │ Writer Thread│    │ (CircularBuf)│
   │              │    │ (Async)      │                          │ (Async)      │    │              │
   └──────┬───────┘    └──────┬───────┘                          └──────┬───────┘    └──────┬───────┘
          │                   │                                         │                   │
          │ Write frames      │ Write metadata                          │ Flush to disk     │ Return entries
          │ [N,1,3,64,64]     │ (JSONL format)                          │ (JSON files)      │ {entries:[...]}
          │                   │                                         │                   │
          ▼                   ▼                                         ▼                   ▼
   ┌──────────────┐    ┌──────────────┐                          ┌──────────────┐    ┌──────────────┐
   │              │    │              │                          │              │    │              │
   │ demo.mrd     │    │ *.jsonl      │                          │ files/       │    │   (in-mem    │
   │ (HDF5 file)  │    │              │                          │ *.json       │    │   only)      │
   │              │    │ • index.jsonl│                          │              │    │              │
   │ Dataset:     │    │ • bio.jsonl  │                          │ • file_1.json│    │              │
   │ /images/data │    │ • poses.jsonl│                          │ • file_2.json│    │              │
   │              │    │ • latest.json│                          │ ... (13 logs)│    │              │
   │ Shape:       │    │              │                          │              │    │              │
   │ [frames,1,   │    └──────────────┘                          └──────────────┘    └──────────────┘
   │  3,64,64]    │
   │              │
   │ Type:        │
   │ complex64    │
   │              │
   └──────┬───────┘
          │
          │ Direct HDF5 SWMR Read
          │ (No HTTP transfer!)
          │
          │ Reader calls:
          │ 1. f.open(swmr=True)
          │ 2. dset.refresh()
          │ 3. dset[frame_index][0][slice][:][:]
          │
          │
┌─────────┴──────────┐                                          ┌─────────────────┐
│                    │                                          │  Front-End      │
│   Viz Client       │                                          │  Client         │
│   (C++/OpenCV)     │                                          │                 │
│                    │                                          └────────┬────────┘
└─────────┬──────────┘                                                   │
          │                                                              │ GET /read/streaming_2D_images
          │ GET /v1/mrd/latest                                           │ GET /read/tip_position_orientation
          │ (polls every 10ms)                                           │ (for visualization)
          │                                                              │
          │ Response:                                                    │ POST /write/user_input
          │ {frame_index: 42,                                            │ (target position from UI)
          │  path: "demo.mrd",                                           │
          │  dims: {x:64, y:64, z:3},                                    │
          │  element_type: "complex64"}                                  │
          │                                                              │
          └──────────────────────┐                                       │
                                 │                                       │
                                 └───────────────┐                       │
                                                 │                       │
                                                 ▼                       ▼
                                    ┌────────────────────────────────────────┐
                                    │                                        │
                                    │         SAME HTTP SERVERS              │
                                    │         (looped back)                  │
                                    │                                        │
                                    └────────────────────────────────────────┘


                                                 │
                                                 │ GET /read/biological_signals
                                                 │ GET /read/streaming_2D_images
                                                 │ (for surface reconstruction)
                                                 │
                                                 │ POST /write/surface_model_parameters
                                                 │ (heart surface model)
                                                 │
                                                 ▼
                                    ┌─────────────────────┐
                                    │  Surface Tracking   │
                                    │  Client             │
                                    └─────────────────────┘



┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                  OPTIONAL: WEBSOCKET                                                                │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘

         ┌──────────────────────────────────────┐
         │  WebSocket Server (Port 8090)        │
         │  (MRI Marshal only - optional)       │
         └──────────┬───────────────────────────┘
                    │
                    │ ws://localhost:8090/ws
                    │
                    │ Client sends: {"subscribe": "mrd"}
                    │ Server sends: {"type":"mrd", "frame_index":42, "ts":"..."}
                    │
                    ▼
         ┌──────────────────────────────────────┐
         │  Any WebSocket Client                │
         │  (Alternative to HTTP polling)       │
         └──────────────────────────────────────┘




┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                    KEY INFORMATION                                                                   │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘

MRI MARSHAL ENDPOINTS:
  POST /v1/mrd/frame                  → Upload MRI frame (binary ISMRMRD)
  POST /v1/mrd/ingest                 → Legacy ingest endpoint (binary ISMRMRD)
  POST /v1/bio/signal                 → Upload ECG/bio signal (JSON)
  POST /v1/pose/update                → Upload position/orientation (JSON)
  GET  /v1/mrd/latest                 → Get latest frame metadata (JSON with dims, slice count, size)
  GET  /v1/mrd/frame?path=...&index=... → Get specific frame metadata
  GET  /v1/mrd/ingest?path=...        → Get file metadata for HDF5 access
  GET  /v1/mrd/since?ts=...&limit=... → Query frames since timestamp
  GET  /v1/mrd/since?last=N           → Get last N frames
  GET  /v1/bio/latest                 → Get latest bio signal (JSON)
  GET  /v1/pose/current               → Get current pose (JSON)
  GET  /v1/config                     → Get marshal configuration
  GET  /health                        → Health check

ROBOT MARSHAL ENDPOINTS:
  GET  /                   → List all 13 data channels (HTML)
  GET  /read/<filename>    → Read channel data (JSON: {entries:[{sent_at, values[]}]})
  POST /write/<filename>   → Write to channel (JSON: {sent_at, values[]})

WEBSOCKET (OPTIONAL):
  ws://localhost:8090/ws   → Subscribe to topics: "mrd", "bio", "pose"

DATA FORMATS:
  MRI Frame Response:      {type, path, stream, ts, t_ms, frame_index, flushed, element_type, dims:{x,y,z,channels}, size_bytes, seq}
  Bio Signal:              {source, data[], rate_hz, timestamp}
  Pose:                    {p:[x,y,z], R:[9 floats], timestamp}
  Robot Channel Entry:     {sent_at: nanoseconds, values: [...]}

IMAGE DIMENSIONS:
  Typical: 64×64 pixels, 3 slices, 1 channel, complex64 = 49,152 bytes per frame

ROBOT DATA CHANNELS (13 total):
  localization_data, tip_position_orientation, desired_planned_motion, forward_kinematics,
  biological_signals, surface_model_parameters, user_input, streaming_2D_images, 3D_images, ...

PERFORMANCE:
  MRI Marshal:  POST ~1ms, GET ~100μs, 20-50 FPS, HTTP polling at 100 Hz
  Robot Marshal: POST ~300μs, GET ~200μs, 50-80 Hz control loop

KEY TECHNOLOGIES:
  • SWMR (Single Writer Multiple Readers): HDF5 concurrent access - 1 writer, N readers, no blocking
  • ISMRMRD: Standard MRI raw data format (vendor-neutral)
  • CircularBuffer: Fixed 1000-entry ring buffers (bounded memory, lock-free reads)
  • Blackboard Pattern: Clients coordinate via shared state (no direct communication)
  • Async I/O: Background threads for disk writes (API never blocks on disk)

DATA FLOW HIGHLIGHTS:
  • Cache-first: All POSTs update in-memory cache immediately, then queue async disk write
  • Direct file access: Viz client reads HDF5 directly via SWMR (no HTTP transfer of image data)
  • Polling architecture: Viz client polls GET /v1/mrd/latest every 10ms (100 Hz)
  • Blackboard coordination: Robot clients read/write shared channels at 50-80 Hz

DEPLOYMENT:
  docker-compose -f docker-compose.demo.yml up
  MRI Marshal:   http://localhost:8080/health
  Robot Marshal: http://localhost:8081/
```

---

## Component Details

### MRI Marshal Components (Left Side)
1. **4 Clients**: Image Streamer, ECG Client, Pose Client, Viz Client
2. **HTTP Server**: Boost.Beast on port 8080
3. **Marshal State**: In-memory cache with latest data for fast GET responses
4. **MRD Sink**: Writes frames to HDF5 file in SWMR mode
5. **JSON Writer**: Background thread for async JSONL writes
6. **Storage**: HDF5 file (demo.mrd) + JSONL files (index, bio, poses)
7. **WebSocket Server** (optional): Port 8090 for pub/sub notifications

### Robot Marshal Components (Right Side)
1. **5 Clients**: Catheter Tracking, Controller, Planning, Front-End, Surface Tracking
2. **HTTP Server**: cpp-httplib on port 8081
3. **Virtual Filesystem**: 13 CircularBuffers (1000 entries each, in-memory)
4. **Background Writer**: Async thread for disk logging
5. **Read Handler**: Returns entries from CircularBuffers
6. **Storage**: JSON log files (files/*.json)

### Key Connections
- **MRI clients → HTTP → Cache → HDF5/JSONL** (producer-consumer pattern)
- **Viz client → HTTP (metadata) → Direct HDF5 SWMR read** (no image data via HTTP)
- **Robot clients ↔ HTTP ↔ CircularBuffers** (blackboard coordination, bidirectional)
- **All async writers → Disk** (non-blocking background I/O)

---

**This diagram shows:**
- ✅ All components connected (no disconnected parts)
- ✅ All HTTP endpoints labeled on arrows
- ✅ Data formats shown in transit
- ✅ Cache-before-file flow visible
- ✅ SWMR direct read path shown
- ✅ Robot blackboard pattern clear
- ✅ Complete end-to-end data flows
