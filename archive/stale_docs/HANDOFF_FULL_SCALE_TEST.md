# Handoff: Full-Scale System Test with Reconstruction

**Date:** 2026-01-30
**Branch:** `feature/reconstruction-client-improvements`
**Latest Commit:** `6e11f7c` - Add k-space streamer client for testing reconstruction flow

---

## Task for Next Agent

Run a full-scale integration test of the entire CWRU Data Marshal system with all components:
- MRI Marshal (with reconstruction enabled)
- Robot Marshal
- ECG Client
- Pose Client
- Robot Clients
- K-Space Streamer (raw data → reconstruction)
- Mock Reconstruction Service
- Viz Client (optional)

---

## Prerequisites

```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal

# Build all binaries
cmake --build build -j4

# Verify builds exist
ls -la build/marshal build/kspace_streamer build/image_streamer build/viz_client 2>/dev/null
```

---

## Terminal Setup (8 Terminals)

### Terminal 0: Mock Reconstruction Service

```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal

# Install flask if needed
pip install flask -q

# Start mock reconstruction service
python3 tests/mock_recon_service.py
```

**Expected output:**
```
[mock-recon] Starting mock reconstruction service on port 9002
[mock-recon] Endpoints:
[mock-recon]   POST /reconstruct - Receive k-space, return reconstructed image
[mock-recon]   GET /health - Health check
```

**Leave running.** The reconstruction service receives raw k-space and returns reconstructed images.

---

### Terminal 1: MRI Marshal (with Reconstruction)

```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal

# Clean data directory
rm -rf ./data/mrd && mkdir -p ./data/mrd

# Start marshal with reconstruction endpoint
./build/marshal --data ./data --recon-endpoint http://localhost:9002
```

**Expected output:**
```
marshal listening http=0.0.0.0:8080 ws=0.0.0.0:8090 data=./data ... recon_endpoint=http://localhost:9002
```

**Endpoints:**
- HTTP API: http://localhost:8080
- WebSocket: ws://localhost:8090
- Health: http://localhost:8080/health

---

### Terminal 2: Robot Marshal

```bash
cd /workspaces/cwru_data_marshal

# Use Docker Compose for robot marshal
docker compose --env-file .env.demo -f docker-compose.demo.yml up robot-marshal
```

**Or native (if available):**
```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal/clients
# Robot marshal is typically a separate binary or Python service
```

**Expected output:**
```
[Robot Marshal] Server listening on http://0.0.0.0:8081
```

**Endpoints:**
- HTTP API: http://localhost:8081

---

### Terminal 3: K-Space Streamer (Raw Data Generator)

```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal

# Stream raw k-space data (triggers reconstruction)
./build/kspace_streamer \
  --http http://localhost:8080 \
  --stream raw_scan \
  --samples 256 \
  --channels 8 \
  --lines 64 \
  --interval 0.1 \
  --log-stride 10
```

**Expected output:**
```
kspace_streamer: Starting
  marshal: http://localhost:8080
  stream: raw_scan
  samples: 256
  channels: 8
  lines/frame: 64
  interval: 0.1s
kspace_streamer: connected (startup)
readout 0 (line 0/64, frame 0) -> HTTP 201
readout 10 (line 10/64, frame 0) -> HTTP 201
...
```

**Data flow:**
1. K-space streamer sends `AcquisitionHeader` + complex k-space data
2. Marshal detects type as `ACQUISITION`
3. Marshal forwards to reconstruction service (`POST /reconstruct`)
4. Reconstruction service returns `ImageHeader` + pixels
5. Marshal stores reconstructed image to SWMR file

---

### Terminal 4: ECG Client

```bash
cd /workspaces/cwru_data_marshal

# Use Docker Compose
docker compose --env-file .env.demo -f docker-compose.demo.yml up ecg-client
```

**Expected output:**
```
[ecg-client] ❤️  HR: 72 bpm, ECG: 0.523
[ecg-client] ❤️  HR: 73 bpm, ECG: 0.612
...
```

**Sends to:** http://mri-marshal:8080/v1/bio/signal (or localhost:8080 if native)

---

### Terminal 5: Pose Client

```bash
cd /workspaces/cwru_data_marshal

# Use Docker Compose
docker compose --env-file .env.demo -f docker-compose.demo.yml up pose-client
```

**Expected output:**
```
[pose-client] 📍 Pose: pos=[1.2, 3.4, 5.6], ori=[0.0, 0.0, 0.707, 0.707]
...
```

**Sends to:** http://mri-marshal:8080/v1/pose/update

---

### Terminal 6: Robot Clients

```bash
cd /workspaces/cwru_data_marshal

# Use Docker Compose
docker compose --env-file .env.demo -f docker-compose.demo.yml up robot-clients
```

**Expected output:**
```
[robot-clients] CATHETER: Read tip_position_orientation
[robot-clients] CONTROLLER: Read desired_planned_motion
[robot-clients] PLANNING: Wrote desired_planned_motion
...
```

**Communicates with:** http://robot-marshal:8081

---

### Terminal 7: Viz Client (Optional - Requires X11)

```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal

# Native viz client (reads from SWMR files)
./build/viz_client --http http://localhost:8080 --interval 0.1
```

**Or via Docker:**
```bash
cd /workspaces/cwru_data_marshal
docker compose --env-file .env.demo -f docker-compose.demo.yml --profile viz up viz-client
```

**Prerequisites:**
- X11 display (set `DISPLAY` environment variable)
- WSL2 users: VcXsrv, Xming, or WSLg

