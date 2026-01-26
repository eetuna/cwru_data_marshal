# CWRU Data Marshal - Handover & Deployment Guide

This document provides complete instructions for deploying and running the CWRU Data Marshal system from either the GitHub repository or the USB export package.

---

## Table of Contents

1. [Deployment Options](#deployment-options)
2. [Option A: From GitHub Repository](#option-a-from-github-repository)
3. [Option B: From USB Export Package](#option-b-from-usb-export-package)
4. [Environment Configuration](#environment-configuration)
5. [Running the Demo](#running-the-demo)
6. [Connecting External Clients](#connecting-external-clients)
7. [Troubleshooting](#troubleshooting)

---

## Deployment Options

### Option A: Clone from GitHub
Best for development, allows rebuilding from source.

### Option B: Load from USB Export
Best for offline deployment, uses pre-built Docker images.

---

## Option A: From GitHub Repository

### Prerequisites
- Docker Engine 20.10+
- Docker Compose 2.0+
- Git
- 10GB free disk space

### 1. Clone Repository
```bash
git clone https://github.com/cwru-mercis/cwru_data_marshal.git
cd cwru_data_marshal
```

### 2. Verify Repository Structure
```bash
ls -la
# Should see:
# - docker-compose.demo.yml
# - .env.demo
# - scripts/demo-docker.sh
# - scripts/demo-persistent.sh
# - docs/API_REFERENCE.md
# - README.md
```

### 3. Configure Environment Variables

The demo configuration is in `.env.demo`. Key settings:

```bash
# Edit if needed (defaults work fine)
nano .env.demo
```

**Essential variables:**
```bash
# Image streaming (20fps, 64x64x3 slices)
IMAGE_WIDTH=64
IMAGE_HEIGHT=64
IMAGE_SLICES=3
IMAGE_INTERVAL=20        # 20ms = 50fps

# Demo duration
DEMO_DURATION=30         # seconds (0 = infinite)

# Data persistence
CLEANUP_DATA=false       # Set true to auto-delete after demo

# Visualization
ENABLE_VIZ=true         # Requires X11 display
```

### 4. Build Docker Images
```bash
# Build all images (takes 5-10 minutes first time)
docker compose -f docker-compose.demo.yml build

# Or build specific services
docker compose -f docker-compose.demo.yml build mri-marshal
docker compose -f docker-compose.demo.yml build robot-marshal
```

### 5. Run the Demo

**Quick 30-second demo:**
```bash
./scripts/demo-docker.sh
```

**Persistent demo (keeps running):**
```bash
./scripts/demo-persistent.sh
# Press Ctrl+C to stop monitoring (containers keep running)
# To stop: docker compose -f docker-compose.demo.yml down
```

---

## Option B: From USB Export Package

### Prerequisites
- Docker Engine 20.10+
- Docker Compose 2.0+
- 10GB free disk space

### 1. Extract Package
```bash
# Copy USB package to local machine
cp -r /media/usb/cwru-marshal-export ~/
cd ~/cwru-marshal-export
```

### 2. Verify Package Contents
```bash
ls -la
# Should see:
# - images/cwru-demo-images.tar
# - docker-compose.demo.yml
# - .env.demo
# - demo-docker.sh
# - demo-persistent.sh
# - docs/API_REFERENCE.md
# - README.md
```

### 3. Load Docker Images
```bash
# Load all 7 images from tarball (takes 2-5 minutes)
docker load -i images/cwru-demo-images.tar
```

**Verify images loaded:**
```bash
docker images | grep cwru
# Should see:
# cwru-mri-marshal
# cwru-robot-marshal
# cwru-image-streamer
# cwru-ecg-client
# cwru-pose-client
# cwru-viz-client
# cwru-robot-clients
```

### 4. Configure Environment (Optional)
```bash
# Edit .env.demo if you want to change defaults
nano .env.demo
```

See [Environment Configuration](#environment-configuration) section below for details.

### 5. Run the Demo

**Quick 30-second demo:**
```bash
./demo-docker.sh
```

**Persistent demo (keeps running):**
```bash
./demo-persistent.sh
```

---

## Environment Configuration

### Complete `.env.demo` Reference

```bash
# =============================================================================
# IMAGE STREAMING CONFIGURATION
# =============================================================================
IMAGE_WIDTH=64                # Image width in pixels (16-512)
IMAGE_HEIGHT=64               # Image height in pixels (16-512)
IMAGE_SLICES=3                # Number of slices per volume (1-20)
IMAGE_INTERVAL=20             # Streaming interval in ms (20ms = 50fps, 50ms = 20fps)
IMAGE_LOG_STRIDE=100          # Print frame info every N frames (0 = silent)

# =============================================================================
# ECG CLIENT CONFIGURATION
# =============================================================================
ECG_INTERVAL=1                # ECG sample interval in seconds
ECG_HEART_RATE=72             # Simulated heart rate in BPM

# =============================================================================
# POSE CLIENT CONFIGURATION
# =============================================================================
POSE_INTERVAL=1               # Pose update interval in seconds
POSE_RADIUS=50.0              # Circular trajectory radius in mm

# =============================================================================
# DATA STORAGE
# =============================================================================
SESSION_DATA_DIR=./session-data  # Where MRI data files are stored (HDF5 + JSONL)

# =============================================================================
# DEMO RUNTIME SETTINGS
# =============================================================================
DEMO_DURATION=30              # Demo run time in seconds (0 = run forever)
ENABLE_VIZ=true               # Show visualization client window (requires X11)
CLEANUP_DATA=false            # Remove session-data/ after demo (true/false)
MONITOR_INTERVAL=2            # Print robot ops count every N seconds
```

### Common Configuration Scenarios

**Fast, high-resolution streaming:**
```bash
IMAGE_WIDTH=128
IMAGE_HEIGHT=128
IMAGE_SLICES=5
IMAGE_INTERVAL=20  # 50fps
```

**Long-running demo for client development:**
```bash
DEMO_DURATION=0     # Infinite
CLEANUP_DATA=false  # Keep all data
ENABLE_VIZ=false    # Headless (no X11 needed)
```

**Headless server deployment:**
```bash
ENABLE_VIZ=false
```

---

## Running the Demo

### Demo Modes

#### 1. Quick Demo (Auto-stops after DEMO_DURATION)
```bash
./scripts/demo-docker.sh   # From GitHub
./demo-docker.sh           # From USB package
```

**What happens:**
- Starts all 12 containers
- Runs for `DEMO_DURATION` seconds (default: 30s)
- Shows live logs
- Auto-stops and cleans up

**Services started:**
- MRI Marshal (port 8080, WebSocket 8090)
- Robot Marshal (port 8081)
- Image Streamer (generates MRI frames)
- ECG Client (generates ECG signals)
- Pose Client (generates pose data)
- 5 Robot Clients (catheter tracking, controller, planning, front-end, surface tracking)
- Viz Client (if ENABLE_VIZ=true)

#### 2. Persistent Demo (Services stay running)
```bash
./scripts/demo-persistent.sh   # From GitHub
./demo-persistent.sh           # From USB package
```

**What happens:**
- Starts all containers in detached mode
- Monitoring script shows progress
- Press Ctrl+C to stop monitoring (containers keep running)
- Re-run script to attach monitor again

**Stop services:**
```bash
docker compose -f docker-compose.demo.yml down
```

#### 3. Manual Per-Service Control

**Perfect for connecting your own clients!**

**Terminal 1 - MRI Marshal:**
```bash
docker compose -f docker-compose.demo.yml up mri-marshal
```

**Terminal 2 - Robot Marshal:**
```bash
docker compose -f docker-compose.demo.yml up robot-marshal
```

**Terminal 3 - Image Streamer:**
```bash
docker compose -f docker-compose.demo.yml up image-streamer
```

**Terminal 4 - ECG Client:**
```bash
docker compose -f docker-compose.demo.yml up ecg-client
```

**Terminal 5 - Pose Client:**
```bash
docker compose -f docker-compose.demo.yml up pose-client
```

**Terminal 6 - Robot Clients (all 5):**
```bash
docker compose -f docker-compose.demo.yml up robot-clients
```

**Terminal 7 - Viz Client (optional):**
```bash
docker compose -f docker-compose.demo.yml --profile viz up viz-client
```

**Benefits:**
- See each service's logs in real-time
- Stop/restart individual services (Ctrl+C)
- Test components in isolation
- Connect your own clients while system runs

---

## Connecting External Clients

### 1. Verify Services Running

```bash
# Check health
curl http://localhost:8080/health
# {"status":"ok"}

# List robot marshal endpoints
curl http://localhost:8081/
# Shows HTML page with all available endpoints
```

### 2. MRI Marshal API (Port 8080)

**Get latest frame metadata:**
```bash
curl http://localhost:8080/v1/mrd/latest | jq
```

**Response:**
```json
{
  "data": {
    "path": "/session-data/mrd/demo.mrd",
    "frame_index": 1234,
    "total_frames": 1235
  }
}
```

**IMPORTANT:** The HTTP API returns **metadata only** (file paths, indices). To read actual image data, use HDF5 library:

**Python example:**
```python
import requests
import h5py
import numpy as np

# Get file path via HTTP
response = requests.get('http://localhost:8080/v1/mrd/latest')
file_path = response.json()['data']['path']
frame_idx = response.json()['data']['frame_index']

# Read binary data via HDF5 (SWMR mode)
with h5py.File(file_path, 'r', swmr=True) as f:
    dset = f['/images/data']
    dset.refresh()
    frame = dset[frame_idx, 0, :, :, :]  # shape: [z, y, x]
    print(f"Frame shape: {frame.shape}")
```

### 3. Robot Marshal API (Port 8081)

**Read catheter tip position:**
```bash
curl http://localhost:8081/read/tip_position_orientation | jq
```

**Write user command:**
```bash
curl -X POST http://localhost:8081/write/user_input \
  -H "Content-Type: application/json" \
  -d '{"values":[10.0,20.0,30.0,0,0,90],"sent_at":1706126625123456789}'
```

### 4. WebSocket Notifications (Port 8090)

**Subscribe to real-time frame updates:**

```python
import asyncio
import websockets
import json

async def listen():
    uri = "ws://localhost:8090/ws"
    async with websockets.connect(uri) as ws:
        await ws.send(json.dumps({"subscribe": "mrd"}))
        async for msg in ws:
            data = json.loads(msg)
            print(f"New frame: {data['frame_index']}")

asyncio.run(listen())
```

### 5. Complete API Documentation

See `docs/API_REFERENCE.md` for:
- All 12 MRI marshal endpoints
- Robot marshal endpoint reference
- Complete Python/C++ example code
- WebSocket protocol details
- Data format specifications

---

## Troubleshooting

### Docker Issues

**Docker daemon not running:**
```bash
sudo systemctl start docker
sudo systemctl enable docker
```

**Permission denied:**
```bash
sudo usermod -aG docker $USER
# Log out and back in
```

**Port already in use:**
```bash
# Check what's using port 8080
sudo lsof -i :8080
# Kill the process or change ports in docker-compose.demo.yml
```

### Demo Issues

**No visualization window (X11 error):**
```bash
# Option 1: Disable viz
# Edit .env.demo: ENABLE_VIZ=false

# Option 2: Fix X11 permissions
xhost +local:docker
```

**Out of disk space:**
```bash
# Check disk space
df -h

# Clean old data
rm -rf session-data/*

# Clean Docker
docker system prune -a
```

**Containers fail to start:**
```bash
# Check logs
docker compose -f docker-compose.demo.yml logs mri-marshal

# Restart specific service
docker compose -f docker-compose.demo.yml restart mri-marshal

# Full restart
docker compose -f docker-compose.demo.yml down
docker compose -f docker-compose.demo.yml up
```

### Data Issues

**No frames appearing:**
```bash
# Check image-streamer logs
docker compose -f docker-compose.demo.yml logs image-streamer

# Verify marshal is receiving data
curl http://localhost:8080/v1/mrd/latest
```

**Session data directory not found:**
```bash
# Create it manually
mkdir -p session-data/mrd

# Or let the marshal create it on first run
```

### Network Issues

**Cannot connect to marshal from host:**
```bash
# Check container network
docker network ls | grep cwru

# Verify ports are exposed
docker compose -f docker-compose.demo.yml ps

# Check firewall
sudo ufw allow 8080
sudo ufw allow 8081
sudo ufw allow 8090
```

---

## Next Steps After Successful Deployment

### For Developers

1. **Explore the API:**
   - Read `docs/API_REFERENCE.md`
   - Try example curl commands
   - Test WebSocket subscriptions

2. **Write Your Own Client:**
   - See `docs/EXTERNAL_CLIENT_GUIDE.md`
   - Use Python examples in `docs/API_REFERENCE.md`
   - Connect to marshals while demo runs

3. **Modify Configuration:**
   - Adjust `.env.demo` for your use case
   - Test different frame rates and resolutions
   - Run in headless mode for server deployments

### For Production Deployment

⚠️ **CRITICAL: This demo has NO authentication!**

Before production:
1. Add API keys or OAuth
2. Use HTTPS/WSS instead of HTTP/WS
3. Implement rate limiting
4. Add access control lists
5. Enable logging and monitoring
6. Set up backup/recovery for session data
7. Configure firewall rules
8. Use production-grade Docker orchestration (Kubernetes, Docker Swarm)

---

## Key Files Reference

| File | Purpose |
|------|---------|
| `.env.demo` | Demo configuration (frame rates, dimensions, duration) |
| `docker-compose.demo.yml` | Service orchestration |
| `demo-docker.sh` | Quick 30-second demo script |
| `demo-persistent.sh` | Persistent demo script |
| `docs/API_REFERENCE.md` | Complete API documentation |
| `docs/EXTERNAL_CLIENT_GUIDE.md` | Client integration guide |
| `docs/DEMO_AND_API_EXPORT.md` | Demo system overview |
| `session-data/` | Generated MRI data files (HDF5 + JSONL) |

---

## Support & Resources

- **API Documentation:** `docs/API_REFERENCE.md`
- **Client Guide:** `docs/EXTERNAL_CLIENT_GUIDE.md`
- **GitHub Issues:** https://github.com/cwru-mercis/cwru_data_marshal/issues
- **Source Code:** See marshal worktree branches for implementation

---

## Quick Command Reference

```bash
# Start quick demo
./scripts/demo-docker.sh

# Start persistent demo
./scripts/demo-persistent.sh

# Stop all services
docker compose -f docker-compose.demo.yml down

# View logs
docker compose -f docker-compose.demo.yml logs -f

# Check service status
docker compose -f docker-compose.demo.yml ps

# Restart service
docker compose -f docker-compose.demo.yml restart mri-marshal

# Clean everything
docker compose -f docker-compose.demo.yml down
docker system prune -a
rm -rf session-data/

# Health checks
curl http://localhost:8080/health
curl http://localhost:8081/

# API calls
curl http://localhost:8080/v1/mrd/latest | jq
curl http://localhost:8081/read/tip_position_orientation | jq
```

---

**Last Updated:** 2026-01-24
**System Version:** Docker Demo v1.0
