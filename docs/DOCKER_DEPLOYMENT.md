# Docker Deployment Guide

This guide covers the Docker containerization and deployment of the CWRU Data Marshal system.

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [Quick Start](#quick-start)
- [Service Details](#service-details)
- [Volume Structure](#volume-structure)
- [Health Checks](#health-checks)
- [Mock Clients](#mock-clients)
- [USB Deployment](#usb-deployment)
- [Troubleshooting](#troubleshooting)
- [Advanced Configuration](#advanced-configuration)

---

## Architecture Overview

The CWRU Data Marshal system consists of two containerized services that run independently:

```
┌─────────────────────────────────────────────────────────────────┐
│                         Host Machine                             │
│                                                                   │
│  ┌────────────────────┐              ┌─────────────────────┐    │
│  │  MRI Marshal       │              │  Robot Marshal      │    │
│  │  Container         │              │  Container          │    │
│  │                    │              │                     │    │
│  │  ┌──────────────┐  │              │  ┌──────────────┐  │    │
│  │  │   marshal    │  │              │  │ robot_marshal│  │    │
│  │  │              │  │              │  │    _demo     │  │    │
│  │  │ HTTP: 8080   │◄─┼──────┐       │  │ HTTP: 8081   │◄─┼─┐  │
│  │  │ WS:   8090   │◄─┼────┐ │       │  │              │  │ │  │
│  │  └──────────────┘  │    │ │       │  └──────────────┘  │ │  │
│  │                    │    │ │       │                     │ │  │
│  │  Volume:           │    │ │       │  Volume:            │ │  │
│  │  /data/mri_data ◄──┼─┐  │ │       │  /data/robot_data ◄─┼┐│  │
│  └────────────────────┘ │  │ │       └─────────────────────┘││  │
│                         │  │ │                               ││  │
│  ┌──────────────────────┼──┼─┼───────────────────────────────┼┼──┤
│  │  Bind Mounts         │  │ │                               ││  │
│  │                      ▼  │ │                               ▼│  │
│  │  ./data/mri_data/*.h5   │ │       ./data/robot_data/*.json│  │
│  └─────────────────────────┼─┼────────────────────────────────┼──┤
│                             │ │                                │  │
│  ┌──────────────────────────┼─┼────────────────────────────────┼──┤
│  │  Host Clients            │ │                                │  │
│  │                          │ │                                │  │
│  │  viz_client ─────────────┘ │                                │  │
│  │  ecg_client.py ────────────┘                                │  │
│  │  pose_client.py ─────────────────────────────────────────────┘  │
│  │                                                                  │
│  └──────────────────────────────────────────────────────────────────┘
└─────────────────────────────────────────────────────────────────┘
```

### Key Points

- **Two Independent Containers**: MRI Marshal and Robot Marshal run in separate containers
- **Host-based Clients**: Mock clients and viz_client run on the HOST (not containerized)
- **Bind Mounts**: Data directories are bind-mounted from host to containers
- **Port Exposure**: Services expose HTTP/WebSocket ports to host network
- **No Inter-Container Communication**: Services are independent; clients connect via exposed ports

---

## Quick Start

### Prerequisites

- Docker Engine 20.10+
- Docker Compose 2.0+
- 4GB+ available disk space (for images)
- Git (for building from source)

### Build and Run

```bash
# From the main branch repository root
cd /workspaces/cwru_data_marshal

# Create data directories
mkdir -p data/mri_data data/robot_data

# Build and start services
docker compose up --build -d

# Check status
docker compose ps

# View logs
docker compose logs -f
```

### Verify Services

```bash
# Check MRI Marshal health
curl http://localhost:8080/health

# Expected: {"ok": true} or similar

# Check Robot Marshal health
curl http://localhost:8081/read/robot_status

# Expected: {"ok": true, "status": "operational"} or similar
```

### Test with Mock Clients

```bash
# Send 5 ECG signals
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/ecg_client.py \
  --endpoint http://localhost:8080 --count 5

# Send 20 pose updates
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/pose_client.py \
  --endpoint http://localhost:8080 --count 20
```

---

## Service Details

### MRI Marshal

**Container Name:** `cwru-mri-marshal`
**Image:** `cwru/mri-marshal:latest`
**Source Branch:** `mri-data-marhsal`

**Ports:**
- `8080` - HTTP API endpoint
- `8090` - WebSocket streaming endpoint

**Volume:**
- Host: `./data/mri_data`
- Container: `/data/mri_data`
- Purpose: Store MRI data as HDF5 files (`.h5`)

**Health Check:**
- Endpoint: `http://localhost:8080/health`
- Interval: 10s
- Timeout: 5s
- Start Period: 30s (allows build time)
- Retries: 3

**Environment Variables:**
- `HDF5_USE_FILE_LOCKING=FALSE` - Disable HDF5 file locking (WSL2/NFS compatibility)
- `HDF5_FILE_LOCKING=FALSE` - Additional HDF5 locking flag

**Command:**
```bash
/opt/mri/build/marshal \
  --http 0.0.0.0:8080 \
  --ws 0.0.0.0:8090 \
  --data /data/mri_data \
  --sink mrd
```

**Build Details:**
- Base image: `ubuntu:22.04`
- Dependencies: Boost, HDF5, PugiXML, Eigen3, ISMRMRD
- Build targets: `marshal`, `image_streamer`
- Excludes: `viz_client` (runs on host)

---

### Robot Marshal

**Container Name:** `cwru-robot-marshal`
**Image:** `cwru/robot-marshal:latest`
**Source Branch:** `robot_data_marshal_with_catheter_system_components`

**Ports:**
- `8081` - HTTP API endpoint

**Volume:**
- Host: `./data/robot_data`
- Container: `/data/robot_data`
- Purpose: Store robot data as JSON files (`.json`)

**Health Check:**
- Endpoint: `http://localhost:8081/read/robot_status`
- Interval: 10s
- Timeout: 5s
- Start Period: 10s
- Retries: 3

**Command:**
```bash
/opt/robot/build/robot_marshal_demo 8081
```

**Build Details:**
- Base image: `ubuntu:22.04`
- Compiler: g++ with C++17
- Dependencies: Minimal (pthread only)
- Entrypoint: Custom script that initializes data directory

---

## Volume Structure

The project uses **bind mounts** for data persistence:

```
/workspaces/cwru_data_marshal/
├── data/
│   ├── mri_data/           # Bind-mounted to MRI Marshal container
│   │   ├── stream_*.h5     # MRI data files (created at runtime)
│   │   └── ...
│   └── robot_data/         # Bind-mounted to Robot Marshal container
│       ├── robot_status.json
│       ├── catheter_pose.json
│       └── ...
├── docker-compose.yml
└── ...
```

### Why Bind Mounts?

- **Easy Access**: Data is directly accessible from host filesystem
- **Portability**: Simple to backup, inspect, or transfer data
- **Development**: Makes debugging easier (tail files, inspect with tools)
- **USB Export**: Data directories can be included in export packages

### Data Lifecycle

1. **Startup**: Directories are created if they don't exist
2. **Runtime**: Services write data to mounted volumes
3. **Shutdown**: Data persists on host after container stops
4. **Cleanup**: Remove `data/` contents manually if desired

---

## Health Checks

Both services include Docker health checks for monitoring:

### Checking Health Status

```bash
# View health status in compose
docker compose ps

# Expected output:
# NAME                 STATUS              PORTS
# cwru-mri-marshal     Up (healthy)        0.0.0.0:8080->8080/tcp, ...
# cwru-robot-marshal   Up (healthy)        0.0.0.0:8081->8081/tcp

# Inspect specific health check
docker inspect cwru-mri-marshal --format='{{.State.Health.Status}}'
```

### Health Check States

- **starting**: Container is in start period (health checks not yet run)
- **healthy**: Service is responding correctly
- **unhealthy**: Service failed health checks (after retries)

### Manual Health Checks

```bash
# MRI Marshal
curl -f http://localhost:8080/health && echo " ✓ MRI Marshal healthy"

# Robot Marshal
curl -f http://localhost:8081/read/robot_status && echo " ✓ Robot Marshal healthy"
```

---

## Mock Clients

Mock clients simulate external devices and run on the HOST machine (not containerized).

### Location

- **Repository**: MRI Marshal branch worktree
- **Path**: `/workspaces/mri_data_marshal_worktree/clients/mocks/`

### Available Clients

| Client | Purpose | Dependencies | Endpoint |
|--------|---------|--------------|----------|
| `ecg_client.py` | Simulated ECG signals | None (stdlib) | POST `/v1/bio/signal` |
| `pose_client.py` | Simulated robot poses | None (stdlib) | POST `/v1/pose/update` |
| `http_tracker.py` | Poll MRI/pose data | `requests` | GET `/v1/mrd/latest`, `/v1/pose/current` |
| `planner.py` | WebSocket frame verifier | `websockets` | WS `/ws` (subscribe to `mrd`) |
| `surface_tracker.py` | Mock surface tracker | `websockets` | WS `/ws` |

### Usage Examples

```bash
# ECG Client - Send 10 signals at 1 Hz
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/ecg_client.py \
  --count 10 --interval 1.0

# Pose Client - Circular trajectory at 10 Hz
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/pose_client.py \
  --trajectory circular --interval 0.1 --count 100

# HTTP Tracker - Continuous polling
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/http_tracker.py

# Planner - WebSocket frame continuity check
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/planner.py
```

See [`/workspaces/mri_data_marshal_worktree/clients/mocks/README.md`](../../mri_data_marshal_worktree/clients/mocks/README.md) for complete documentation.

---

## USB Deployment

The project includes a USB export script that packages everything needed for offline deployment.

### Creating Export Package

```bash
# From main branch repository root
cd /workspaces/cwru_data_marshal

# Create export package
./scripts/export_usb.sh /path/to/output

# Example: Export to USB drive
./scripts/export_usb.sh /media/usb/cwru_deploy
```

### Export Contents

```
output_directory/
├── images/
│   ├── mri-marshal.tar       # Docker image (~1-2 GB)
│   └── robot-marshal.tar     # Docker image (~300-500 MB)
├── mock_clients/
│   ├── ecg_client.py
│   ├── pose_client.py
│   └── README.md
├── data/
│   ├── mri_data/             # Empty directory
│   └── robot_data/           # Empty directory
├── docker-compose.yml
└── README.md                 # Deployment instructions
```

### Deploying on Receiving Machine

On the target machine (with Docker installed):

```bash
cd /path/to/exported/package

# Load Docker images
docker load -i images/mri-marshal.tar
docker load -i images/robot-marshal.tar

# Start services
docker compose up -d

# Verify
curl http://localhost:8080/health
curl http://localhost:8081/read/robot_status

# Test with mock clients
python3 mock_clients/ecg_client.py --count 5
```

### Export Script Details

The [`scripts/export_usb.sh`](../scripts/export_usb.sh) script:
1. Builds both Docker images using `docker compose build`
2. Saves images as `.tar` files using `docker save`
3. Copies `docker-compose.yml` for deployment
4. Copies mock clients from MRI worktree
5. Creates deployment `README.md` with instructions
6. Sets up empty data directory structure

---

## Troubleshooting

### Issue: Health checks failing

**Symptoms:**
```bash
docker compose ps
# Shows: unhealthy
```

**Solutions:**

1. Check logs:
   ```bash
   docker compose logs mri-marshal
   docker compose logs robot-marshal
   ```

2. Verify services are listening:
   ```bash
   docker exec cwru-mri-marshal netstat -tlnp | grep 8080
   docker exec cwru-robot-marshal netstat -tlnp | grep 8081
   ```

3. Manually test health endpoints:
   ```bash
   docker exec cwru-mri-marshal curl http://localhost:8080/health
   docker exec cwru-robot-marshal curl http://localhost:8081/read/robot_status
   ```

### Issue: Port already in use

**Symptoms:**
```
Error: bind: address already in use
```

**Solutions:**

1. Find conflicting process:
   ```bash
   sudo lsof -i :8080
   sudo lsof -i :8081
   ```

2. Stop conflicting service or change ports in `docker-compose.yml`:
   ```yaml
   ports:
     - "8082:8080"  # Change host port
   ```

### Issue: HDF5 file locking errors

**Symptoms:**
```
HDF5-DIAG: Error detected in HDF5 (1.x.x) thread 0:
  unable to lock file
```

**Solution:**

Environment variables are already set in `docker-compose.yml`:
```yaml
environment:
  - HDF5_USE_FILE_LOCKING=FALSE
  - HDF5_FILE_LOCKING=FALSE
```

If still experiencing issues, check NFS/network filesystem settings.

### Issue: Permission denied on volumes

**Symptoms:**
```
mkdir: cannot create directory '/data/mri_data': Permission denied
```

**Solutions:**

1. Fix host directory permissions:
   ```bash
   sudo chown -R $USER:$USER ./data
   chmod -R 755 ./data
   ```

2. Check SELinux context (if applicable):
   ```bash
   sudo chcon -Rt svirt_sandbox_file_t ./data
   ```

### Issue: Build failures

**Symptoms:**
```
Error: failed to build image
```

**Solutions:**

1. Check Docker disk space:
   ```bash
   docker system df
   docker system prune  # Clean up if needed
   ```

2. Rebuild without cache:
   ```bash
   docker compose build --no-cache
   ```

3. Verify network access for git clones:
   ```bash
   curl -I https://github.com
   ```

---

## Advanced Configuration

### Custom Build Arguments

Override repository URLs or branches:

```bash
docker compose build \
  --build-arg REPO_URL=https://github.com/myorg/fork.git \
  --build-arg BRANCH=my-feature-branch
```

### Resource Limits

Add resource constraints in `docker-compose.yml`:

```yaml
services:
  mri-marshal:
    # ... existing config ...
    deploy:
      resources:
        limits:
          cpus: '2.0'
          memory: 4G
        reservations:
          cpus: '1.0'
          memory: 2G
```

### Network Configuration

Access services from other machines:

```yaml
services:
  mri-marshal:
    # ... existing config ...
    ports:
      - "0.0.0.0:8080:8080"  # Expose to all interfaces
```

### Multiple Instances

Run multiple marshal instances on different ports:

```bash
# Create separate compose file
cp docker-compose.yml docker-compose.dev.yml

# Edit ports in docker-compose.dev.yml
# Then run:
docker compose -f docker-compose.dev.yml up -d
```

### Logging Configuration

Add logging drivers:

```yaml
services:
  mri-marshal:
    # ... existing config ...
    logging:
      driver: "json-file"
      options:
        max-size: "10m"
        max-file: "3"
```

---

## Port Reference

| Service | Port | Protocol | Purpose |
|---------|------|----------|---------|
| MRI Marshal | 8080 | HTTP | REST API for data operations |
| MRI Marshal | 8090 | WebSocket | Real-time streaming |
| Robot Marshal | 8081 | HTTP | REST API for robot data |

---

## Related Documentation

- **Mock Clients**: [`/workspaces/mri_data_marshal_worktree/clients/mocks/README.md`](../../mri_data_marshal_worktree/clients/mocks/README.md)
- **USB Export Script**: [`scripts/export_usb.sh`](../scripts/export_usb.sh)
- **Docker Compose**: [`docker-compose.yml`](../docker-compose.yml)
- **Dockerfiles**:
  - [`docker/Dockerfile.mri`](../docker/Dockerfile.mri)
  - [`docker/Dockerfile.robot`](../docker/Dockerfile.robot)

---

## Summary

The Docker deployment provides:

- ✅ **Containerized Services**: MRI and Robot marshals in separate containers
- ✅ **Health Monitoring**: Automatic health checks for both services
- ✅ **Persistent Data**: Bind-mounted volumes for data persistence
- ✅ **Host-based Clients**: Mock clients run on host for easy testing
- ✅ **USB Deployment**: Complete offline deployment package
- ✅ **Production Ready**: Restart policies, resource management, logging

For questions or issues, refer to the troubleshooting section or check the main project repository.