---

## Verification Checks

### 1. Health Checks

```bash
# MRI Marshal
curl -s http://localhost:8080/health | jq

# Robot Marshal
curl -s http://localhost:8081/ | jq

# Mock Reconstruction
curl -s http://localhost:9002/health | jq
```

### 2. Data Flow Verification

```bash
# Check latest MRI frame (should show reconstructed data)
curl -s http://localhost:8080/v1/mrd/latest | jq

# Check latest bio signal
curl -s http://localhost:8080/v1/bio/latest | jq

# Check current pose
curl -s http://localhost:8080/v1/pose/current | jq
```

### 3. File System Verification

```bash
# Check generated files
ls -la ./data/mrd/

# Expected files:
# - raw_scan-64x64x1-g0000.mrd  (reconstructed images - SWMR HDF5)
# - bio.jsonl                    (ECG signals)
# - poses.jsonl                  (pose tracking)
# - index.jsonl                  (frame metadata)
# - latest.json                  (latest frame pointer)

# View index entries
tail -5 ./data/mrd/index.jsonl | jq

# Check HDF5 file
h5dump -H ./data/mrd/raw_scan-*.mrd | head -30
```

### 4. Reconstruction Flow Verification

Watch Terminal 0 (mock recon) for:
```
[mock-recon] Received 16724 bytes of k-space data
[mock-recon] Stream: raw_scan
[mock-recon] Returning 16582 bytes (198 header + 16384 pixels)
[mock-recon] Image dimensions: 64x64x1, channels=1, type=FLOAT
```

Watch Terminal 1 (marshal) for:
```
[marshal_http] Detected MRD type: ACQUISITION (stream=raw_scan, size=16724 bytes)
[marshal_http] Forwarding raw k-space to reconstruction service: http://localhost:9002
[marshal_http] Reconstruction service responded: HTTP 200
[marshal_http] Stored reconstructed image: stream=raw_scan, frame=N, path=...
```

---

## Expected Data Rates

| Component | Rate | Notes |
|-----------|------|-------|
| K-Space Streamer | 10 Hz (0.1s interval) | Each readout triggers reconstruction |
| ECG Client | 2 Hz (0.5s interval) | Configurable via `ECG_INTERVAL` |
| Pose Client | 10 Hz (0.1s interval) | Configurable via `POSE_INTERVAL` |
| Robot Clients | 50 Hz (0.02s interval) | Configurable via `ROBOT_INTERVAL` |
| Reconstruction | ~10 Hz | Limited by mock service |
| Viz Client | 15 Hz (0.067s interval) | Display refresh |

---

## Troubleshooting

### Reconstruction Not Working

```bash
# Check if recon service is running
curl -s http://localhost:9002/health

# Check marshal was started with --recon-endpoint
# Should see: recon_endpoint=http://localhost:9002 in startup output

# If marshal returns 501, reconstruction not configured
# Restart marshal with: --recon-endpoint http://localhost:9002
```

### K-Space Streamer Getting 502 Errors

```bash
# Check mock recon service logs for errors
# Common issues:
# - Port 9002 not listening
# - Flask not installed (pip install flask)
# - Service crashed (restart it)
```

### No Data in SWMR Files

```bash
# Check if marshal is receiving data
curl -s http://localhost:8080/v1/mrd/latest | jq

# Check index.jsonl for entries
cat ./data/mrd/index.jsonl | wc -l

# Verify HDF5 file has frames
h5ls ./data/mrd/*.mrd
```

### Clients Can't Connect

```bash
# For Docker clients, ensure network is correct
docker network ls | grep cwru

# For native clients, ensure marshal is on localhost:8080
curl -s http://localhost:8080/health
```

---

## Stopping Everything

```bash
# Stop all Docker services
cd /workspaces/cwru_data_marshal
docker compose -f docker-compose.demo.yml down

# Kill native processes
pkill -f mock_recon_service.py
pkill -f "marshal --data"
pkill -f kspace_streamer
pkill -f viz_client

# Clean up data
rm -rf ./data/mrd/*
```

---

## Success Criteria

1. ✅ Mock reconstruction service running on port 9002
2. ✅ MRI Marshal accepting connections with reconstruction enabled
3. ✅ K-space streamer sending data, receiving HTTP 201
4. ✅ Reconstruction service processing requests (visible in logs)
5. ✅ SWMR files being created with reconstructed images
6. ✅ ECG and Pose data being logged
7. ✅ Robot clients communicating with robot marshal
8. ✅ Viz client displaying frames (if X11 available)

---

## Files Reference

| File | Purpose |
|------|---------|
| `build/marshal` | MRI Marshal binary |
| `build/kspace_streamer` | Raw k-space data generator |
| `build/image_streamer` | Reconstructed image generator (alternative) |
| `build/viz_client` | Visualization client |
| `tests/mock_recon_service.py` | Mock reconstruction service |
| `.env.demo` | Environment configuration |
| `docker-compose.demo.yml` | Docker service definitions |

---

## Alternative: Image Streamer (No Reconstruction)

If you want to test without reconstruction, use `image_streamer` instead:

```bash
# Terminal 3 alternative: Image Streamer (sends reconstructed images directly)
./build/image_streamer \
  --http http://localhost:8080 \
  --stream demo_stream \
  --size 64 \
  --slices 5 \
  --interval 0.05 \
  --log-stride 100
```

This bypasses reconstruction and sends `ImageHeader` data directly.

---

**Ready for full-scale testing!**
