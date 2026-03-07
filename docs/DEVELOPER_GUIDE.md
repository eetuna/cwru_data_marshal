# CWRU Data Marshal — Developer Guide

How the repository is organized, how to work with branches and worktrees, and how to replace mock clients with real ones.

---

## Branch Architecture

```
main                          ← Generic marshal server (shared core)
├── mri-data-marshal          ← MRI-specific clients (image streamer, viz, k-space)
└── robot-data-marshal        ← Robot-specific clients (catheter, controller, planning)
```

| Branch | What it contains | Who works on it |
|--------|-----------------|-----------------|
| `main` | Marshal HTTP server, Docker configs, scripts, docs | Everyone (shared) |
| `mri-data-marshal` | MRI image streamer, k-space streamer, viz client, ECG/pose mock clients | MRI team |
| `robot-data-marshal` | Catheter tracking, controller, planning, front-end, surface tracking | Robot team |

**Rules:**
- Shared server changes go into `main`, then merge `main` into domain branches
- Domain branches never merge back into `main` — they only diverge with their own clients
- New domain (e.g. ultrasound) = new branch off `main`

---

## Worktree Setup

Git worktrees let you have all branches checked out simultaneously in separate directories, so you can build and run everything without switching branches.

### Directory Layout

```
/workspaces/cwru_data_marshal/                        ← main (checked out here)
/workspaces/cwru_data_marshal/.worktrees/
    ├── mri_data_marshal/                             ← mri-data-marshal branch
    └── robot_data_marshal/                           ← robot-data-marshal branch
```

### Creating Worktrees

```bash
# From the repo root (main branch)
git worktree add .worktrees/mri_data_marshal mri-data-marshal
git worktree add .worktrees/robot_data_marshal robot-data-marshal
```

Or just run the build script — it creates worktrees automatically:

```bash
./scripts/build-client-images.sh
```

### Managing Worktrees

```bash
# List active worktrees
git worktree list

# Remove a worktree
git worktree remove .worktrees/mri_data_marshal

# Clean up stale references
git worktree prune
```

### How Docker Builds Use Worktrees

The build script (`scripts/build-client-images.sh`) does:

1. Creates worktrees for `mri-data-marshal` and `robot-data-marshal`
2. Builds MRI images from the MRI worktree using Dockerfiles in `docker/`
3. Builds Robot images from the Robot worktree using Dockerfiles in `docker/`

```
main branch (docker/Dockerfile.mri)  +  mri-data-marshal worktree (source code)  →  cwru/mri-marshal image
main branch (docker/Dockerfile.robot) +  robot-data-marshal worktree (source code) →  cwru/robot-marshal image
```

Dockerfiles live on `main`. Source code lives on domain branches. Worktrees bridge them.

---

## System Overview

```
┌─────────────────┐     POST /v1/mrd/frame      ┌──────────────────────┐
│  Your Client    │ ──────────────────────────── │  Marshal Server      │
│  (MRI, Robot,   │     POST /v1/bio/signal      │  :8080 HTTP          │
│   or custom)    │     POST /v1/pose/update      │  :8090 WebSocket     │
└─────────────────┘     GET  /v1/mrd/latest       └──────────┬───────────┘
                                                             │
                                                    Stores to disk
                                                             │
                                                  ┌──────────▼───────────┐
                                                  │  /session-data/      │
                                                  │  ├── *.mrd (HDF5)    │
                                                  │  ├── bio.jsonl       │
                                                  │  └── poses.jsonl     │
                                                  └──────────────────────┘
```

The marshal is a generic HTTP server. It accepts binary MRI data, ECG signals, and pose data. It stores everything to disk. Clients read back via HTTP or direct HDF5 file access.

For the full HTTP flow with reconstruction routing, see [SYSTEM_DIAGRAM_COMPLETE.md](../SYSTEM_DIAGRAM_COMPLETE.md).

For detailed request/response examples of every endpoint, see [HTTP_ROUTING_EXAMPLES.md](../HTTP_ROUTING_EXAMPLES.md).

---

## Replacing Mock Clients with Real Ones

The demo ships with mock clients that generate synthetic data. To integrate real hardware or software, you replace these mocks on your domain branch.

### What Are the Mock Clients?

