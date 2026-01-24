# CWRU Data Marshal

Complete MRI-guided robotic surgery data management system with real-time streaming, biological signal monitoring, and robot state coordination.

---

## 🚀 Quick Start

### Docker Demo (Recommended)
Run the complete system demo with pre-built Docker images:

```bash
# Quick 30-second demo
./scripts/demo-docker.sh

# Persistent demo (services stay running)
./scripts/demo-persistent.sh
```

**What runs:**
- 2 Marshals (MRI + Robot)
- 3 Data Generators (Image streamer, ECG, Pose)
- 5 Robot Clients (Catheter tracking, Controller, Planning, Front-end, Surface tracking)
- 1 Visualization Client (Optional, requires X11)

### Export for USB Transfer
Package the entire system for offline deployment:

```bash
./scripts/export_usb.sh
# Creates: cwru-marshal-export/ with Docker images + docs + demo scripts
```

---

## 📖 Documentation

### Essential Guides (in `/docs/`)
- **[API_REFERENCE.md](docs/API_REFERENCE.md)** - Complete API documentation for both marshals
- **[EXTERNAL_CLIENT_GUIDE.md](docs/EXTERNAL_CLIENT_GUIDE.md)** - How to integrate your own clients
- **[DEMO_AND_API_EXPORT.md](docs/DEMO_AND_API_EXPORT.md)** - Demo system overview and basic API examples

### Archived Documentation
Historical docs moved to `archive/docs_old/` for reference.

---

## 🏗️ System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  MRI Marshal (Port 8080, WebSocket 8090)                    │
│  - Receives MRI frames via HTTP/WebSocket                   │
│  - Stores in HDF5 (SWMR mode) for real-time access         │
│  - Stores ECG/pose data in JSONL                            │
│  - Broadcasts frame notifications via WebSocket             │
└─────────────────────────────────────────────────────────────┘
                          │
                          │ HTTP API (metadata only)
                          │ Clients use HDF5 for binary data
                          ▼
              ┌──────────────────────┐
              │  Your MRI Client     │
              │  (h5py, HDF5 C++)    │
              └──────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  Robot Marshal (Port 8081)                                   │
│  - In-memory "virtual filesystem" for robot state           │
│  - Read/write JSON endpoints for coordination               │
│  - 10+ data channels (positions, commands, tracking, etc.)  │
└─────────────────────────────────────────────────────────────┘
                          │
                          │ HTTP API (JSON)
                          ▼
              ┌──────────────────────┐
              │  Your Robot Client   │
              │  (REST API)          │
              └──────────────────────┘
```

---

## 🔑 Key Features

### MRI Marshal
- **HDF5 SWMR Mode** - Concurrent read/write for real-time streaming
- **Metadata-Only HTTP API** - Returns file paths, clients do direct HDF5 reads
- **WebSocket Notifications** - Real-time frame arrival notifications
- **Biological Signals** - ECG/vitals stored as time-series JSONL
- **Pose Tracking** - Position/orientation data storage

### Robot Marshal
- **Virtual Filesystem** - In-memory JSON "files" for fast state exchange
- **Blackboard Pattern** - Multiple clients read/write shared state
- **10+ Data Channels** - Tip position, motion plans, user input, surface models, etc.
- **~50-80Hz Operation** - Low-latency HTTP-based coordination

### Demo System
- **Synthetic Data** - Generates realistic MRI frames, ECG, and poses
- **5 Robot Clients** - Full catheter system simulation
- **Visualization** - Real-time OpenCV display (optional)
- **Configurable** - Adjust frame rates, dimensions, duration via `.env.demo`

---

## 🛠️ Development

### Repository Structure
```
.
├── scripts/
│   ├── demo-docker.sh           # Quick demo (30s)
│   ├── demo-persistent.sh       # Persistent demo
│   ├── export_usb.sh            # Export for USB transfer
│   └── tools/                   # Worktree helpers
├── docker/
│   ├── Dockerfile.mri-marshal   # MRI marshal image
│   ├── Dockerfile.robot-marshal # Robot marshal image
│   ├── Dockerfile.ecg-client    # ECG generator
│   └── ...                      # Other client images
├── docs/
│   ├── API_REFERENCE.md         # Complete API docs
│   ├── EXTERNAL_CLIENT_GUIDE.md # Client integration
│   └── DEMO_AND_API_EXPORT.md   # Demo guide
├── docker-compose.yml           # Development setup
├── docker-compose.demo.yml      # Demo orchestration
└── .env.demo                    # Demo configuration
```

### Worktree Setup
This repo uses git worktrees to build from dedicated marshal branches:

```bash
# MRI marshal worktree
git worktree add ../mri_data_marshal_worktree mri-data-marshal

# Robot marshal worktree
git worktree add ../robot_data_marshal_worktree robot_data_marshal_with_catheter_system_components
```

The demo scripts automatically create these if needed.

### Build from Source
```bash
# Build all Docker images
docker compose -f docker-compose.demo.yml build

# Or build individual components
docker compose -f docker-compose.demo.yml build mri-marshal
docker compose -f docker-compose.demo.yml build robot-marshal
```

---

## 📡 API Quick Reference

### MRI Marshal (Port 8080)
```bash
# Health check
curl http://localhost:8080/health

# Get latest frame metadata (returns file path + index)
curl http://localhost:8080/v1/mrd/latest

# Get file metadata for HDF5 access
curl http://localhost:8080/v1/mrd/ingest

# Then use HDF5 library to read binary data directly
```

### Robot Marshal (Port 8081)
```bash
# List available data channels
curl http://localhost:8081/

# Read catheter tip position
curl http://localhost:8081/read/tip_position_orientation

# Write user command
curl -X POST http://localhost:8081/write/user_input \
  -H "Content-Type: application/json" \
  -d '{"values":[10.0,20.0,30.0,0,0,90],"sent_at":1706126625123456789}'
```

See [API_REFERENCE.md](docs/API_REFERENCE.md) for complete documentation.

---

## 🔒 Security Note

**The demo has NO authentication!** For production:
- Add API keys or OAuth
- Use HTTPS/WSS instead of HTTP/WS
- Implement rate limiting
- Add access control lists

---

## 📄 License

See LICENSE file for details.

---

## 🙋 Support

- **Documentation Issues:** Check `docs/API_REFERENCE.md` and `docs/EXTERNAL_CLIENT_GUIDE.md`
- **Demo Issues:** Review `docs/DEMO_AND_API_EXPORT.md`
- **Source Code:** See marshal worktree branches for implementation details
