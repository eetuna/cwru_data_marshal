# Docker Demo - Step by Step Guide

Complete walkthrough for running the CWRU Data Marshal Docker demo.

---

## Prerequisites Check

### 1. Verify Docker Installation

```bash
# Check Docker is installed and running
docker --version
# Expected: Docker version 20.10 or later

# Check Docker Compose is installed
docker compose version
# Expected: Docker Compose version 2.0 or later

# Test Docker works
docker ps
# Should show running containers (or empty list)
```

### 2. Verify X11 Display (For Visualization)

```bash
# Check DISPLAY environment variable
echo $DISPLAY
# Expected: :0 or similar

# If empty, set it
export DISPLAY=:0
```

### 3. Grant X11 Access to Docker (Required for viz-client)

```bash
# Allow Docker containers to access your X server
xhost +local:docker

# Verify xhost settings
xhost
# Should show: "access control disabled, clients can connect from any host"
# or "LOCAL:docker" in the list
```

**Note:** For WSL2 users, you may need an X server on Windows (VcXsrv, Xming, or use WSLg).

---

## Quick Start (Automated)

### Option 1: Run with Default Settings

```bash
# Navigate to project root
cd /workspaces/cwru_data_marshal

# Run 30-second demo with all defaults
./scripts/demo-docker.sh
```

**What happens:**
- Builds all 7 Docker images (if not already built)
- Starts 2 marshals, 3 mock clients, 5 robot clients, 1 viz client
- Runs for 30 seconds showing ECG, Pose, and Image streaming
- Displays robot operations count every 2 seconds
- Opens visualization window (if X11 configured)
- Cleans up all data after demo ends

---

## Manual Step-by-Step

### Step 1: Review Configuration

```bash
# Edit demo settings (optional)
nano .env.demo
```

**Key settings:**
- `IMAGE_WIDTH=64` - Image width (pixels)
- `IMAGE_HEIGHT=64` - Image height (pixels)
- `IMAGE_SLICES=5` - Slices per volume
- `IMAGE_INTERVAL=50` - Streaming rate (50ms = 20fps)
- `DEMO_DURATION=30` - Demo length (0 = infinite)
- `ENABLE_VIZ=true` - Show visualization window
- `CLEANUP_DATA=true` - Remove data after demo

### Step 2: Build Docker Images

```bash
# Build all 7 images (takes 5-10 minutes first time)
docker compose -f docker-compose.demo.yml build

# Verify images were created
docker images | grep cwru
```

**Expected output:**
```
cwru/mri-marshal        latest
cwru/robot-marshal      latest
cwru/image-streamer     latest
cwru/ecg-client         latest
cwru/pose-client        latest
cwru/viz-client         latest
cwru/robot-clients      latest
```

### Step 3: Start Services

```bash
# Option A: Start without visualization
docker compose -f docker-compose.demo.yml up -d

# Option B: Start with visualization (requires X11)
docker compose -f docker-compose.demo.yml --profile viz up -d
```

**What happens:**
1. Creates `mri-data` volume
2. Starts `mri-marshal` (HTTP :8080, WebSocket :8090)
3. Starts `robot-marshal` (HTTP :8081)
4. Waits for marshals to become healthy
5. Starts all clients (image-streamer, ecg, pose, robot clients)
6. Starts viz-client (if using --profile viz)

### Step 4: Verify Services Are Running

```bash
# Check all containers are up and healthy
docker compose -f docker-compose.demo.yml ps
```

**Expected output:**
```
NAME                      STATUS
cwru-mri-marshal          Up (healthy)
cwru-robot-marshal        Up (healthy)
cwru-image-streamer       Up
cwru-ecg-client          Up
cwru-pose-client         Up
cwru-viz-client          Up (if enabled)
cwru-catheter-tracking   Up
cwru-controller          Up
cwru-planning            Up
cwru-front-end           Up
cwru-surface-tracking    Up
```

### Step 5: Monitor Services

```bash
# Watch all logs in real-time
docker compose -f docker-compose.demo.yml logs -f

# Watch specific service
docker compose -f docker-compose.demo.yml logs -f image-streamer

# Watch only MRI-related streams (filtered)
docker compose -f docker-compose.demo.yml logs -f 2>&1 | grep -E "ecg-client|pose-client|image-streamer"
```

### Step 6: Test Marshal APIs

Open a new terminal while demo is running:

```bash
# Test MRI Marshal health
curl -s http://localhost:8080/health | jq '.'

# Get latest MRI data header
curl -s http://localhost:8080/v1/mrd/latest/header | jq '.acquisitionSystemInformation'

# List available MRI files
curl -s http://localhost:8080/v1/mrd/list | jq '.'

# Test Robot Marshal
curl -s http://localhost:8081/ | head -20
```

### Step 7: Access Generated Data

```bash
# View files inside running marshal
docker exec cwru-mri-marshal ls -lh /data/mri_data/mrd/

# Copy data to local directory
docker cp cwru-mri-marshal:/data/mri_data/ ./demo-output/

# Check volume location on host
docker volume inspect cwru_data_marshal_mri-data
```

**Expected files:**
- `demo_stream-64x64x5-g0000.mrd` - MRI image data
- `index.jsonl` - Frame metadata index
- `latest.json` - Latest frame pointer
- `bio.jsonl` - ECG data log
- `poses.jsonl` - Pose tracking data log

### Step 8: Stop Services

```bash
# Stop all containers
docker compose -f docker-compose.demo.yml down

# Stop and remove data volume (clean slate)
docker compose -f docker-compose.demo.yml down
docker volume rm cwru_data_marshal_mri-data
```

---

## Customization Examples

### Example 1: Change Image Resolution