| Mock Client | What it does | Docker image | Branch |
|-------------|-------------|--------------|--------|
| `image_streamer` | Generates synthetic MRI frames, POSTs to `/v1/mrd/frame` | `cwru/image-streamer` | `mri-data-marshal` |
| `kspace_streamer` | Generates synthetic k-space data, POSTs to `/v1/mrd/frame` | `cwru/kspace-streamer` | `mri-data-marshal` |
| `ecg_client` | Generates synthetic ECG, POSTs to `/v1/bio/signal` | `cwru/ecg-client` | `mri-data-marshal` |
| `pose_client` | Generates synthetic poses, POSTs to `/v1/pose/update` | `cwru/pose-client` | `mri-data-marshal` |
| `viz_client` | Reads HDF5/SWMR files, displays with OpenCV | `cwru/viz-client` | `mri-data-marshal` |
| `catheter-tracking` | Simulates catheter position updates | `cwru/robot-clients` | `robot-data-marshal` |
| `controller` | Simulates robot controller | `cwru/robot-clients` | `robot-data-marshal` |
| `planning` | Simulates motion planning | `cwru/robot-clients` | `robot-data-marshal` |
| `front-end` | Simulates user interface | `cwru/robot-clients` | `robot-data-marshal` |
| `surface-tracking` | Simulates surface tracking | `cwru/robot-clients` | `robot-data-marshal` |

### Steps to Replace a Mock

**Example: Replace the mock `image_streamer` with a real MRI scanner bridge**

1. **Work on your domain branch:**
   ```bash
   cd .worktrees/mri_data_marshal
   # or: git checkout mri-data-marshal
   ```

2. **Understand the interface the mock uses.** The image streamer POSTs binary data to the marshal:
   ```
   POST /v1/mrd/frame
   Header: X-MRD-Stream: <stream_name>
   Body: ImageHeader (198 bytes) + pixel data
   ```
   The marshal auto-detects the data type from the binary header. Your real client just needs to POST in the same format.

3. **Write your real client.** It can be any language. It just needs to make HTTP POST requests:
   ```python
   # Example: Real scanner bridge (Python)
   import requests
   import struct

   MARSHAL = "http://mri-marshal:8080"

   def send_frame(image_header_bytes, pixel_data):
       resp = requests.post(
           f"{MARSHAL}/v1/mrd/frame",
           headers={
               "X-MRD-Stream": "cardiac_scan",
               "Content-Type": "application/octet-stream"
           },
           data=image_header_bytes + pixel_data
       )
       return resp.status_code == 200
   ```

4. **Update the Dockerfile** (on `main`, in `docker/`):
   ```dockerfile
   # docker/Dockerfile.my-scanner
   FROM python:3.11-slim
   COPY my_scanner_bridge.py /app/
   CMD ["python3", "/app/my_scanner_bridge.py"]
   ```

5. **Update `docker-compose.demo.yml`** to use your new image instead of the mock:
   ```yaml
   image-streamer:
     image: cwru/my-scanner:latest    # was cwru/image-streamer
     command: ["python3", "/app/my_scanner_bridge.py"]
   ```

6. **The marshal server does not change.** It accepts data based on binary headers, not client identity.

### Example: Replace viz_client

The viz client reads reconstructed images and displays them. To replace it:

1. The mock viz client reads HDF5/SWMR files directly from the shared volume and polls `/v1/mrd/latest` for metadata.

2. Your real visualization can either:
   - **Read HDF5 directly** (mount the same `session-data` volume, open with `swmr=True`)
   - **Poll the HTTP API** (`GET /v1/mrd/latest`, `GET /v1/mrd/since`)
   - **Use WebSocket** (`ws://mri-marshal:8090`) for real-time frame notifications

3. Write your viz in whatever framework you want (Qt, web browser, Unity, etc.) — the marshal doesn't care what reads the data.

---

## HTTP API Quick Reference

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/health` | GET | Health check |
| `/v1/mrd/frame` | POST | Stream MRI frame (auto-detects: IMAGE, ACQUISITION, HDF5) |
| `/v1/mrd/ingest` | POST | Batch upload complete HDF5 file |
| `/v1/mrd/latest` | GET | Latest frame metadata |
| `/v1/mrd/since` | GET | Frames since timestamp |
| `/v1/bio/signal` | POST | Submit ECG/biosignal |
| `/v1/bio/latest` | GET | Latest biosignal |
| `/v1/pose/update` | POST | Submit pose data |
| `/v1/pose/current` | GET | Current pose |
| `/v1/config` | GET | Server config |

**Robot Marshal** (port 8081):

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/` | GET | Status |
| `/read/{filename}` | GET | Read data channel |
| `/write/{filename}` | POST | Write data channel |

