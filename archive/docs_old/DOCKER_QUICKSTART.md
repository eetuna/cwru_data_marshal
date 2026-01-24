# Docker Quick Start Guide

Step-by-step instructions for building Docker images, running services, and executing demos.

---

## Prerequisites

After rebuilding the devcontainer with Docker support, verify Docker is working:

```bash
docker --version
docker compose version
```

---

## Step 1: Build Docker Images

```bash
# Navigate to project root
cd /workspaces/cwru_data_marshal

# Build both images (takes 5-15 minutes first time)
docker compose build

# Or build individually:
docker compose build mri-marshal
docker compose build robot-marshal

# Verify images were created
docker images | grep cwru
```

**Expected output:**
```
cwru/mri-marshal     latest    abc123...   X minutes ago   1.5GB
cwru/robot-marshal   latest    def456...   X minutes ago   400MB
```

---

## Step 2: Start Services

```bash
# Start both services in background
docker compose up -d

# Or start with logs visible (Ctrl+C to stop)
docker compose up
```

---

## Step 3: Check Health Status

```bash
# View container status
docker compose ps

# Expected output:
# NAME                 STATUS              PORTS
# cwru-mri-marshal     Up (healthy)        0.0.0.0:8080->8080/tcp, 0.0.0.0:8090->8090/tcp
# cwru-robot-marshal   Up (healthy)        0.0.0.0:8081->8081/tcp

# Manual health checks
curl http://localhost:8080/health
curl http://localhost:8081/read/robot_status

# View logs
docker compose logs -f

# View specific service logs
docker compose logs -f mri-marshal
docker compose logs -f robot-marshal
```

---

## Step 4: Test with Mock Clients

Mock clients run on HOST (not in Docker) and connect to containerized services.

```bash
# Send 5 ECG signals
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/ecg_client.py \
  --endpoint http://localhost:8080 \
  --count 5

# Send 20 pose updates
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/pose_client.py \
  --endpoint http://localhost:8080 \
  --count 20

# Continuous ECG (Ctrl+C to stop)
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/ecg_client.py \
  --endpoint http://localhost:8080 \
  --interval 1.0

# Continuous pose at 10Hz (Ctrl+C to stop)
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/pose_client.py \
  --endpoint http://localhost:8080 \
  --interval 0.1
```

---

## Step 5: Run Full Demo

### Option A: Using Demo Script

```bash
# Ensure Docker services are running
docker compose ps

# Run simultaneous demo from MRI worktree
cd /workspaces/mri_data_marshal_worktree
../cwru_data_marshal/scripts/run_demo_simultaneous.sh
```

### Option B: Manual Multi-Terminal Demo

**Terminal 1 - Docker Services (already running):**
```bash
cd /workspaces/cwru_data_marshal
docker compose up
```

**Terminal 2 - Stream MRI Data:**
```bash
cd /workspaces/mri_data_marshal_worktree
./build/image_streamer \
  --endpoint http://localhost:8080 \
  --ws ws://localhost:8090 \
  --stream demo_stream \
  --count 100
```

**Terminal 3 - Visualize Data:**
```bash
cd /workspaces/mri_data_marshal_worktree
./build/viz_client \
  --endpoint http://localhost:8080 \
  --ws ws://localhost:8090
```

**Terminal 4 - Send Pose Updates:**
```bash
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/pose_client.py \
  --endpoint http://localhost:8080 \
  --trajectory circular \
  --interval 0.1
```

**Terminal 5 - Send ECG Signals:**
```bash
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/ecg_client.py \
  --endpoint http://localhost:8080 \
  --interval 1.0
```

---

## Step 6: Check Generated Data

```bash
# View MRI data files
ls -lh /workspaces/cwru_data_marshal/data/mri_data/

# View Robot data files
ls -lh /workspaces/cwru_data_marshal/data/robot_data/

# Inspect a specific file
cat /workspaces/cwru_data_marshal/data/robot_data/robot_status.json
```

---

## Step 7: Stop Services

```bash
# Stop containers (data persists in ./data/)
docker compose down

# Stop and remove everything including volumes
docker compose down -v

# Kill any remaining demo processes
pkill -f viz_client
pkill -f image_streamer
```

---

## Quick Reference Commands

### Docker Compose

| Command | Description |
|---------|-------------|
| `docker compose build` | Build images |
| `docker compose up -d` | Start services (background) |
| `docker compose up` | Start services (foreground) |
| `docker compose ps` | Check status |
| `docker compose logs -f` | View logs |
| `docker compose down` | Stop services |
| `docker compose restart` | Restart services |

### Health Checks

| Endpoint | Service |
|----------|---------|
| `http://localhost:8080/health` | MRI Marshal |
| `http://localhost:8081/read/robot_status` | Robot Marshal |

### Ports

| Port | Service | Protocol |
|------|---------|----------|
| 8080 | MRI Marshal | HTTP |
| 8090 | MRI Marshal | WebSocket |
| 8081 | Robot Marshal | HTTP |

### Data Directories

| Path | Contents |
|------|----------|
| `./data/mri_data/` | MRI HDF5 files (`.h5`) |
| `./data/robot_data/` | Robot JSON files (`.json`) |

---

## USB Export

To export Docker images for offline deployment:

```bash
cd /workspaces/cwru_data_marshal

# Create export package
./scripts/export_usb.sh /path/to/usb/cwru_deploy

# On receiving machine:
cd /path/to/usb/cwru_deploy
docker load -i images/mri-marshal.tar
docker load -i images/robot-marshal.tar
docker compose up -d
```

---

## Troubleshooting

### "docker: command not found"
Rebuild devcontainer with Docker feature enabled.

### "port already in use"
```bash
# Find process using port
sudo lsof -i :8080

# Kill it or change ports in docker-compose.yml
```

### "health check failing"
```bash
# Check logs for errors
docker compose logs mri-marshal
docker compose logs robot-marshal
```

### "permission denied on volumes"
```bash
sudo chown -R $USER:$USER ./data
chmod -R 755 ./data
```

### "viz_client window won't close"
```bash
pkill -f viz_client
```

---

## Architecture Summary

```
┌─────────────────────────────────────────┐
│         Docker Containers               │
│  ┌─────────────┐    ┌──────────────┐   │
│  │ MRI Marshal │    │Robot Marshal │   │
│  │  :8080/:8090│    │    :8081     │   │
│  └─────────────┘    └──────────────┘   │
└──────────┬──────────────────────────────┘
           │ HTTP/WebSocket
┌──────────┴──────────────────────────────┐
│         Host (Clients)                  │
│  • image_streamer (sends MRI data)      │
│  • viz_client (visualizes)              │
│  • ecg_client.py (mock ECG)             │
│  • pose_client.py (mock poses)          │
└─────────────────────────────────────────┘
```

Marshals run in Docker. Clients run on host and connect via localhost ports.
