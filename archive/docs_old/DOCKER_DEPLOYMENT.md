# Docker Deployment Guide

This guide covers the Docker containerization of the CWRU Data Marshal system.

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [Build Strategy](#build-strategy)
- [Prerequisites](#prerequisites)
- [Quick Start](#quick-start)
- [Service Details](#service-details)
- [Data Access](#data-access)
- [Mock Clients](#mock-clients)
- [USB Export](#usb-export)
- [Running Demos](#running-demos)
- [Troubleshooting](#troubleshooting)

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Host Machine                                 │
│                                                                      │
│  ┌────────────────────────┐       ┌────────────────────────┐       │
│  │   MRI Marshal          │       │   Robot Marshal        │       │
│  │   Container            │       │   Container            │       │
│  │                        │       │                        │       │
│  │   HTTP:  8080 ─────────┼───┐   │   HTTP:  8081 ─────────┼───┐  │
│  │   WS:    8090 ─────────┼─┐ │   │                        │   │  │
│  │                        │ │ │   │                        │   │  │
│  │   /data/mri_data ◄─────┼─┼─┼───┼────────────────────────┼─┐ │  │
│  └────────────────────────┘ │ │   └────────────────────────┘ │ │  │
│                              │ │                              │ │  │
│  ┌───────────────────────────┼─┼──────────────────────────────┼─┼──┤
│  │  Bind Mounts              │ │                              │ │  │
│  │                           │ │                              │ │  │
│  │  ./data/mri_data/ ◄───────┘ │   ./data/robot_data/ ◄───────┘ │  │
│  │  (HDF5 files)               │   (JSON files)                 │  │
│  └─────────────────────────────┼────────────────────────────────┼──┤
│                                │                                │  │
│  ┌─────────────────────────────┴────────────────────────────────┴──┤
│  │  External Clients (run on HOST)                                 │
│  │                                                                  │
│  │  • viz_client        → connects to localhost:8080, :8090        │
│  │  • ecg_client.py     → POST to localhost:8080/v1/bio/signal     │
│  │  • pose_client.py    → POST to localhost:8080/v1/pose/update    │
│  │  • Your applications → read ./data/mri_data/, ./data/robot_data/│
│  └──────────────────────────────────────────────────────────────────┘
└─────────────────────────────────────────────────────────────────────┘
```

### Key Points

- **Two Independent Containers**: MRI Marshal and Robot Marshal run separately
- **Host-based Clients**: Mock clients, viz_client run on HOST (not containerized)
- **Bind Mounts**: Data directories accessible from both containers and host
- **No Inter-Container Communication**: Services are independent; clients connect via exposed ports

---

## Build Strategy

### Why Local Worktrees? (Option 2)

The GitHub repository is **private**, so Docker cannot clone it during build. Instead, we use **local git worktrees** as build contexts.

```
/workspaces/
├── cwru_data_marshal/                    # main branch (Dockerfiles, compose)
│   ├── docker/
│   │   ├── Dockerfile.mri
│   │   └── Dockerfile.robot
│   └── docker-compose.yml
│
├── mri_data_marshal_worktree/            # mri-data-marhsal branch
│   ├── CMakeLists.txt                    # ← Build context for MRI Marshal
│   ├── src/
│   └── ...
│
└── robot_data_marshal_worktree/          # robot branch
    ├── server.cpp                        # ← Build context for Robot Marshal
    ├── httplib.h
    └── ...
```

### How It Works

1. **docker-compose.yml** specifies each service's build context as its worktree path
2. **Dockerfiles** use `COPY` to pull files from the build context
3. No network access to GitHub needed during build

### Alternative Approaches Considered

| Approach | Pros | Cons |
|----------|------|------|
| **Git Clone** | Self-contained | Needs public repo or tokens |
| **Local COPY (chosen)** | Works offline, simple | Requires worktrees exist |
| **Additional Contexts** | Flexible mixing | Needs Docker Compose v2.17+ |

#### Option 1: Git Clone During Build

Clone the repository directly in the Dockerfile:

```dockerfile
# Dockerfile.robot (Git Clone approach)
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y git build-essential
RUN git clone --depth 1 --branch robot_branch https://github.com/org/repo.git /opt/robot
RUN g++ /opt/robot/server.cpp -o /opt/robot/server
CMD ["./server"]
```

**Pros:**
- Self-contained image - no local files needed
- Always builds from the specified branch
- Works for CI/CD pipelines

**Cons:**
- Requires public repository OR authentication tokens
- Build fails if network is unavailable
- Slower builds (clone every time without caching)

**When to use:** Public repositories or CI systems with GitHub tokens.

#### Option 2: Local COPY (Chosen)

Copy source files from a local directory (git worktree) during build:

```dockerfile
# Dockerfile.robot (Local COPY approach)
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y build-essential
COPY server.cpp httplib.h json.hpp /opt/robot/
RUN g++ /opt/robot/server.cpp -o /opt/robot/server
CMD ["./server"]
```

```yaml
# docker-compose.yml
services:
  robot-marshal:
    build:
      context: /workspaces/robot_data_marshal_worktree  # Worktree path
      dockerfile: /workspaces/cwru_data_marshal/docker/Dockerfile.robot
```

**Pros:**
- Works offline (no network needed)
- Fast builds (leverages Docker cache)
- Builds exactly what you have locally
- Works with private repositories

**Cons:**
- Requires git worktrees to exist before building
- Build context must contain all needed files
- Cannot easily mix files from multiple directories

**When to use:** Private repositories, local development, offline environments.

#### Option 3: Additional Contexts (Docker Compose v2.17+)

Use named build contexts to reference multiple directories:

```yaml
# docker-compose.yml (Additional Contexts approach)
services:
  robot-marshal:
    build:
      context: .                                        # Primary context (main branch)
      dockerfile: docker/Dockerfile.robot
      additional_contexts:
        robot_src: /workspaces/robot_data_marshal_worktree  # Named context
```

```dockerfile
# Dockerfile.robot (Additional Contexts approach)
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y build-essential

# Copy from named context
COPY --from=robot_src server.cpp httplib.h json.hpp /opt/robot/

# Can also copy from primary context
COPY docker/shared_config.json /opt/robot/

RUN g++ /opt/robot/server.cpp -o /opt/robot/server
CMD ["./server"]
```

**Pros:**
- Mix files from multiple directories in one Dockerfile
- Primary context can hold shared files (configs, scripts)
- Clean separation of concerns
- Flexible for complex multi-source builds

**Cons:**
- Requires Docker Compose v2.17 or later
- More complex syntax (`COPY --from=name`)
- Less portable to older Docker versions

**When to use:** Complex builds needing files from multiple locations, shared configurations.

#### Why We Chose Option 2

For this project, **Local COPY** was chosen because:

1. **Private Repository**: The GitHub repo is private, making Git Clone impractical without tokens
2. **Simplicity**: Standard `COPY` syntax works with any Docker version
3. **Offline Capability**: Builds work without network access
4. **Development Workflow**: Worktrees are already used for branch management
5. **Independence**: Each marshal builds from its own worktree without cross-dependencies

---

## Prerequisites

### 1. Docker with Docker Compose

```bash
docker --version        # Docker 20.10+
docker compose version  # Docker Compose v2.0+
```

### 2. Git Worktrees

Create the required worktrees:

```bash
cd /workspaces/cwru_data_marshal

# Create MRI worktree (if not exists)
git worktree add /workspaces/mri_data_marshal_worktree mri-data-marhsal

# Create Robot worktree (if not exists)
git worktree add /workspaces/robot_data_marshal_worktree robot_data_marshal_with_catheter_system_components

# Verify
git worktree list
```

### 3. Data Directories

```bash
mkdir -p data/mri_data data/robot_data
```

---

## Quick Start

```bash
# Navigate to main branch
cd /workspaces/cwru_data_marshal

# Build images (first time takes 5-15 minutes)
docker compose build

# Start services
docker compose up -d

# Check status
docker compose ps

# Verify health
curl http://localhost:8080/health
curl http://localhost:8081/read/robot_status
```

---

## Service Details

### MRI Marshal

| Property | Value |
|----------|-------|
| Image | `cwru/mri-marshal:latest` |
| Container | `cwru-mri-marshal` |
| Source | `/workspaces/mri_data_marshal_worktree` |
| HTTP Port | 8080 |
| WebSocket Port | 8090 |
| Data Volume | `./data/mri_data` → `/data/mri_data` |
| Health Check | `GET /health` |

### Robot Marshal

| Property | Value |
|----------|-------|
| Image | `cwru/robot-marshal:latest` |
| Container | `cwru-robot-marshal` |
| Source | `/workspaces/robot_data_marshal_worktree` |
| HTTP Port | 8081 |
| Data Volume | `./data/robot_data` → `/data/robot_data` |
| Health Check | `GET /read/robot_status` |

**Note**: Robot Marshal's server.cpp is patched during build to listen on `0.0.0.0:8081` instead of the hardcoded `172.28.1.10:8080`.

---

## Data Access

### For External Clients

External clients can access data in two ways:

#### 1. HTTP APIs (Recommended)

```bash
# MRI Marshal
curl http://localhost:8080/v1/mrd/latest
curl http://localhost:8080/v1/pose/current

# Robot Marshal
curl http://localhost:8081/read/robot_status
curl http://localhost:8081/read/catheter_pose
```

#### 2. Direct File Access

Data files are bind-mounted, so they're accessible on the host:

```bash
# MRI data (HDF5 files)
ls ./data/mri_data/

# Robot data (JSON files)
ls ./data/robot_data/
```

### Data Flow

```
External Client
      │
      ├── HTTP ──────────► MRI Marshal ──────► ./data/mri_data/*.h5
      │                         │
      │                         └── WebSocket (real-time)
      │
      └── HTTP ──────────► Robot Marshal ────► ./data/robot_data/*.json
```

### Cross-Marshal Data Access

The marshals are **independent** and don't share data directly. If you need:

- **MRI data in Robot client**: Query MRI Marshal's HTTP API
- **Robot data in MRI client**: Query Robot Marshal's HTTP API
- **Combined data**: Use a coordinator/bridge service on the host

---

## Mock Clients

Mock clients are located in the MRI worktree and run on the **host** (not in Docker).

### Location

```
/workspaces/mri_data_marshal_worktree/clients/mocks/
├── ecg_client.py       # Pure Python - no dependencies
├── pose_client.py      # Pure Python - no dependencies
├── http_tracker.py     # Requires: pip install requests
├── planner.py          # Requires: pip install websockets
├── surface_tracker.py  # Requires: pip install websockets
└── README.md
```

### Usage

```bash
# ECG signals (pure Python)
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/ecg_client.py \
  --endpoint http://localhost:8080 --count 10

# Pose updates (pure Python)
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/pose_client.py \
  --endpoint http://localhost:8080 --count 20

# HTTP tracker (requires requests)
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/http_tracker.py

# WebSocket planner (requires websockets)
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/planner.py
```

---

## USB Export

To deploy on a machine without access to the source repository:

```bash
# Build images first
docker compose build

# Export to USB/directory
./scripts/export_usb.sh /path/to/usb/cwru_deploy
```

### Export Contents

```
cwru_deploy/
├── images/
│   ├── mri-marshal.tar      # Docker image
│   └── robot-marshal.tar    # Docker image
├── mock_clients/
│   ├── ecg_client.py
│   ├── pose_client.py
│   └── README.md
├── data/
│   ├── mri_data/
│   └── robot_data/
├── docker-compose.yml
└── README.md                 # Load instructions
```

### On Receiving Machine

```bash
cd /path/to/usb/cwru_deploy

# Load images
docker load -i images/mri-marshal.tar
docker load -i images/robot-marshal.tar

# Start services
docker compose up -d

# Test
curl http://localhost:8080/health
python3 mock_clients/ecg_client.py --count 5
```

---

## Running Demos

### Quick Demo - Mock Clients Only

Test the system with synthetic data:

```bash
# Terminal 1: Start Docker services
cd /workspaces/cwru_data_marshal
docker compose up -d
docker compose ps  # Wait until both show "healthy"

# Terminal 2: Send ECG signals
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/ecg_client.py \
  --endpoint http://localhost:8080 --count 20 --interval 0.5

# Terminal 3: Send pose updates
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/pose_client.py \
  --endpoint http://localhost:8080 --count 100 --interval 0.1
```

### Full Demo - With Visualization

Run the complete demo with MRI streaming and visualization:

```bash
# Terminal 1: Start Docker services
cd /workspaces/cwru_data_marshal
docker compose up -d

# Terminal 2: Build and run viz_client on HOST (requires GUI)
cd /workspaces/mri_data_marshal_worktree
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target viz_client
./build/viz_client --endpoint http://localhost:8080 --ws ws://localhost:8090

# Terminal 3: Stream MRI data
cd /workspaces/mri_data_marshal_worktree
./build/image_streamer --endpoint http://localhost:8080 --ws ws://localhost:8090 \
  --stream demo_stream --count 100

# Terminal 4: Send pose updates (continuous)
python3 clients/mocks/pose_client.py --endpoint http://localhost:8080 \
  --trajectory circular --interval 0.1

# Terminal 5: Send ECG signals (continuous)
python3 clients/mocks/ecg_client.py --endpoint http://localhost:8080 \
  --interval 1.0
```

### Using Existing Demo Scripts

If you have demo scripts in the main branch:

```bash
# Ensure Docker services are running
docker compose up -d

# Run from MRI worktree (scripts expect local marshal, but will connect to Docker)
cd /workspaces/mri_data_marshal_worktree
../cwru_data_marshal/scripts/run_demo_simultaneous.sh
```

**Note**: Demo scripts may need modification to work with Docker. Check that they connect to `localhost:8080/8081` rather than starting their own marshals.

### Demo Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Docker Containers                             │
│                                                                  │
│  ┌─────────────────┐              ┌─────────────────┐          │
│  │  MRI Marshal    │              │  Robot Marshal  │          │
│  │  :8080 / :8090  │              │  :8081          │          │
│  └────────▲────────┘              └────────▲────────┘          │
└───────────┼────────────────────────────────┼────────────────────┘
            │                                │
┌───────────┼────────────────────────────────┼────────────────────┐
│  Host     │                                │                    │
│           │                                │                    │
│  image_streamer ───► MRI data              │                    │
│  ecg_client.py ────► bio signals           │                    │
│  pose_client.py ───► pose updates          │                    │
│  viz_client ◄─────── visualization         │                    │
│                                            │                    │
│  (Robot clients would connect here) ───────┘                    │
└─────────────────────────────────────────────────────────────────┘
```

### Stopping the Demo

```bash
# Stop mock clients (Ctrl+C in each terminal)

# Close viz_client window

# Stop Docker services
docker compose down

# Verify everything stopped
docker compose ps
pkill -f viz_client
pkill -f image_streamer
```

### Checking Demo Data

After running a demo, inspect the generated data:

```bash
# MRI data files
ls -lh data/mri_data/

# Robot data files
ls -lh data/robot_data/
cat data/robot_data/robot_status.json
```

---

## Port Reference

| Port | Service | Protocol | Purpose |
|------|---------|----------|---------|
| 8080 | MRI Marshal | HTTP | REST API |
| 8090 | MRI Marshal | WebSocket | Real-time streaming |
| 8081 | Robot Marshal | HTTP | REST API |

---

## Common Commands

```bash
# Build
docker compose build                    # Build all
docker compose build mri-marshal        # Build one service
docker compose build --no-cache         # Force rebuild

# Run
docker compose up -d                    # Start detached
docker compose up                       # Start with logs
docker compose up mri-marshal           # Start one service

# Status
docker compose ps                       # Container status
docker compose logs -f                  # Follow logs
docker compose logs -f mri-marshal      # One service logs

# Stop
docker compose down                     # Stop containers
docker compose down -v                  # Stop and remove volumes

# Debug
docker exec -it cwru-mri-marshal bash   # Shell into container
docker inspect cwru-mri-marshal         # Container details
```

---

## Troubleshooting

### "Worktree not found" during build

```bash
# Create missing worktrees
git worktree add /workspaces/mri_data_marshal_worktree mri-data-marhsal
git worktree add /workspaces/robot_data_marshal_worktree robot_data_marshal_with_catheter_system_components
```

### "Port already in use"

```bash
# Find process using port
sudo lsof -i :8080

# Kill it or change ports in docker-compose.yml
```

### Health check failing

```bash
# Check container logs
docker compose logs mri-marshal
docker compose logs robot-marshal

# Manual health check
docker exec cwru-mri-marshal curl http://localhost:8080/health
```

### Stale worktree references

```bash
# Remove orphaned worktree entries
git worktree prune

# List current worktrees
git worktree list
```

### HDF5 file locking errors

Environment variables are set in docker-compose.yml:
```yaml
environment:
  - HDF5_USE_FILE_LOCKING=FALSE
  - HDF5_FILE_LOCKING=FALSE
```

---

## Summary

| What | Where |
|------|-------|
| Dockerfiles | `/workspaces/cwru_data_marshal/docker/` |
| docker-compose.yml | `/workspaces/cwru_data_marshal/` |
| MRI source | `/workspaces/mri_data_marshal_worktree/` |
| Robot source | `/workspaces/robot_data_marshal_worktree/` |
| MRI data | `./data/mri_data/` |
| Robot data | `./data/robot_data/` |
| Mock clients | `/workspaces/mri_data_marshal_worktree/clients/mocks/` |
| USB export | `./scripts/export_usb.sh` |
