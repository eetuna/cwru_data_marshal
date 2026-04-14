#!/bin/bash
#
# USB Export Script for CWRU Data Marshal Demo
#
# Creates a portable deployment package containing:
# - All 9 Docker images for the demo (as .tar files)
# - docker-compose.demo.yml
# - demo-docker.sh launcher script
# - External client integration guide
# - README with instructions for receiver
#
# Usage:
#   ./scripts/export_usb.sh <output_directory>
#
# Example:
#   ./scripts/export_usb.sh /media/usb/cwru_marshal_demo
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Check arguments
if [ $# -ne 1 ]; then
    echo "Usage: $0 <output_directory>"
    echo ""
    echo "Example:"
    echo "  $0 /media/usb/cwru_marshal_deploy"
    exit 1
fi

OUT_DIR="$1"

# Validate output directory
if [ -e "$OUT_DIR" ]; then
    echo "Error: Output directory already exists: $OUT_DIR"
    echo "Please remove it first or choose a different location."
    exit 1
fi

echo "============================================"
echo "CWRU Data Marshal - USB Export Script"
echo "============================================"
echo ""
echo "Output directory: $OUT_DIR"
echo "Project root: $PROJECT_ROOT"
echo ""

# Create directory structure
echo "[1/6] Creating directory structure..."
mkdir -p "$OUT_DIR"/{images,docs}

# Build Docker images
echo ""
echo "[2/6] Building Docker images..."
cd "$PROJECT_ROOT"
docker compose -f docker-compose.demo.yml build

# Verify all 9 images were built
REQUIRED_IMAGES=(
    "cwru/mri-marshal"
    "cwru/robot-marshal"
    "cwru/image-streamer"
    "cwru/ecg-client"
    "cwru/pose-client"
    "cwru/kspace-streamer"
    "cwru/mock-recon"
    "cwru/viz-client"
    "cwru/robot-clients"
)

echo "Verifying images..."
for img in "${REQUIRED_IMAGES[@]}"; do
    if ! docker images | grep -q "$img"; then
        echo "Error: $img image not found after build"
        exit 1
    fi
    echo "  ✓ $img"
done

# Save Docker images to tar files
echo ""
echo "[3/6] Exporting Docker images (this may take several minutes)..."
docker save -o "$OUT_DIR/images/cwru-demo-images.tar" \
    cwru/mri-marshal:latest \
    cwru/robot-marshal:latest \
    cwru/image-streamer:latest \
    cwru/ecg-client:latest \
    cwru/pose-client:latest \
    cwru/kspace-streamer:latest \
    cwru/mock-recon:latest \
    cwru/viz-client:latest \
    cwru/robot-clients:latest

# Get total size
TOTAL_SIZE=$(du -h "$OUT_DIR/images/cwru-demo-images.tar" | cut -f1)
echo "  ✓ Saved all 9 images: $TOTAL_SIZE"

# Copy demo files
echo ""
echo "[4/6] Copying demo configuration files..."
cp "$PROJECT_ROOT/docker-compose.demo.yml" "$OUT_DIR/"
cp "$PROJECT_ROOT/.env.demo" "$OUT_DIR/"
cp "$PROJECT_ROOT/scripts/demo-docker.sh" "$OUT_DIR/"
cp "$PROJECT_ROOT/scripts/demo-persistent.sh" "$OUT_DIR/"
chmod +x "$OUT_DIR/demo-docker.sh"
chmod +x "$OUT_DIR/demo-persistent.sh"
echo "  ✓ docker-compose.demo.yml"
echo "  ✓ .env.demo"
echo "  ✓ demo-docker.sh"
echo "  ✓ demo-persistent.sh"

# Copy documentation
echo ""
echo "[5/6] Copying documentation..."
if [ -f "$PROJECT_ROOT/docs/EXTERNAL_CLIENT_GUIDE.md" ]; then
    cp "$PROJECT_ROOT/docs/EXTERNAL_CLIENT_GUIDE.md" "$OUT_DIR/docs/"
    echo "  ✓ EXTERNAL_CLIENT_GUIDE.md"
fi
if [ -f "$PROJECT_ROOT/docs/DEMO_AND_API_EXPORT.md" ]; then
    cp "$PROJECT_ROOT/docs/DEMO_AND_API_EXPORT.md" "$OUT_DIR/docs/"
    echo "  ✓ DEMO_AND_API_EXPORT.md"
fi

# Create comprehensive API reference
echo "  ✓ Creating API_REFERENCE.md..."
cat > "$OUT_DIR/docs/API_REFERENCE.md" <<'APIEOF'
# CWRU Data Marshal - Complete API Reference

Complete reference for connecting external clients to the CWRU Data Marshal system.

## MRI Marshal API (Port 8080)

### Scanner/recon-facing MRD TCP

Scanner-side clients connect to port `9100` and send the same raw MRD TCP wire
protocol used by python-ismrmrd-server: 2-byte message tag followed by the
tag-specific body. The marshal forwards scanner messages to recon over MRD TCP
when recon is configured, and pushes recon return messages back to the scanner
on the original TCP connection.

### Query endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/image/latest` | GET | Path to latest reconstructed image file |
| `/transform` | GET | Slice transform delta (atomically zeros after read) |
| `/transform` | PUT | Write new slice transform delta |
| `/pose` | GET | Latest cached pose |
| `/pose` | POST | Submit pose update |
| `/health` | GET | Health check |
| `/dump/scanner` | GET | List archived scanner HDF5 files |
| `/dump/recon` | GET | List archived reconstruction HDF5 files |

### Data flow

Scanner sends CONFIG + METADATA_XML + ACQUISITION/IMAGE/WAVEFORM + CLOSE over
MRD TCP. Marshal forwards to recon over MRD TCP when `--recon-host` is set.
Recon returns IMAGE/WAVEFORM/TEXT/CLOSE over MRD TCP; marshal pushes those
messages back to the scanner on the original scanner socket. With `--dump`,
standard ISMRMRD objects are recorded as canonical H5 under `from_scanner/` and
`from_reconstruction/`. Live clients use `GET /image/latest` to obtain the
standalone latest-image file path.

## Robot Marshal API (Port 8081)

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | List available data channels |
| `/read/<filename>` | GET | Read data from channel |
| `/write/<filename>` | POST | Write data to channel |

## Example Usage

```bash
# Health check
curl http://localhost:8080/health

# Get latest reconstructed image path
curl http://localhost:8080/image/latest

# Get current pose
curl http://localhost:8080/pose

# Submit pose
curl -X POST http://localhost:8080/pose \
  -H "Content-Type: application/json" \
  -d '{"position":[1,2,3],"orientation":[0,0,0.707,0.707]}'

# Get/set slice transform
curl http://localhost:8080/transform
curl -X PUT http://localhost:8080/transform \
  -H "Content-Type: application/json" \
  -d '{"through_plane_mm":1.0,"readout_mm":0,"phase_mm":0,"rotation_rad":0}'

# List archived files
curl http://localhost:8080/dump/scanner
curl http://localhost:8080/dump/recon
```
APIEOF

echo "  ✓ API_REFERENCE.md"

# Create README for receiver
echo ""
echo "[6/6] Creating deployment README..."
cat > "$OUT_DIR/README.md" <<'EOF'
# CWRU Data Marshal - Demo Package

This package contains a complete, self-contained demo of the CWRU Data Marshal system.

## Contents

```
.
├── images/
│   └── cwru-demo-images.tar   # All 9 Docker images (single file)
├── docs/
│   ├── API_REFERENCE.md         # Complete API documentation
│   ├── EXTERNAL_CLIENT_GUIDE.md # External client integration guide
│   └── DEMO_AND_API_EXPORT.md   # Demo + API export guide (if available)
├── docker-compose.demo.yml    # Service orchestration
├── .env.demo                  # Configuration settings
├── demo-docker.sh             # Quick 30-second demo
├── demo-persistent.sh         # Persistent demo (services stay running)
└── README.md                  # This file
```

## What's Included

The demo includes:
- **2 Marshals**: MRI Marshal (HTTP) and Robot Marshal
- **5 Mock Data Generators**: Image streamer, K-space streamer, Mock recon, ECG client, Pose client
- **5 Robot Clients**: Catheter tracking, Controller, Planning, Front-end, Surface tracking
- **1 Visualization Client**: Real-time image viewer (optional, requires X11)

**Note:** All components are pre-built and packaged as Docker images. Source code is available at https://github.com/cwru-mercis/cwru_data_marshal for developers who wish to modify or rebuild components.

## Prerequisites

The receiving machine must have:
- Docker Engine (version 20.10 or later)
- Docker Compose (version 2.0 or later)
- X11 display server (for visualization client - optional)

### Installing Docker

**Ubuntu/Debian:**
```bash
curl -fsSL https://get.docker.com -o get-docker.sh
sudo sh get-docker.sh
sudo usermod -aG docker $USER
# Log out and back in for group changes
```

**Other platforms:** See https://docs.docker.com/get-docker/

## Quick Start

### 1. Load Docker Images

```bash
cd /path/to/this/directory

# Load all demo images (single file with 9 images)
docker load -i images/cwru-demo-images.tar

# Verify all images loaded
docker images | grep cwru
```

Expected output (9 images):
```
cwru/mri-marshal        latest
cwru/robot-marshal      latest
cwru/image-streamer     latest
cwru/ecg-client         latest
cwru/pose-client        latest
cwru/kspace-streamer    latest
cwru/mock-recon         latest
cwru/viz-client         latest
cwru/robot-clients      latest
```

### 2. Run the Demo

```bash
# Run 30-second demo with all services
./demo-docker.sh

# The demo will show:
# - ECG data streaming
# - Pose tracking data
# - Image frames being generated
# - Robot operations count
# - Visualization window (if X11 available)
```

### 3. Access Generated Data

**Data is automatically visible in the `session-data/` directory!**

```bash
# View generated files
ls -lh session-data/mrd/

# Expected directories:
# - from_scanner/        (scanner HDF5 files when --dump is enabled)
# - from_reconstruction/ (recon HDF5 files when --dump is enabled, plus latest_image.h5)
```

**Note:** By default, `CLEANUP_DATA=true` clears this directory after each demo run. To keep data between runs, edit `.env.demo` and set `CLEANUP_DATA=false`.

### 4. Verify External Client Access

While demo is running, test the marshal APIs:

```bash
# Check MRI Marshal health
curl -s http://localhost:8080/health

# Get latest reconstructed image path
curl -s http://localhost:8080/image/latest | jq

# Get current pose
curl -s http://localhost:8080/pose | jq

# Check Robot Marshal
curl -s http://localhost:8081/

# Read catheter tip position
curl -s http://localhost:8081/read/tip_position_orientation | jq
```

## Service Information

### Marshal Endpoints (for External Clients)

- **MRI Marshal HTTP:** http://localhost:8080
- **Robot Marshal HTTP:** http://localhost:8081

### Complete API Documentation

**See `docs/API_REFERENCE.md` for:**
- Complete endpoint reference for both marshals
- Scanner, recon, and query endpoint reference
- Example curl commands
- Data flow overview

**Also see:**
- `docs/EXTERNAL_CLIENT_GUIDE.md` - External client integration guide
- `docs/DEMO_AND_API_EXPORT.md` - Demo and API export guide (if available)

### Demo Configuration

Edit `.env.demo` to change:
- `DEMO_DURATION` - Demo run time (default: 30s, 0 = infinite)
- `IMAGE_WIDTH` / `IMAGE_HEIGHT` - Image dimensions (default: 64x64)
- `IMAGE_SLICES` - Slices per volume (default: 5)
- `IMAGE_INTERVAL` - Image streaming rate (default: 50ms = 20fps)
- `ECG_INTERVAL` - ECG sample rate (default: 0.5s)
- `POSE_INTERVAL` - Pose update rate (default: 0.1s)
- `ENABLE_VIZ` - Show visualization window (default: true, requires X11)
- `CLEANUP_DATA` - Remove session-data/ after demo (default: true, set false to keep data)

### Persistent Demo (Keep Services Running)

For development/testing where you want services to keep running:

```bash
./demo-persistent.sh

# Services will start and stay running even after monitoring stops
# Re-run the script to attach monitor again
# To stop: docker compose -f docker-compose.demo.yml down
```

### Manual Control - Run Each Service Separately

**Perfect for connecting your own clients!** Run each service in its own terminal:

**Terminal 1: MRI Marshal**
```bash
docker compose -f docker-compose.demo.yml up mri-marshal
```

**Terminal 2: Robot Marshal**
```bash
docker compose -f docker-compose.demo.yml up robot-marshal
```

**Terminal 3: Image Streamer (sends MRI data)**
```bash
docker compose -f docker-compose.demo.yml up image-streamer
```

**Terminal 4: ECG Client**
```bash
docker compose -f docker-compose.demo.yml up ecg-client
```

**Terminal 5: Pose Client**
```bash
docker compose -f docker-compose.demo.yml up pose-client
```

**Terminal 6: Robot Clients (all 5 together)**
```bash
docker compose -f docker-compose.demo.yml up robot-clients
```

**Terminal 7: Viz Client (optional)**
```bash
docker compose -f docker-compose.demo.yml --profile viz up viz-client
```

**Benefits:**
- See each service's logs in real-time
- Stop/restart individual services easily (Ctrl+C in that terminal)
- Connect your own custom clients while demo is running
- Test individual components in isolation

### Other Manual Commands

```bash
# Start all services in background (daemon mode)
docker compose -f docker-compose.demo.yml up -d

# Stop all services
docker compose -f docker-compose.demo.yml down

# View logs from all services
docker compose -f docker-compose.demo.yml logs -f

# View logs from specific service
docker compose -f docker-compose.demo.yml logs -f mri-marshal

# Check status
docker compose -f docker-compose.demo.yml ps

# Restart a specific service
docker compose -f docker-compose.demo.yml restart mri-marshal
```

## Troubleshooting

### Docker daemon not running
```bash
sudo systemctl start docker
sudo usermod -aG docker $USER  # Log out and back in after this
```

### Port already in use
```bash
sudo lsof -i :8080  # Find process using port
docker compose -f docker-compose.demo.yml down  # Stop demo services
```

### Visualization client not showing
- Ensure `ENABLE_VIZ=true` in `demo-docker.sh`
- Check X11 is available: `echo $DISPLAY` should show `:0` or similar
- WSL2 users: Install X server on Windows (VcXsrv, Xming, or use WSLg)

### Containers not healthy
```bash
docker compose -f docker-compose.demo.yml logs mri-marshal
docker compose -f docker-compose.demo.yml logs robot-marshal
```

## Known Limitations

- **Viz client display FPS**: Shows ~15 fps due to Docker X11 forwarding overhead
  - Core system (marshals) operates at full 20+ fps
  - Only affects display, not data ingestion or client APIs
  - See `docs/EXTERNAL_CLIENT_GUIDE.md` for details

## Cleanup

```bash
# Stop all services
docker compose -f docker-compose.demo.yml down

# Remove all demo images
docker rmi cwru/mri-marshal:latest cwru/robot-marshal:latest \
    cwru/image-streamer:latest cwru/ecg-client:latest \
    cwru/pose-client:latest cwru/kspace-streamer:latest \
    cwru/mock-recon:latest cwru/viz-client:latest \
    cwru/robot-clients:latest
```

## Support

For issues or questions:
- Review documentation: `docs/EXTERNAL_CLIENT_GUIDE.md`
- Check logs: `docker compose -f docker-compose.demo.yml logs`
- Project repository: https://github.com/cwru-mercis/cwru_data_marshal

---

**Package created:** $(date)
**Requirements:** Docker Engine 20.10+, Docker Compose 2.0+
EOF

# Summary
echo ""
echo "============================================"
echo "Export Complete!"
echo "============================================"
echo ""
echo "Package location: $OUT_DIR"
echo "Package size: $(du -sh "$OUT_DIR" | cut -f1)"
echo ""
echo "Contents:"
echo "  ✓ 9 Docker images ($TOTAL_SIZE)"
echo "  ✓ docker-compose.demo.yml (orchestration)"
echo "  ✓ .env.demo (configuration)"
echo "  ✓ demo-docker.sh (30-second demo)"
echo "  ✓ demo-persistent.sh (keep services running)"
echo "  ✓ API_REFERENCE.md (complete API docs)"
echo "  ✓ EXTERNAL_CLIENT_GUIDE.md (if available)"
echo "  ✓ DEMO_AND_API_EXPORT.md (if available)"
echo "  ✓ README with setup instructions"
echo ""
echo "Usage modes:"
echo "  1. Quick demo: ./demo-docker.sh"
echo "  2. Persistent: ./demo-persistent.sh"
echo "  3. Manual per-service: docker compose -f docker-compose.demo.yml up <service>"
echo ""
echo "Next steps:"
echo "  1. Copy $OUT_DIR to USB drive"
echo "  2. On receiving machine:"
echo "     - Load images: docker load -i images/cwru-demo-images.tar"
echo "     - Run demo: ./demo-docker.sh"
echo "     - Or run services separately to connect your own clients"
echo ""
