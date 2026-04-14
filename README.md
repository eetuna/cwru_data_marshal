# CWRU Data Marshal

Complete MRI-guided robotic surgery data management system with real-time streaming, biological signal monitoring, and robot state coordination.

---

## Quick Start

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
- 5 Data Generators (K-space streamer, Image streamer, Mock recon, ECG, Pose)
- 5 Robot Clients (Catheter tracking, Controller, Planning, Front-end, Surface tracking)
- 1 Visualization Client (Optional, requires X11)

### Export for USB Transfer
Package the entire system for offline deployment:

```bash
./scripts/export_usb.sh
# Creates: cwru-marshal-export/ with Docker images + docs + demo scripts
```

---

## Documentation

### Essential Guides (in `/docs/`)
- **[API_REFERENCE.md](docs/API_REFERENCE.md)** - Complete API documentation for both marshals
- **[EXTERNAL_CLIENT_GUIDE.md](docs/EXTERNAL_CLIENT_GUIDE.md)** - How to integrate your own clients
- **[DEMO_AND_API_EXPORT.md](docs/DEMO_AND_API_EXPORT.md)** - Demo system overview and basic API examples

### Archived Documentation
Historical docs moved to `archive/docs_old/` for reference.

---

## System Architecture

```
Scanner / K-Space Streamer
    │
    │  MRD TCP (port 9100)
    │  CONFIG_FILE + METADATA_XML + ACQUISITION×N + WAVEFORM + CLOSE
    v
+--------------------------------------+     MRD TCP
|  MRI Marshal                         | ──> Reconstruction Service (port 9002)
|  HTTP :8080 (query/control)          |
|  MRD TCP :9100 (scanner data)        | <── IMAGE(1022) returned
|  - Optional dump to from_scanner/    |
|  - Forwards to recon via MRD TCP     |
|  - Pushes IMAGE back to scanner      |
|  - GET /image/latest (file path)     |
|  - POST/GET /pose, PUT/GET /transform|
+--------------------------------------+
          │
          │ HTTP GET /image/latest (file path)
          v
    Viz Client (reads standalone file, renders with OpenCV)

+----------------------------+
|  Robot Marshal (Port 8081) |
|  - Virtual filesystem      |
|  - Read/write JSON channels|
+----------------------------+
          |
          v
    Robot Clients (catheter, controller, planning, front-end, surface)
```

---

## Key Features

### MRI Marshal
- **Canonical ISMRMRD HDF5 dump** - Records scanner and recon data in standard format when `--dump` is enabled
- **Standalone File for Live View** - Atomic rename for fast image polling
- **Reconstruction Forwarding** - Transparent proxy to external recon service
- **Pose Tracking** - Cached JSON position/orientation
- **Slice Transform** - Delta transform with atomic consume-on-read

### Robot Marshal
- **Virtual Filesystem** - In-memory JSON "files" for fast state exchange
- **Blackboard Pattern** - Multiple clients read/write shared state
- **10+ Data Channels** - Tip position, motion plans, user input, surface models, etc.
- **~50-80Hz Operation** - Low-latency HTTP-based coordination

### Demo System
- **Two Flows** - K-space (with recon) or pre-made images (bypass recon)
- **5 Robot Clients** - Full catheter system simulation
- **Visualization** - Real-time OpenCV display (optional)
- **Configurable** - Adjust frame rates, dimensions, duration via `.env.demo`

---

## Development

### Repository Structure
```
.
├── scripts/
│   ├── demo-docker.sh           # Quick demo (30s)
│   ├── demo-persistent.sh       # Persistent demo
│   ├── export_usb.sh            # Export for USB transfer
│   └── build-client-images.sh   # Build all Docker images
├── docker/
│   ├── Dockerfile.mri           # MRI marshal image
│   ├── Dockerfile.mock-recon    # Reconstruction service
│   ├── Dockerfile.kspace-streamer
│   ├── Dockerfile.image-streamer
│   └── ...                      # Other client images
├── docs/
│   ├── API_REFERENCE.md         # Complete API docs
│   ├── EXTERNAL_CLIENT_GUIDE.md # Client integration
│   └── DEMO_AND_API_EXPORT.md   # Demo guide
├── docker-compose.demo.yml      # Demo orchestration
└── .env.demo                    # Demo configuration
```

### Worktree Setup
This repo uses git worktrees to build from dedicated marshal branches:

```bash
# MRI marshal worktree
git worktree add .worktrees/mri_data_marshal feature/mri-marshal-rewrite-v2-inner

# Robot marshal worktree
git worktree add .worktrees/robot_data_marshal robot_data_marshal_with_catheter_system_components
```

The demo scripts automatically create these if needed.

### Build from Source
```bash
# Build all Docker images
./scripts/build-client-images.sh

# Or build individual components
docker compose --env-file .env.demo -f docker-compose.demo.yml build mri-marshal
```

---

## API Quick Reference

### MRI Marshal (Port 8080)
```bash
# Health check
curl http://localhost:8080/health

# Get latest reconstructed image path
curl http://localhost:8080/image/latest

# Get/set slice transform
curl http://localhost:8080/transform
curl -X PUT http://localhost:8080/transform \
  -H "Content-Type: application/json" \
  -d '{"through_plane_mm":1.0,"readout_mm":0,"phase_mm":0,"rotation_rad":0}'

# Pose
curl http://localhost:8080/pose
curl -X POST http://localhost:8080/pose \
  -H "Content-Type: application/json" \
  -d '{"position":[1,2,3],"orientation":[0,0,0.707,0.707]}'

# List archived files
curl http://localhost:8080/dump/scanner
curl http://localhost:8080/dump/recon
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

## Security Note

**The demo has NO authentication!** For production:
- Add API keys or OAuth
- Use HTTPS instead of HTTP
- Implement rate limiting
- Add access control lists

---

## License

See LICENSE file for details.

---

## Support

- **Documentation Issues:** Check `docs/API_REFERENCE.md` and `docs/EXTERNAL_CLIENT_GUIDE.md`
- **Demo Issues:** Review `docs/DEMO_AND_API_EXPORT.md`
- **Source Code:** See marshal worktree branches for implementation details
