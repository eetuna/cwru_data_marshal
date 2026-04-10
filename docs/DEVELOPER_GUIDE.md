# CWRU Data Marshal - Developer Guide

How the repository is organized, how to work with branches and worktrees, and how to replace mock clients with real ones.

---

## Branch Architecture

```
main                          <- Umbrella branch (docs, Docker configs, scripts - no application code)
├── mri-data-marshal          <- MRI marshal server + MRI clients (image streamer, viz, k-space)
└── robot-data-marshal        <- Robot marshal server + robot clients (catheter, controller, planning)
```

`main` does **not** contain the marshal server source code or any client code. It is an umbrella/orchestration branch that holds:
- Documentation (`docs/`, `README.md`)
- Dockerfiles (`docker/`)
- Docker Compose files (`docker-compose.*.yml`)
- Build and demo scripts (`scripts/`)
- Configuration (`.env.demo`, `.gitignore`)

| Branch | What it contains | Who works on it |
|--------|-----------------|-----------------|
| `main` | Docs, Dockerfiles, compose files, scripts (no source code) | Everyone (shared) |
| `feature/mri-marshal-rewrite-v2-inner` | MRI marshal server (C++), streamers, viz client, ECG/pose clients | MRI team |
| `robot-data-marshal` | Robot marshal server (C++), catheter tracking, controller, planning, front-end, surface tracking | Robot team |

**Rules:**
- Docs, Dockerfiles, and scripts go into `main`, then merge `main` into domain branches
- Domain branches never merge back into `main` -- they only diverge with their own code
- New domain (e.g. ultrasound) = new branch off `main`

---

## Worktree Setup

Git worktrees let you have all branches checked out simultaneously in separate directories, so you can build and run everything without switching branches.

### Directory Layout

```
/workspaces/cwru_data_marshal/                        <- main (checked out here)
/workspaces/cwru_data_marshal/.worktrees/
    ├── mri_data_marshal/                             <- MRI branch
    └── robot_data_marshal/                           <- Robot branch
```

### Creating Worktrees

```bash
# From the repo root (main branch)
git worktree add .worktrees/mri_data_marshal feature/mri-marshal-rewrite-v2-inner
git worktree add .worktrees/robot_data_marshal robot_data_marshal_with_catheter_system_components
```

Or just run the build script -- it creates worktrees automatically:

```bash
./scripts/build-client-images.sh
```

---

## System Overview

```
Scanner / K-Space Streamer
    |
    | POST /header, /config, /frame, /close
    v
MRI Marshal (:8080)
    |
    | forwards to recon (if --recon-url set)
    v
Reconstruction Service (:9002)
    |
    | POST /image (reconstructed)
    v
MRI Marshal
    |
    | GET /image/latest -> file path
    v
Viz Client (reads file, renders with OpenCV)
```

The marshal is a generic HTTP server. Scanner clients POST ISMRMRD data via `/header`, `/config`, `/frame`, and `/close`. Marshal archives to canonical ISMRMRD HDF5 and optionally forwards to a reconstruction service. Recon posts images back via `/image`. The viz client polls `/image/latest` for the file path and reads the standalone binary file directly.

---

## Replacing Mock Clients with Real Ones

The demo ships with mock clients that generate synthetic data. To integrate real hardware or software, replace these mocks on your domain branch.

### What Are the Mock Clients?

| Mock Client | What it does | Docker image | Branch |
|-------------|-------------|--------------|--------|
| `kspace_streamer` | Generates synthetic k-space, POSTs /header+/config+/frame+/close | `cwru/kspace-streamer` | MRI |
| `image_streamer` | Generates synthetic images, POSTs /header+/config+/frame+/close | `cwru/image-streamer` | MRI |
| `ecg_client` | Generates ISMRMRD waveforms (ECG), POSTs /frame | `cwru/ecg-client` | MRI |
| `pose_client` | Generates synthetic poses, POSTs /pose | `cwru/pose-client` | MRI |
| `viz_client` | Polls GET /image/latest, reads file, displays with OpenCV | `cwru/viz-client` | MRI |
| `mock_recon` | Reconstruction service, accepts /header+/config+/frame+/close, POSTs /image | `cwru/mock-recon` | MRI |
| Robot clients | Simulate catheter, controller, planning, front-end, surface tracking | `cwru/robot-clients` | Robot |

### Steps to Replace a Mock

**Example: Replace the mock `kspace_streamer` with a real MRI scanner bridge**

1. **Work on your domain branch:**
   ```bash
   cd .worktrees/mri_data_marshal
   ```

2. **Understand the interface.** The k-space streamer sends:
   ```
   POST /header   (ISMRMRD XML header)
   POST /config   (recon config name, e.g. "simplefft")
   POST /frame    (one ISMRMRD acquisition per call, repeated)
   POST /close    (end of scan)
   ```

3. **Write your real client.** It just needs to make the same HTTP POST requests.

4. **Update the Dockerfile** (on `main`, in `docker/`).

5. **Update `docker-compose.demo.yml`** to use your new image.

6. **The marshal server does not change.** It accepts data based on ISMRMRD wire format, not client identity.

---

## HTTP API Quick Reference

### MRI Marshal (port 8080)

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/health` | GET | Health check |
| `/header` | POST | Start scan (ISMRMRD XML header) |
| `/config` | POST | Set recon config name |
| `/frame` | POST | Submit ISMRMRD message (acquisition/image/waveform) |
| `/close` | POST | End scan |
| `/image` | POST | Receive reconstructed image from recon |
| `/image/latest` | GET | Path to latest reconstructed image |
| `/transform` | GET | Read slice transform delta (consume-on-read) |
| `/transform` | PUT | Write slice transform delta |
| `/pose` | POST | Submit pose update |
| `/pose` | GET | Get latest pose |
| `/dump/scanner` | GET | List scanner archive files |
| `/dump/recon` | GET | List recon archive files |

**Robot Marshal** (port 8081):

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/` | GET | List channels |
| `/read/{filename}` | GET | Read data channel |
| `/write/{filename}` | POST | Write data channel |

---

## Storage

Data is archived to disk as canonical ISMRMRD HDF5 files:

```
${dump_dir}/
├── from_scanner/
│   └── scan_<timestamp>.h5      <- Scanner data (acquisitions, images, waveforms)
├── from_reconstruction/
│   ├── scan_<timestamp>.h5      <- Reconstructed images
│   ├── latest_image.bin         <- Standalone file for live viz (raw ISMRMRD wire bytes)
│   └── latest_error.png         <- Reconstruction-failed indicator (if applicable)
```

- `/header` + `/config` + `/frame` + `/close` -> scanner archive
- `/image` (from recon) -> recon archive + standalone file
- HDF5 files are readable only after `/close`
- `latest_image.bin` is updated atomically during the scan for live viewing

---

## Docker Compose Files

### `docker-compose.demo.yml` - Full demo with all mock clients

Runs the complete system: both marshals, all mock clients, and optionally viz.

```bash
# Using the alias
alias cdd='docker compose --env-file .env.demo -f docker-compose.demo.yml'
cdd up mri-marshal
```

Configuration is in `.env.demo` (frame rate, image size, intervals, etc.).

---

## Adding a New Domain

To add a completely new domain (e.g. ultrasound):

1. Create a new branch off `main`
2. Add your domain-specific clients on that branch
3. Add a worktree entry and Dockerfiles on `main`
4. The marshal server stays the same -- your clients just POST/GET to the same HTTP API