```bash
# Edit .env.demo
sed -i 's/IMAGE_WIDTH=64/IMAGE_WIDTH=128/' .env.demo
sed -i 's/IMAGE_HEIGHT=64/IMAGE_HEIGHT=128/' .env.demo

# Run demo with new settings
./scripts/demo-docker.sh
```

### Example 2: Run Infinite Demo (Manual Stop)

```bash
# Edit .env.demo
sed -i 's/DEMO_DURATION=30/DEMO_DURATION=0/' .env.demo

# Start demo (runs until Ctrl+C)
./scripts/demo-docker.sh
```

### Example 3: Keep Data Between Runs

```bash
# Edit .env.demo
sed -i 's/CLEANUP_DATA=true/CLEANUP_DATA=false/' .env.demo

# Run demo multiple times - data accumulates
./scripts/demo-docker.sh
./scripts/demo-docker.sh

# View accumulated data
docker exec cwru-mri-marshal ls -lh /data/mri_data/mrd/
```

### Example 4: Faster Frame Rate

```bash
# Edit .env.demo - change from 50ms (20fps) to 25ms (40fps)
sed -i 's/IMAGE_INTERVAL=50/IMAGE_INTERVAL=25/' .env.demo

# Run demo
./scripts/demo-docker.sh
```

### Example 5: No Visualization (Headless)

```bash
# Edit .env.demo
sed -i 's/ENABLE_VIZ=true/ENABLE_VIZ=false/' .env.demo

# Run demo without GUI
./scripts/demo-docker.sh
```

---

## Troubleshooting

### Issue: "Cannot connect to Docker daemon"

```bash
# Start Docker service
sudo systemctl start docker

# Add your user to docker group (requires logout/login)
sudo usermod -aG docker $USER
```

### Issue: Visualization window doesn't appear

```bash
# Check DISPLAY is set
echo $DISPLAY

# Grant X11 access
xhost +local:docker

# Restart viz-client
docker compose -f docker-compose.demo.yml restart viz-client

# Check viz-client logs for errors
docker logs cwru-viz-client
```

### Issue: "Port already in use"

```bash
# Find process using port 8080
sudo lsof -i :8080

# Kill existing demo
docker compose -f docker-compose.demo.yml down

# Or kill specific process
kill <PID>
```

### Issue: Image streamer not producing data

```bash
# Check image-streamer logs
docker logs cwru-image-streamer

# Verify marshal is healthy
docker compose -f docker-compose.demo.yml ps mri-marshal

# Check if marshal is receiving data
curl -s http://localhost:8080/v1/mrd/list | jq '.'
```

### Issue: Containers fail health checks

```bash
# Check marshal logs
docker logs cwru-mri-marshal
docker logs cwru-robot-marshal

# Restart services
docker compose -f docker-compose.demo.yml restart
```

### Issue: "Error response from daemon: conflict"

```bash
# Remove conflicting containers
docker compose -f docker-compose.demo.yml down
docker rm -f $(docker ps -aq)

# Clean up
docker system prune -f
```

---

## Performance Tuning

### Higher Resolution Images

```bash
# In .env.demo, set:
IMAGE_WIDTH=256
IMAGE_HEIGHT=256
IMAGE_SLICES=10
IMAGE_INTERVAL=100  # Slower rate for larger images
```

### Lower Resource Usage

```bash
# In .env.demo, set:
IMAGE_WIDTH=32
IMAGE_HEIGHT=32
IMAGE_SLICES=3
IMAGE_INTERVAL=100
ECG_INTERVAL=1.0
POSE_INTERVAL=0.5
```

### Production-like Settings

```bash
# In .env.demo, set:
IMAGE_WIDTH=256
IMAGE_HEIGHT=256
IMAGE_SLICES=20
IMAGE_INTERVAL=50  # 20fps
ECG_INTERVAL=0.1   # 10Hz
POSE_INTERVAL=0.05 # 20Hz
DEMO_DURATION=0    # Infinite
CLEANUP_DATA=false # Persist data
```

---

## Cleanup

### Remove All Demo Artifacts

```bash
# Stop all containers
docker compose -f docker-compose.demo.yml down

# Remove data volume
docker volume rm cwru_data_marshal_mri-data

# Remove all demo images
docker rmi cwru/mri-marshal:latest \
           cwru/robot-marshal:latest \
           cwru/image-streamer:latest \
           cwru/ecg-client:latest \
           cwru/pose-client:latest \
           cwru/viz-client:latest \
           cwru/robot-clients:latest

# Revoke X11 access
xhost -local:docker
```

### Full Docker Cleanup (Use with Caution)

```bash
# Remove all stopped containers
docker container prune -f

# Remove unused images
docker image prune -a -f

# Remove unused volumes
docker volume prune -f

# Remove unused networks
docker network prune -f
```

---

## Next Steps

- **USB Export**: Use `./scripts/export_usb.sh /path/to/output` to create portable demo package
- **External Clients**: See `docs/EXTERNAL_CLIENT_GUIDE.md` for API integration
- **Development**: Mount source code into containers for live development

---

**Quick Reference:**

| Command | Purpose |
|---------|---------|
| `./scripts/demo-docker.sh` | Run full demo (automated) |
| `docker compose -f docker-compose.demo.yml build` | Build images |
| `docker compose -f docker-compose.demo.yml up -d` | Start services |
| `docker compose -f docker-compose.demo.yml ps` | Check status |
| `docker compose -f docker-compose.demo.yml logs -f` | Watch logs |
| `docker compose -f docker-compose.demo.yml down` | Stop services |
| `xhost +local:docker` | Enable GUI |
| `curl http://localhost:8080/health` | Test MRI marshal |
| `curl http://localhost:8081/` | Test Robot marshal |
