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
    │
    │ MRD TCP (port 9100)
    │ CONFIG_FILE + METADATA_XML + ACQUISITION×N + WAVEFORM + CLOSE
    v
MRI Marshal (HTTP :8080, MRD TCP :9100)
    │                              │
    │ MRD TCP forward              │ HTTP query/control
    │ (if --recon-host set)        │ GET /image/latest, /transform, /pose, /health
    v                              v
Reconstruction Service (:9002)   Viz Client, Pose Client, etc.
    │
    │ MRD TCP IMAGE(1022) back
    v
MRI Marshal → live/from_reconstruction/scan_<ts>.h5 (and dump/… with --dump)
    │
    │ MRD TCP IMAGE(1022) pushed to scanner
    v
Scanner
```

The marshal has two interfaces: **MRD TCP** for scanner data transport (the same wire protocol as python-ismrmrd-server) and **HTTP** for query/control endpoints. Scanner clients connect via MRD TCP and send ISMRMRD messages. Marshal forwards to recon via MRD TCP and always appends scanner- and recon-side standard ISMRMRD objects to per-scan HDF5 files under `live/from_scanner/` and `live/from_reconstruction/`. With `--dump`, it additionally mirrors both sides under `dump/from_scanner/` and `dump/from_reconstruction/` for retrospective analysis. Recon sends images back on the same TCP connection. The viz client polls HTTP `GET /image/latest`, reads the returned `path` and `newest_series`, and opens image group `image_<newest_series>` in the per-scan HDF5 file.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full wire protocol reference and compose topology.

---

## Replacing Mock Clients with Real Ones

The demo ships with mock clients that generate synthetic data. To integrate real hardware or software, replace these mocks on your domain branch.

### What Are the Mock Clients?

| Mock Client | What it does | Docker image | Branch |
|-------------|-------------|--------------|--------|
| `kspace_streamer` | Sends k-space + ECG waveforms via MRD TCP (--ecg flag for waveforms) | `cwru/kspace-streamer` | MRI |
| `image_streamer` | Sends synthetic images via MRD TCP | `cwru/image-streamer` | MRI |
| `pose_client` | Sends synthetic poses via HTTP POST /pose | `cwru/pose-client` | MRI |
| `viz_client` | Polls HTTP GET /image/latest, reads file, displays with OpenCV | `cwru/viz-client` | MRI |
| `mock_recon` | MRD TCP recon server, receives acquisitions, sends images back | `cwru/mock-recon` | MRI |
| Robot clients | Simulate catheter, controller, planning, front-end, surface tracking | `cwru/robot-clients` | Robot |

### Steps to Replace a Mock

**Example: Replace the mock `kspace_streamer` with a real MRI scanner bridge**

1. **Work on your domain branch:**
   ```bash
   cd .worktrees/mri_data_marshal
   ```

2. **Understand the interface.** The k-space streamer connects via MRD TCP and sends:
   ```
   CONFIG_FILE(1)        config name (e.g. "simplefft")
   METADATA_XML_TEXT(3)  ISMRMRD XML header
   ACQUISITION(1008)     one k-space line per message (repeated)
   WAVEFORM(1026)        ECG / physio data (optional, with --ecg)
   CLOSE(4)              end of scan
   ```
   See [ARCHITECTURE.md](ARCHITECTURE.md) for the full wire protocol.

3. **Write your real client.** Open a raw TCP socket to the marshal's `--mrd-port` and send the same MRD messages. The wire format is identical to what python-ismrmrd-server expects.

4. **Update the Dockerfile** (on `main`, in `docker/`).

5. **Update `docker-compose.demo.yml`** to use your new image.

6. **The marshal server does not change.** It accepts MRD TCP connections and forwards based on the ISMRMRD wire format, not client identity. With `--dump`, it also archives standard ISMRMRD objects.

---

## API Quick Reference

### MRI Marshal — MRD TCP (port 9100, primary scanner/recon transport)

Scanner and recon clients connect via raw TCP using python-ismrmrd-server's 2-byte message ID framing. See [ARCHITECTURE.md](ARCHITECTURE.md) for the wire protocol.

### MRI Marshal — HTTP (port 8080, query/control only)

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/health` | GET | Health check |
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

Every scan produces per-scan HDF5 files under the session-data umbrella. The live subtree is always populated; the dump subtree is populated only with `--dump`:

```
${dump_dir}/
├── live/
│   ├── from_scanner/
│   │   └── scan_<ts>.h5         <- Scanner-origin ISMRMRD data (acquisitions, images, waveforms), appended
│   └── from_reconstruction/
│       ├── scan_<ts>.h5         <- Recon-returned images, appended; image groups by image_series_index
│       └── latest_error.png     <- Reconstruction-failed indicator (single overwritten file)
└── dump/                         <- only when --dump is on
    ├── from_scanner/scan_<ts>.h5
    └── from_reconstruction/scan_<ts>.h5
```

- Scanner MRD TCP standard ISMRMRD objects → live (always) and dump (if `--dump`) scanner archives.
- Recon MRD TCP standard ISMRMRD objects → live (always) and dump (if `--dump`) recon archives.
- Recon failure → scanner MRD `IMAGE(1022)` failure image + HTTP `GET /image/latest` points at `latest_error.png`.
- HDF5 files are readable while being written (per-scan append). Viz uses `/image/latest`'s `newest_series` field to know which `image_<N>` group is the newest volume.
- `<ts>` is shared between a scan's live and dump files.

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
