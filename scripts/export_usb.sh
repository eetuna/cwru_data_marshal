#!/bin/bash
#
# USB Export Script for CWRU Data Marshal Demo
#
# Creates a portable deployment package containing:
# - All 7 Docker images for the demo (as .tar files)
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

# Verify all 7 images were built
REQUIRED_IMAGES=(
    "cwru/mri-marshal"
    "cwru/robot-marshal"
    "cwru/image-streamer"
    "cwru/ecg-client"
    "cwru/pose-client"
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
    cwru/viz-client:latest \
    cwru/robot-clients:latest

# Get total size
TOTAL_SIZE=$(du -h "$OUT_DIR/images/cwru-demo-images.tar" | cut -f1)
echo "  ✓ Saved all 7 images: $TOTAL_SIZE"

# Copy demo files
echo ""
echo "[4/6] Copying demo configuration files..."
cp "$PROJECT_ROOT/docker-compose.demo.yml" "$OUT_DIR/"
cp "$PROJECT_ROOT/scripts/demo-docker.sh" "$OUT_DIR/"
chmod +x "$OUT_DIR/demo-docker.sh"
echo "  ✓ docker-compose.demo.yml"
echo "  ✓ demo-docker.sh"

# Copy documentation
echo ""
echo "[5/6] Copying documentation..."
if [ -f "$PROJECT_ROOT/docs/EXTERNAL_CLIENT_GUIDE.md" ]; then
    cp "$PROJECT_ROOT/docs/EXTERNAL_CLIENT_GUIDE.md" "$OUT_DIR/docs/"
    echo "  ✓ EXTERNAL_CLIENT_GUIDE.md"
fi

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
│   └── cwru-demo-images.tar   # All 7 Docker images (single file)
├── docs/
│   └── EXTERNAL_CLIENT_GUIDE.md  # External client integration guide
├── docker-compose.demo.yml    # Demo configuration
├── demo-docker.sh             # Demo launcher script
└── README.md                  # This file
```

## What's Included

The demo includes:
- **2 Marshals**: MRI Marshal (HTTP + WebSocket) and Robot Marshal
- **3 Mock Data Generators**: Image streamer, ECG client, Pose client
- **5 Robot Clients**: Catheter tracking, Controller, Planning, Front-end, Surface tracking
- **1 Visualization Client**: Real-time MRI image viewer (optional, requires X11)

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

# Load all demo images (single file with 7 images)
docker load -i images/cwru-demo-images.tar

# Verify all images loaded
docker images | grep cwru
```

Expected output (7 images):
```
cwru/mri-marshal        latest
cwru/robot-marshal      latest
cwru/image-streamer     latest
cwru/ecg-client         latest
cwru/pose-client        latest
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

### 3. Verify External Client Access

While demo is running, test the marshal APIs:

```bash
# Check MRI Marshal (should return field strength)
curl -s http://localhost:8080/v1/mrd/latest/header | jq '.acquisitionSystemInformation.systemFieldStrength_T'

# Check Robot Marshal (should return HTML or JSON)
curl -s http://localhost:8081/
```

## Service Information

### Marshal Endpoints (for External Clients)

- **MRI Marshal HTTP:** http://localhost:8080
- **MRI Marshal WebSocket:** ws://localhost:8090
- **Robot Marshal HTTP:** http://localhost:8081

See `docs/EXTERNAL_CLIENT_GUIDE.md` for complete API documentation.

### Demo Configuration

Edit `demo-docker.sh` to change:
- `DEMO_DURATION` - Demo run time (default: 30s)
- `IMAGE_INTERVAL_MS` - Image streaming rate (default: 50ms = 20fps)
- `ECG_INTERVAL` - ECG sample rate (default: 0.5s)
- `POSE_INTERVAL` - Pose update rate (default: 0.1s)
- `ENABLE_VIZ` - Show visualization window (default: true)
- `CLEANUP_DATA` - Remove mri-data volume after demo (default: true, set false to keep data)

### Manual Control

Instead of using `demo-docker.sh`, you can manually control services:

```bash
# Start all services (no viz)
docker compose -f docker-compose.demo.yml up -d

# Start with visualization
docker compose -f docker-compose.demo.yml --profile viz up -d

# Stop all services
docker compose -f docker-compose.demo.yml down

# View logs
docker compose -f docker-compose.demo.yml logs -f

# Check status
docker compose -f docker-compose.demo.yml ps
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
    cwru/pose-client:latest cwru/viz-client:latest \
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
echo "  ✓ 7 Docker images ($TOTAL_SIZE)"
echo "  ✓ docker-compose.demo.yml"
echo "  ✓ demo-docker.sh launcher"
echo "  ✓ External client documentation"
echo "  ✓ README with setup instructions"
echo ""
echo "Next steps:"
echo "  1. Copy $OUT_DIR to USB drive"
echo "  2. On receiving machine:"
echo "     - Load images: docker load -i images/cwru-demo-images.tar"
echo "     - Run demo: ./demo-docker.sh"
echo ""
