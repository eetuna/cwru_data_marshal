# CWRU Data Marshal - Developer Guide

How the repository is organized, how to work with branches and worktrees, and how to replace mock clients with real ones.

---

## Branch Architecture

```
audit/mri-marshal-protocol-fixes-umbrella   <- active umbrella (docs, Dockerfiles, scripts, compose)
├── audit/live-atomic-rename                <- MRI marshal baseline (v0 snapshot at 837a101)
│   ├── perf/latest-image-shared-buffers    <- latest-image experiment #1 (bounded backpressure)
│   ├── perf/latest-file-reuse              <- latest-image experiment #2
│   ├── perf/latest-slot-reuse              <- latest-image experiment #3
│   └── perf/latest-bulk-prealloc           <- latest-image experiment #4 (benchmark winner)
└── robot_data_marshal_with_catheter_system_components  <- Robot marshal + robot clients
```

Active fix branches (bug-audit follow-up 2026-04-18):
- `fix/marshal-bug-audit-2026-04-18` on umbrella: compose / docs / scripts edits
- `fix/marshal-source-2026-04-18` on inner worktree: source fixes

**Historical note.** An older layout named `main` as the umbrella and
`mri-data-marshal` / `robot-data-marshal` as the domain branches. Those
branches still exist (see `git branch -a`) but are no longer the active
development targets.

The umbrella does **not** contain the marshal server source code or any
client code. It holds:
- Documentation (`docs/`, `README.md`)
- Dockerfiles (`docker/`)
- Docker Compose files (`docker-compose.*.yml`)
- Build and demo scripts (`scripts/`)
- Configuration (`.env.demo`, `.gitignore`)

| Branch | What it contains | Who works on it |
|--------|-----------------|-----------------|
| `audit/mri-marshal-protocol-fixes-umbrella` | Docs, Dockerfiles, compose files, scripts (no source code) | Everyone (shared) |
| `audit/live-atomic-rename` / `perf/latest-*` | MRI marshal server (C++), streamers, viz client, pose client | MRI team |
| `robot_data_marshal_with_catheter_system_components` | Robot marshal server (C++), catheter tracking, controller, planning, front-end, surface tracking | Robot team |

**Rules:**
- Docs, Dockerfiles, and scripts go into the umbrella, then merge into domain branches
- Domain branches never merge back into the umbrella -- they only diverge with their own code
- New domain (e.g. ultrasound) = new branch off the umbrella

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
git worktree add .worktrees/mri_data_marshal audit/live-atomic-rename
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
MRI Marshal → <mode>/from_reconstruction/scan_<ts>.h5  (<mode> = live OR dump)
    │
    │ MRD TCP IMAGE(1022) pushed to scanner
    v
Scanner
```

The marshal has two interfaces: **MRD TCP** for scanner data transport (same wire protocol as python-ismrmrd-server) and **HTTP** for query/control endpoints. Scanner clients connect via MRD TCP. Marshal forwards to recon via MRD TCP. Archival is **mode-exclusive**: marshal is started in either live mode (default) or dump mode (`--dump`) and only the selected mode's subtree under `${dump_dir}/` is populated. Both modes use a raw-MRD spool written during the scan, converted to a canonical `scan_<ts>.h5` on CLOSE; the per-scan HDF5 is an archival output, not a mid-scan reader interface. In live mode only, each incoming live IMAGE also atomically publishes a closed companion snapshot at `live/from_*/latest_image.h5`. Recon sends images back on the same TCP connection. The viz client polls HTTP `GET /image/latest`, receives the companion path in live mode, or `404 Not Found` in dump mode.

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

Every scan produces per-scan HDF5 files under the session-data umbrella. Archival mode is **exclusive**: marshal is started in either live mode (default) or dump mode (`--dump`), and only the selected mode's subtree is populated.

```
${dump_dir}/
├── live/                         <- ONLY in live mode (no --dump)
│   ├── from_scanner/
│   │   ├── scan_<ts>.h5.spool    <- raw MRD wire frames written during scan
│   │   ├── scan_<ts>.h5          <- ISMRMRD HDF5 produced by converter on CLOSE (images + waveforms; no raw ACQs in live mode)
│   │   └── latest_image.h5       <- closed companion snapshot, atomic-rename per live IMAGE
│   └── from_reconstruction/
│       ├── scan_<ts>.h5.spool
│       ├── scan_<ts>.h5          <- recon-returned images
│       ├── latest_image.h5
│       └── latest_error.png      <- reconstruction-failed indicator (single overwritten file)
└── dump/                         <- ONLY in dump mode (--dump)
    ├── from_scanner/
    │   ├── scan_<ts>.h5.spool    <- full raw stream incl. ACQ
    │   └── scan_<ts>.h5
    └── from_reconstruction/
        ├── scan_<ts>.h5.spool
        └── scan_<ts>.h5
```

- Both modes use the same spool-then-convert pipeline: a `.spool` of raw MRD wire frames is written during the scan, and converted to canonical `scan_<ts>.h5` on CLOSE. The `.spool` is retained by default for forensic recovery.
- The per-scan `scan_<ts>.h5` is an archival output finalized at CLOSE. It is NOT a mid-scan reader interface (HDF5's default file locking would block mid-scan opens on an open writer).
- The only mid-scan readable interface is `latest_image.h5` (live mode only), published via atomic rename per incoming IMAGE.
- `GET /image/latest` in live mode: `204 No Content` before first IMAGE, then `{path, error}`. In dump mode: `404 Not Found` with `{"error":"dump mode; no live snapshot"}`.
- Recon failure in live mode → scanner MRD `IMAGE(1022)` failure image + `GET /image/latest` points at `latest_error.png`.

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
