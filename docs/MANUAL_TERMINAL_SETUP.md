# CWRU Data Marshal - Manual Terminal Setup Guide

This guide shows how to run each marshal and client in separate terminals for maximum visibility and control. Perfect for development, debugging, and connecting your own custom clients.

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Initial Setup](#initial-setup)
3. [Environment Variables](#environment-variables)
4. [Running Services in Separate Terminals](#running-services-in-separate-terminals)
5. [Stopping Services](#stopping-services)
6. [Troubleshooting](#troubleshooting)

---

## Prerequisites

### On a Fresh Ubuntu Machine

**Only Docker is required!**

```bash
# Install Docker on Ubuntu
curl -fsSL https://get.docker.com -o get-docker.sh
sudo sh get-docker.sh
sudo usermod -aG docker $USER

# Log out and back in (for group changes to take effect)
```

### Clone and Build

```bash
# Clone the repository
git clone <repo-url>
cd cwru_data_marshal

# Build all Docker images (takes 5-10 minutes)
./scripts/build-client-images.sh

# Verify all 7 images were built
docker images | grep cwru
```

Expected images:
- `cwru/mri-marshal`
- `cwru/robot-marshal`
- `cwru/image-streamer`
- `cwru/ecg-client`
- `cwru/pose-client`
- `cwru/kspace-streamer`
- `cwru/mock-recon`
- `cwru/viz-client`
- `cwru/robot-clients`

---

## Environment Variables

The system uses `.env.demo` for configuration. Here's what each variable controls:

### Demo Control Variables

```bash
# Demo duration in seconds (0 = run indefinitely)
DEMO_DURATION=30

# Cleanup session-data/ folder after demo ends
# Set to false to keep data between runs
CLEANUP_DATA=true
```

### Session Data Location

```bash
# Where to store all generated data (MRI images, ECG, poses)
SESSION_DATA_DIR=./session-data

# Directory structure created:
# session-data/
#   └── run_YYYYMMDD_HHMMSS/
#       └── mrd/
#           ├── demo_stream-64x64x5-g0000.mrd  (MRI images - HDF5)
#           ├── bio.jsonl                       (ECG data)
#           ├── poses.jsonl                     (Pose tracking)
#           ├── index.jsonl                     (Frame metadata)
#           └── latest.json                     (Latest frame pointer)
```

### Image Generation Settings

```bash
# Image dimensions
IMAGE_WIDTH=64        # Pixel width (default: 64)
IMAGE_HEIGHT=64       # Pixel height (default: 64)
IMAGE_SLICES=5        # Number of slices per 3D volume (default: 5)

# Image streaming rate
IMAGE_INTERVAL=0.05   # Seconds between frames (0.05 = 20 fps)

# Image logging
IMAGE_LOG_STRIDE=100  # Log every Nth frame (0 = disable logging)
```

### Biological Signal Settings

```bash
# ECG/biological signal sampling rate
ECG_INTERVAL=0.5      # Seconds between samples (0.5 = 2 Hz)

# Pose tracking update rate
POSE_INTERVAL=0.1     # Seconds between updates (0.1 = 10 Hz)
```

### K-Space Streaming Settings

```bash
# K-Space streaming rate (raw acquisition data)
KSPACE_INTERVAL=100   # Milliseconds between volumes (100ms = 10 volumes/sec)

# Number of slices per volume
KSPACE_SLICES=5       # Slices in each 3D k-space volume (default: 5)
```

### Robot Client Settings

```bash
# How often robot clients communicate with marshal
ROBOT_INTERVAL=0.02   # Seconds between operations (0.02 = 50 Hz)

# Logging frequency for robot clients
ROBOT_LOG_STRIDE=100  # Log every Nth operation (0 = disable)
```

### Visualization Settings

```bash
# Enable/disable visualization window (requires X11)
ENABLE_VIZ=true

# Visualization refresh rate
VIZ_INTERVAL=0.067    # Seconds between frame updates (0.067 ≈ 15 fps)
```

### Monitoring Settings

```bash
# Status monitoring update interval
MONITOR_INTERVAL=2    # Seconds between status updates
```

---

## Running Services in Separate Terminals

This approach gives you maximum visibility - each service runs in its own terminal window so you can see real-time logs.

### ⚠️ Always pass `--env-file .env.demo`

**Every** `docker compose` command in this guide must include `--env-file .env.demo`. If you drop that flag, compose has no values for `RECON_ENDPOINT`, `IMAGE_INTERVAL`, `IMAGE_WIDTH`, `ECG_*`, `POSE_*`, etc., and you will see warnings like:

```
WARN[0000] The "RECON_ENDPOINT" variable is not set. Defaulting to a blank string.
WARN[0000] The "IMAGE_INTERVAL" variable is not set. Defaulting to a blank string.
```

When those warnings appear:

- Marshal starts **without** `--recon-endpoint`, so it returns `501 Not Implemented` to every k-space POST and the k-space path will never produce images.
- Image-streamer / ECG / pose clients receive empty strings for their flags and will error out or behave unpredictably.

If you do not want to type `--env-file .env.demo -f docker-compose.demo.yml` each time, define an alias once per shell:

```bash
alias cdd='docker compose --env-file .env.demo -f docker-compose.demo.yml'
# then: cdd up mri-marshal
```

### Preparation

Before starting services, ensure `session-data/` is clean:

```bash
# Clear old demo data (optional)
rm -rf session-data/*

# Or set CLEANUP_DATA=false in .env.demo to keep data between runs
```

### Which flow to run

There are two valid end-to-end flows. **Pick one; do not mix them in the same session.**

**Flow A — real reconstruction (k-space → recon → image).** The k-space streamer posts raw k-space, marshal forwards it to the reconstruction service, and the reconstructed images flow back via the callback endpoint. This exercises the full scanner-side path.

Required terminals, **run in this order**:

1. Terminal 1 — **mri-marshal** (wait for `healthy`).
2. Terminal 6 — **mock-recon** (the reconstruction service). Must be up before any k-space arrives, otherwise marshal returns `501 Not Implemented`.
3. Terminal 9 — **viz-client** (opens an empty "Waiting for data..." window).
4. Terminal 8 — **kspace-streamer** (starts POSTing raw k-space; images begin appearing in viz a moment later).

Do NOT run image-streamer in Flow A - it posts pre-reconstructed images to the same stream and will fight with the recon callbacks.

**Flow B — bypass reconstruction (pre-made image producer).** The image streamer posts already-reconstructed images directly. Fastest way to smoke-test marshal + viz without touching recon at all.

Required terminals, **run in this order**:

1. Terminal 1 — **mri-marshal**.
2. Terminal 9 — **viz-client**.
3. Terminal 3 — **image-streamer**.

Do NOT run mock-recon or kspace-streamer in Flow B - they are not used.

**Optional add-ons that work with both flows:**

- Terminal 2 — **robot-marshal** + Terminal 7 — **robot-clients** (robot side of the system, independent of the MRI flow).
- Terminal 4 — **ecg-client** (posts synthetic ECG samples).
- Terminal 5 — **pose-client** (posts synthetic tracking poses).

The per-terminal reference sections below give the exact `docker compose` command, expected output, and configuration for each service, in the original numeric order. Refer back to the flow list above for the run sequence.

### Terminal 1: MRI Marshal (Core Service)

The MRI marshal manages all MRI-related data (images, ECG, poses).

```bash
cd /path/to/cwru_data_marshal

# Run MRI marshal with environment variables
docker compose --env-file .env.demo -f docker-compose.demo.yml up mri-marshal
```

**What you'll see:**
```
[MRI Marshal] Starting on http://0.0.0.0:8080, ws://0.0.0.0:8090
[MRI Marshal] Data dir: /session-data/run_20260124_123456
```

**Endpoints:**
- HTTP API: http://localhost:8080
- WebSocket: ws://localhost:8090
- Health check: http://localhost:8080/health

**Leave this running.** Open a new terminal for the next service.

---

### About Reconstruction

The MRI marshal can forward raw k-space data to an external reconstruction service. This is **optional** - the demo works without it since the image streamer sends pre-reconstructed images.

The `cwru/mock-recon` image has historically been a throwaway stand-in for whatever real reconstruction service is wired into production. It is now backed by **python-ismrmrd-server** (vendored) behind a small HTTP->TCP shim, so it performs real inverse-FFT reconstruction on the k-space it receives. The image tag, service name, port (9002), and HTTP contract are unchanged - swapping it for a real scanner-side recon is still a one-line change (`RECON_ENDPOINT=...`, see below).

#### When You Need Reconstruction

- **Demo/Testing**: NOT needed - image streamer sends reconstructed images (ImageHeader)
- **Real Scanner**: NEEDED - scanners send raw k-space (AcquisitionHeader)

#### Swapping the Simulator for Real Recon

The simulator (`cwru/mock-recon`) is designed to be a drop-in placeholder. To use a real reconstruction service instead:

1. Stop the `mock-recon` container.
2. Start marshal with `RECON_ENDPOINT=http://<real-recon-host>:<port>` pointing at the real service.

Nothing else changes - marshal, viz-client, kspace-streamer, and the manual terminal commands in this guide all keep working as-is. The contract marshal uses (`POST /reconstruct` out, callback to `POST /v1/mrd/callback` in) is identical regardless of what service is behind the endpoint.

#### Data Flow with Reconstruction (Async/Callback)

```
K-Space Streamer                  MRI Marshal                    Recon Service
     │                                 │                              │
     │ POST /v1/mrd/frame              │                              │
     │ (raw k-space)                   │                              │
     │────────────────────────────────>│                              │
     │                                 │ Detects ACQUISITION          │
     │                                 │                              │
     │ HTTP 202 Accepted               │                              │
     │ (immediate response ~7-16ms)    │                              │
     │<────────────────────────────────│                              │
     │                                 │                              │
     │                                 │ [Detached Thread]            │
     │                                 │ POST /reconstruct            │
     │                                 │ X-MRD-Callback: http://...   │
     │                                 │ X-MRD-Job-Id: xxx            │
     │                                 │─────────────────────────────>│
     │                                 │                              │
     │                                 │ HTTP 202 Accepted            │
     │                                 │<─────────────────────────────│
     │                                 │                              │
     │                                 │                              │
     │                                 │   [Background Processing]    │
     │                                 │   (~0.5s simulated delay)    │
     │                                 │                              │
     │                                 │ POST /v1/mrd/callback        │
     │                                 │ (reconstructed image)        │
     │                                 │<─────────────────────────────│
     │                                 │                              │
     │                                 │ Store to SWMR                │
     │                                 │ WebSocket emit "mrd"         │
     │                                 │ HTTP 200 OK                  │
     │                                 │─────────────────────────────>│
```

**Key Features:**
- **Non-blocking**: K-space sender gets HTTP 202 immediately (doesn't wait for reconstruction)
- **Callback-based**: Recon service POSTs result back when done via `/v1/mrd/callback`
- **Event-driven**: Viz clients get WebSocket notification when image arrives (no polling needed)
- **Concurrent**: Multiple k-space frames can reconstruct simultaneously

**Note:** To test the full reconstruction flow, start the mock-recon service (Terminal 6) and k-space streamer (Terminal 8). The `RECON_ENDPOINT` is already configured in `.env.demo`.

### Clearing the Latest Frame Cache (Session End)

Marshal caches the most recent stored frame in memory so `GET /v1/mrd/latest` can serve it instantly. The cache is sticky for the lifetime of the marshal process - after a producer stops, the last frame keeps showing up until marshal is restarted or the cache is explicitly cleared.

To explicitly clear it, call:

```bash
curl -X DELETE http://localhost:8080/v1/mrd/latest
```

Effects:

- Next `GET /v1/mrd/latest` returns `204 No Content` until the next frame is POSTed.
- HDF5 files on disk, `/v1/mrd/since` history, WebSocket subscribers, and every other marshal state are unaffected - only the in-memory "latest" cache is wiped.
- The coordinator safety poller (`clients/bridge/coordinator.py`) is unaffected: it ignores non-200 responses and only acts on observed fault envelopes, so a cleared cache just means it waits for the next real frame.

Typical callers:

- An operator running the curl above manually at end-of-experiment.
- A session orchestration script in a `trap` / `finally` block.
- A scanner / producer shutdown hook that fires on Ctrl-C.

The endpoint is purely additive and optional: if nobody calls it, marshal behaves exactly as before.

---

### Terminal 2: Robot Marshal (Core Service)

The robot marshal manages robot control data exchange.

```bash
cd /path/to/cwru_data_marshal

# Run Robot marshal
docker compose --env-file .env.demo -f docker-compose.demo.yml up robot-marshal
```

**What you'll see:**
```
[Robot Marshal] Server listening on http://0.0.0.0:8081
[Robot Marshal] Available endpoints:
  - GET/POST /read/tip_position_orientation
  - GET/POST /write/user_input
  ...
```

**Endpoints:**
- HTTP API: http://localhost:8081
- Browse available channels: http://localhost:8081/

**Leave this running.** Open a new terminal.

---

### Terminal 3: Image Streamer (MRI Data Generator)

Generates synthetic MRI image frames and sends them to the MRI marshal.

```bash
cd /path/to/cwru_data_marshal

# Run image streamer
docker compose --env-file .env.demo -f docker-compose.demo.yml up image-streamer
```

**What you'll see (if IMAGE_LOG_STRIDE > 0):**
```
[image-streamer] Sent frame 0
[image-streamer] Sent frame 100
[image-streamer] Sent frame 200
...
```

**Configuration:**
- Frame rate: `IMAGE_INTERVAL` (default: 0.05s = 20 fps)
- Dimensions: `IMAGE_WIDTH` x `IMAGE_HEIGHT` x `IMAGE_SLICES`
- Logging: Every `IMAGE_LOG_STRIDE` frames

---

### Terminal 4: ECG Client (Biological Signal Generator)

Generates synthetic ECG/biological signals.

```bash
cd /path/to/cwru_data_marshal

# Run ECG client
docker compose --env-file .env.demo -f docker-compose.demo.yml up ecg-client
```

**What you'll see:**
```
[ecg-client] ❤️  HR: 72 bpm, ECG: 0.523
[ecg-client] ❤️  HR: 73 bpm, ECG: 0.612
...
```

**Configuration:**
- Sample rate: `ECG_INTERVAL` (default: 0.5s = 2 Hz)
- Sends to: http://mri-marshal:8080/v1/bio/signal

---

### Terminal 5: Pose Client (Tracking Data Generator)

Generates synthetic pose/tracking data.

```bash
cd /path/to/cwru_data_marshal

# Run pose client
docker compose --env-file .env.demo -f docker-compose.demo.yml up pose-client
```

**What you'll see:**
```
[pose-client] 📍 Pose: pos=[1.2, 3.4, 5.6], ori=[0.0, 0.0, 0.707, 0.707]
[pose-client] 📍 Pose: pos=[1.3, 3.5, 5.7], ori=[0.0, 0.0, 0.708, 0.706]
...
```

**Configuration:**
- Update rate: `POSE_INTERVAL` (default: 0.1s = 10 Hz)
- Sends to: http://mri-marshal:8080/v1/pose/update

---

### Terminal 6: Reconstruction Service (cwru/mock-recon)

Runs the reconstruction service that marshal forwards k-space to. Despite the legacy image name `cwru/mock-recon`, this is now real reconstruction (`python-ismrmrd-server` + HTTP->TCP shim) rather than a gradient stub. Required if you want to test the full reconstruction pipeline with the k-space streamer.

```bash
cd /path/to/cwru_data_marshal

# Run reconstruction service
docker compose --env-file .env.demo -f docker-compose.demo.yml up mock-recon
```

**What you'll see:**
```
[entrypoint] starting python-ismrmrd-server on 0.0.0.0:9004
[entrypoint] starting shim on 0.0.0.0:9002
[shim] shim listening on :9002, forwards to python-ismrmrd-server 127.0.0.1:9004
```

**Endpoints:**
- Reconstruct: POST http://localhost:9002/reconstruct
- Health: GET http://localhost:9002/health

**How it works:**
1. Receives k-space data with `X-MRD-Callback` header on HTTP `/reconstruct`.
2. Returns HTTP 202 immediately (non-blocking).
3. In a background thread: parses the acquisitions, opens a TCP connection to `python-ismrmrd-server` on port 9004 (inside the container), sends CONFIG + metadata XML + acquisition stream.
4. `python-ismrmrd-server` runs `simplefft` (real 2D inverse FFT per slice) and streams images back.
5. The shim strips the attribute envelope, combines multi-slice outputs into a single 3D ImageHeader+pixels, and POSTs it to the callback URL.

**Note:** The `RECON_ENDPOINT` environment variable in `.env.demo` is already configured to point to this service. To use a real scanner-side reconstruction service instead, stop this container and set `RECON_ENDPOINT=http://<real-recon-host>:<port>` when starting marshal (Terminal 1) - nothing else in this guide changes.

---

### Terminal 7: Robot Clients (All 5 Together)

Runs all 5 robot clients in a single container:
- Catheter tracking
- Controller
- Planning
- Front-end
- Surface tracking

```bash
cd /path/to/cwru_data_marshal

# Run all robot clients
docker compose --env-file .env.demo -f docker-compose.demo.yml up robot-clients
```

**What you'll see (if ROBOT_LOG_STRIDE > 0):**
```
[robot-clients] CATHETER: Read tip_position_orientation
[robot-clients] CONTROLLER: Read desired_planned_motion
[robot-clients] PLANNING: Wrote desired_planned_motion
[robot-clients] FRONTEND: Wrote user_input
[robot-clients] SURFACE: Read streaming_2D_images
...
```

**Configuration:**
- Operation rate: `ROBOT_INTERVAL` (default: 0.02s = 50 Hz)
- Logging: Every `ROBOT_LOG_STRIDE` operations
- Communicates with: http://robot-marshal:8081

---

### Terminal 8: K-Space Streamer (Raw K-Space Data Generator)

Generates synthetic raw k-space data and sends it to the MRI marshal. When reconstruction is configured, the marshal forwards this data to the reconstruction service.

```bash
cd /path/to/cwru_data_marshal

# Run k-space streamer
docker compose --env-file .env.demo -f docker-compose.demo.yml up kspace-streamer
```

**What you'll see:**
```
[kspace-streamer] Starting
[kspace-streamer]   slices/volume: 5
[kspace-streamer]   interval: 0.1s
[kspace-streamer] volume 0 (5 slices) -> HTTP 202
[kspace-streamer] volume 10 (5 slices) -> HTTP 202
...
```

**Configuration:**
- Frame rate: `KSPACE_INTERVAL` (default: 100ms = 10fps)
- Slices per volume: `KSPACE_SLICES` (default: 5)
- Sends multi-slice raw AcquisitionHeader data to: http://mri-marshal:8080/v1/mrd/frame

**Note:** To test the full reconstruction flow, ensure the mock-recon service (Terminal 6) is running. The marshal will automatically forward k-space data to the reconstruction endpoint.

---

### Terminal 9: Visualization Client (Optional - Requires X11)

Displays MRI frames in real-time.

**Prerequisites:**
- X11 display server running
- `ENABLE_VIZ=true` in `.env.demo`
- `DISPLAY` environment variable set (e.g., `:0`)

**WSL2 Users:**
- Install VcXsrv, Xming, or use WSLg
- Allow X11 connections

```bash
cd /path/to/cwru_data_marshal

# Run visualization client (requires --profile viz)
docker compose --env-file .env.demo -f docker-compose.demo.yml --profile viz up viz-client
```

**What you'll see:**
- OpenCV window displaying MRI slices in real-time
- Console output with frame information

**Configuration:**
- Refresh rate: `VIZ_INTERVAL` (default: 0.067s ≈ 15 fps)
- Reads from: http://mri-marshal:8080/v1/mrd/latest

**Note:** Display FPS is lower than actual data ingestion FPS due to Docker X11 forwarding overhead. The marshals still process data at full speed (20+ fps).

---

## Customizing Environment Variables

### Option 1: Edit `.env.demo` File

```bash
nano .env.demo
```

Change values, save, then run services with:
```bash
docker compose --env-file .env.demo -f docker-compose.demo.yml up <service>
```

### Option 2: Override on Command Line

```bash
# Run with custom image size and faster frame rate
IMAGE_WIDTH=128 IMAGE_HEIGHT=128 IMAGE_INTERVAL=0.025 \
  docker compose --env-file .env.demo -f docker-compose.demo.yml up image-streamer

# Run ECG client with faster sampling
ECG_INTERVAL=0.1 \
  docker compose --env-file .env.demo -f docker-compose.demo.yml up ecg-client

# Run indefinitely (no time limit)
DEMO_DURATION=0 \
  docker compose --env-file .env.demo -f docker-compose.demo.yml up mri-marshal
```

### Option 3: Create Custom .env File

```bash
cp .env.demo .env.custom

# Edit .env.custom
nano .env.custom

# Use it
docker compose --env-file .env.custom -f docker-compose.demo.yml up <service>
```

---

## Accessing the APIs While Running

### MRI Marshal API

```bash
# Health check
curl http://localhost:8080/health

# Get latest MRI frame metadata
curl http://localhost:8080/v1/mrd/latest | jq

# Get latest ECG data
curl http://localhost:8080/v1/bio/latest | jq

# Get current pose
curl http://localhost:8080/v1/pose/current | jq
```

### Robot Marshal API

```bash
# List all available channels
curl http://localhost:8081/

# Read catheter tip position
curl http://localhost:8081/read/tip_position_orientation | jq

# Write user command
curl -X POST http://localhost:8081/write/user_input \
  -H "Content-Type: application/json" \
  -d '{"values": [10.0, 20.0, 30.0], "sent_at": 1706126625123456789}'
```

### WebSocket Notifications (Real-time Push)

The marshal emits WebSocket events when new data arrives - this is the **recommended way** for viz clients to know when to fetch new frames (instead of polling).

```bash
# Connect to WebSocket (using wscat)
npm install -g wscat
wscat -c ws://localhost:8090

# You'll receive notifications automatically when data arrives:
< {"type":"mrd","stream":"demo_stream","frame_index":0,"path":"/session-data/...","dims":{...}}
< {"type":"mrd","stream":"demo_stream","frame_index":1,"path":"/session-data/...","dims":{...}}
```

**For Viz Clients (JavaScript/Python):**

```javascript
// JavaScript example
const ws = new WebSocket('ws://mri-marshal:8090');

ws.onmessage = (event) => {
  const data = JSON.parse(event.data);
  if (data.type === 'mrd') {
    console.log('New image available:', data.frame_index);
    // Fetch the actual image
    fetch(`http://mri-marshal:8080/v1/mrd/latest`)
      .then(r => r.json())
      .then(img => displayImage(img));
  }
};
```

```python
# Python example
import websocket
import json

def on_message(ws, message):
    data = json.loads(message)
    if data.get('type') == 'mrd':
        print(f"New image: frame {data['frame_index']}")
        # Fetch and display image

ws = websocket.WebSocketApp('ws://mri-marshal:8090',
                            on_message=on_message)
ws.run_forever()
```

**Why WebSocket over polling?**
- ✅ **Real-time**: Instant notification when frames arrive
- ✅ **Efficient**: No wasted requests (90% waste at 50ms polling with 0.5s recon)
- ✅ **Scalable**: Works with any frame rate (0.5 fps to 100+ fps)
- ✅ **Low latency**: 0ms delay vs 0-50ms average with polling

---

## Viewing Generated Data

Data is stored in `session-data/`:

```bash
# View data structure
tree session-data/

# Expected output:
# session-data/
# └── run_20260124_123456/
#     └── mrd/
#         ├── demo_stream-64x64x5-g0000.mrd  # MRI images (HDF5)
#         ├── bio.jsonl                       # ECG data
#         ├── poses.jsonl                     # Pose tracking
#         ├── index.jsonl                     # Frame metadata
#         └── latest.json                     # Latest frame pointer

# View MRI file info (requires h5dump)
h5dump -H session-data/run_*/mrd/*.mrd | head -50

# View ECG data
cat session-data/run_*/mrd/bio.jsonl | jq

# View pose data
cat session-data/run_*/mrd/poses.jsonl | jq

# Check latest frame
cat session-data/run_*/mrd/latest.json | jq
```

---

## Stopping Services

### Stop Individual Service

In the terminal running the service, press:
```
Ctrl+C
```

### Stop All Services at Once

From any terminal:
```bash
cd /path/to/cwru_data_marshal

# Stop all demo services
docker compose -f docker-compose.demo.yml down
```

### Clean Up Everything

```bash
# Stop all services
docker compose -f docker-compose.demo.yml down

# Remove all stopped containers
docker rm $(docker ps -aq)

# Remove session data
rm -rf session-data/*

# Remove Docker images (optional - you'll need to rebuild)
docker rmi cwru/mri-marshal cwru/robot-marshal \
  cwru/image-streamer cwru/ecg-client cwru/pose-client \
  cwru/viz-client cwru/robot-clients
```

---

## Troubleshooting

### Port Already in Use

```bash
# Find what's using the port
sudo lsof -i :8080
sudo lsof -i :8081

# Stop conflicting services
docker compose -f docker-compose.demo.yml down
```

### Container Not Starting

```bash
# Check logs
docker compose -f docker-compose.demo.yml logs mri-marshal
docker compose -f docker-compose.demo.yml logs robot-marshal

# Check container status
docker ps -a | grep cwru
```

### Visualization Window Not Showing

```bash
# Check DISPLAY variable
echo $DISPLAY
# Should show something like ":0" or ":1"

# Test X11 (requires x11-apps)
xeyes

# WSL2: Ensure X server is running on Windows
# - VcXsrv: Launch XLaunch
# - Or use WSLg (Windows 11)

# Check .env.demo
grep ENABLE_VIZ .env.demo
# Should be: ENABLE_VIZ=true
```

### Session Data Directory Issues

```bash
# Permission denied
sudo chown -R $USER:$USER session-data/

# Device or resource busy
# This means it's mounted - delete contents instead:
rm -rf session-data/*
```

### Services Can't Communicate

```bash
# Check Docker network
docker network ls | grep cwru-net

# Verify all services are on same network
docker compose -f docker-compose.demo.yml ps

# Restart with clean network
docker compose -f docker-compose.demo.yml down
docker network prune -f
docker compose -f docker-compose.demo.yml up
```

### Images Not Built

```bash
# Build all images
./scripts/build-client-images.sh

# Or build specific image
docker compose -f docker-compose.demo.yml build mri-marshal
```

---

## Advanced Usage

### Run Services in Background (Daemon Mode)

```bash
# Start all in background
docker compose --env-file .env.demo -f docker-compose.demo.yml up -d

# View logs in real-time
docker compose -f docker-compose.demo.yml logs -f

# View logs from specific service
docker compose -f docker-compose.demo.yml logs -f mri-marshal
```

### Restart Specific Service

```bash
# Without stopping others
docker compose -f docker-compose.demo.yml restart mri-marshal
```

### Scale Robot Clients (Advanced)

```bash
# Run multiple instances of robot clients
docker compose -f docker-compose.demo.yml up --scale robot-clients=3
```

---

## Summary

**Minimum Required Terminals: 2**
- Terminal 1: MRI Marshal
- Terminal 2: Robot Marshal

**Full Demo Setup: 8-9 Terminals**
1. MRI Marshal (required)
2. Robot Marshal (required)
3. Image Streamer (generates MRI data)
4. ECG Client (generates biological signals)
5. Pose Client (generates tracking data)
6. Mock Reconstruction Service (simulates image reconstruction)
7. Robot Clients (5 robot components)
8. K-Space Streamer (generates raw k-space data)
9. Viz Client (optional - displays images)

**Key Points:**
- All services use `--env-file .env.demo` for configuration
- Each terminal shows real-time logs for that service
- Services communicate over Docker network `cwru-net`
- Data persists in `session-data/` directory
- Stop any service with `Ctrl+C`
- Stop all services with `docker compose down`

---

**For API documentation, see:** `docs/API_REFERENCE.md`

**For external client integration, see:** `docs/EXTERNAL_CLIENT_GUIDE.md`