For complete request/response examples: [HTTP_ROUTING_EXAMPLES.md](../HTTP_ROUTING_EXAMPLES.md)

---

## Storage

Images are stored to **disk** as HDF5 files with SWMR (Single-Writer Multiple-Reader) support:

```
/session-data/run_YYYYMMDD_HHMMSS/
├── mrd/
│   ├── cardiac_scan.mrd       ← SWMR streaming frames
│   └── batch_upload.mrd       ← Complete file uploads
├── bio.jsonl                  ← ECG/biosignals (append-only)
└── poses.jsonl                ← Pose tracking (append-only)
```

- `/v1/mrd/frame` → appends to SWMR file (real-time, readers can read while writing)
- `/v1/mrd/ingest` → saves as complete file (batch upload)
- Viz clients can open SWMR files concurrently with `h5py.File("...", "r", swmr=True)`

---

## Docker Compose Files

There are three compose files, each for a different purpose:

### `docker-compose.demo.yml` — Full demo with all mock clients

Runs the complete system: both marshals, all mock clients, and optionally viz.

```bash
# Quick demo (runs for DEMO_DURATION seconds, then stops)
./scripts/demo-docker.sh

# Persistent demo (containers keep running after script exits)
./scripts/demo-persistent.sh

# Or run manually:
docker compose --env-file .env.demo -f docker-compose.demo.yml up -d
```

**Services included:**

| Service | Image | Port | Role |
|---------|-------|------|------|
| `mri-marshal` | `cwru/mri-marshal` | 8080, 8090 | MRI data server |
| `robot-marshal` | `cwru/robot-marshal` | 8081 | Robot data server |
| `image-streamer` | `cwru/image-streamer` | — | Mock MRI frames → marshal |
| `ecg-client` | `cwru/ecg-client` | — | Mock ECG → marshal |
| `pose-client` | `cwru/pose-client` | — | Mock poses → marshal |
| `mock-recon` | `cwru/mock-recon` | 9002 | Mock reconstruction service |
| `kspace-streamer` | `cwru/kspace-streamer` | — | Mock k-space → marshal |
| `catheter-tracking` | `cwru/robot-clients` | — | Mock catheter → robot marshal |
| `controller` | `cwru/robot-clients` | — | Mock controller → robot marshal |
| `planning` | `cwru/robot-clients` | — | Mock planning → robot marshal |
| `front-end` | `cwru/robot-clients` | — | Mock UI → robot marshal |
| `surface-tracking` | `cwru/robot-clients` | — | Mock surface → robot marshal |
| `viz-client` | `cwru/viz-client` | — | Display (profile: viz) |

Configuration is in `.env.demo` (frame rate, image size, intervals, etc.).

### `docker-compose.recon.yml` — Reconstruction overlay

Extends `docker-compose.demo.yml` to add a real reconstruction service (e.g. Gadgetron) and configure the marshal to forward k-space to it.

```bash
# Run with reconstruction
docker compose -f docker-compose.demo.yml -f docker-compose.recon.yml up
```

This overrides the `mri-marshal` command to include `--recon-endpoint` and adds a `reconstruction-service` container.

### Running individual services in separate terminals

For development and debugging, you can start each service in its own terminal for full log visibility:

```bash
# Terminal 1: MRI Marshal
docker compose --env-file .env.demo -f docker-compose.demo.yml up mri-marshal

# Terminal 2: Robot Marshal
docker compose --env-file .env.demo -f docker-compose.demo.yml up robot-marshal

# Terminal 3: Image Streamer
docker compose --env-file .env.demo -f docker-compose.demo.yml up image-streamer

# Terminal 4: ECG Client
docker compose --env-file .env.demo -f docker-compose.demo.yml up ecg-client

# ... etc
```

For the full terminal-by-terminal walkthrough with expected output, environment variables, and troubleshooting: [MANUAL_TERMINAL_SETUP.md](MANUAL_TERMINAL_SETUP.md)

---

## Adding a New Domain

To add a completely new domain (e.g. ultrasound):

1. Create a new branch off `main`:
   ```bash
   git checkout main
   git checkout -b ultrasound-data-marshal
   ```

2. Add your domain-specific clients on that branch

3. Add a worktree entry and Dockerfiles on `main`:
   ```bash
   git worktree add .worktrees/ultrasound_data_marshal ultrasound-data-marshal
   ```

4. Add Dockerfiles in `docker/` and update `build-client-images.sh`

5. The marshal server stays the same — your clients just POST/GET to the same HTTP API
