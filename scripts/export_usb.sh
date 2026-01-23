#!/bin/bash
#
# USB Export Script for CWRU Data Marshal
#
# Creates a portable deployment package containing:
# - Docker images (as .tar files)
# - docker-compose.yml
# - Mock clients from MRI worktree
# - README with instructions for receiver
#
# Usage:
#   ./scripts/export_usb.sh <output_directory>
#
# Example:
#   ./scripts/export_usb.sh /media/usb/cwru_marshal_deploy
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MRI_WORKTREE="/workspaces/mri_data_marshal_worktree"

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
echo "MRI worktree: $MRI_WORKTREE"
echo ""

# Create directory structure
echo "[1/7] Creating directory structure..."
mkdir -p "$OUT_DIR"/{images,mock_clients,data/mri_data,data/robot_data}

# Build Docker images
echo ""
echo "[2/7] Building Docker images..."
cd "$PROJECT_ROOT"
docker compose build

# Verify images were built
if ! docker images | grep -q "cwru/mri-marshal"; then
    echo "Error: MRI Marshal image not found after build"
    exit 1
fi

if ! docker images | grep -q "cwru/robot-marshal"; then
    echo "Error: Robot Marshal image not found after build"
    exit 1
fi

# Save Docker images to tar files
echo ""
echo "[3/7] Exporting Docker images..."
echo "  - Saving MRI Marshal image (this may take a few minutes)..."
docker save -o "$OUT_DIR/images/mri-marshal.tar" cwru/mri-marshal:latest

echo "  - Saving Robot Marshal image..."
docker save -o "$OUT_DIR/images/robot-marshal.tar" cwru/robot-marshal:latest

# Get image sizes
MRI_SIZE=$(du -h "$OUT_DIR/images/mri-marshal.tar" | cut -f1)
ROBOT_SIZE=$(du -h "$OUT_DIR/images/robot-marshal.tar" | cut -f1)
echo "  - MRI Marshal: $MRI_SIZE"
echo "  - Robot Marshal: $ROBOT_SIZE"

# Copy docker-compose.yml
echo ""
echo "[4/7] Copying docker-compose.yml..."
cp "$PROJECT_ROOT/docker-compose.yml" "$OUT_DIR/"

# Copy mock clients from MRI worktree
echo ""
echo "[5/7] Copying mock clients from MRI worktree..."
if [ -d "$MRI_WORKTREE/clients/mocks" ]; then
    cp "$MRI_WORKTREE/clients/mocks/ecg_client.py" "$OUT_DIR/mock_clients/"
    cp "$MRI_WORKTREE/clients/mocks/pose_client.py" "$OUT_DIR/mock_clients/"
    cp "$MRI_WORKTREE/clients/mocks/README.md" "$OUT_DIR/mock_clients/"
    chmod +x "$OUT_DIR/mock_clients/"*.py
    echo "  - Copied: ecg_client.py, pose_client.py, README.md"
else
    echo "  Warning: MRI worktree mock clients not found at $MRI_WORKTREE/clients/mocks"
    echo "  Creating placeholder mock_clients directory..."
fi

# Create .gitignore for data directories
echo ""
echo "[6/7] Creating data directory placeholders..."
cat > "$OUT_DIR/data/.gitignore" <<'EOF'
# Data directories for runtime use
mri_data/*
robot_data/*

# Keep directory structure
!.gitignore
EOF

# Create README for receiver
echo ""
echo "[7/7] Creating deployment README..."
cat > "$OUT_DIR/README.md" <<'EOF'
# CWRU Data Marshal - Docker Deployment Package

This package contains everything needed to run the CWRU Data Marshal system using Docker.

## Contents

```
.
├── images/
│   ├── mri-marshal.tar       # MRI Marshal Docker image
│   └── robot-marshal.tar     # Robot Marshal Docker image
├── mock_clients/
│   ├── ecg_client.py          # Mock ECG signal generator
│   ├── pose_client.py         # Mock pose tracker
│   └── README.md              # Mock client documentation
├── data/
│   ├── mri_data/              # MRI data storage (empty)
│   └── robot_data/            # Robot data storage (empty)
├── docker-compose.yml         # Docker Compose configuration
└── README.md                  # This file
```

## Prerequisites

The receiving machine must have:
- Docker Engine (version 20.10 or later)
- Docker Compose (version 2.0 or later)
- Python 3.6+ (for mock clients - optional)

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

# Load MRI Marshal image
docker load -i images/mri-marshal.tar

# Load Robot Marshal image
docker load -i images/robot-marshal.tar

# Verify images loaded
docker images | grep cwru
```

Expected output:
```
cwru/mri-marshal     latest    <image-id>   <size>
cwru/robot-marshal   latest    <image-id>   <size>
```

### 2. Start Services

```bash
# Start both marshals
docker compose up -d

# Check status
docker compose ps

# View logs
docker compose logs -f
```

### 3. Verify Health

```bash
# Check MRI Marshal
curl http://localhost:8080/health

# Check Robot Marshal
curl http://localhost:8081/read/robot_status
```

Expected response: `{"ok": true}` (or similar)

### 4. Test with Mock Clients (Optional)

```bash
# Send 5 ECG signals
python3 mock_clients/ecg_client.py --count 5

# Send 20 pose updates
python3 mock_clients/pose_client.py --count 20

# See mock_clients/README.md for full documentation
```

## Service Information

### Ports

- **MRI Marshal HTTP:** 8080
- **MRI Marshal WebSocket:** 8090
- **Robot Marshal HTTP:** 8081

### Data Volumes

Data is persisted in bind-mounted directories:
- `./data/mri_data/` - MRI HDF5 files
- `./data/robot_data/` - Robot JSON files

### Environment Variables

The compose file sets HDF5 locking variables for WSL2/NFS compatibility:
- `HDF5_USE_FILE_LOCKING=FALSE`
- `HDF5_FILE_LOCKING=FALSE`

## Common Operations

### Stop Services
```bash
docker compose down
```

### View Logs
```bash
# All services
docker compose logs -f

# Specific service
docker compose logs -f mri-marshal
docker compose logs -f robot-marshal
```

### Restart Services
```bash
docker compose restart
```

### Check Health
```bash
docker compose ps
```

Services should show `healthy` status after start period.

### Update Images

To update with new images:
1. Stop services: `docker compose down`
2. Load new images: `docker load -i images/mri-marshal.tar`
3. Start services: `docker compose up -d`

## Troubleshooting

### Issue: "Cannot connect to the Docker daemon"
**Solution:** Ensure Docker is running and your user is in the `docker` group.

```bash
sudo systemctl start docker
sudo usermod -aG docker $USER
# Log out and back in
```

### Issue: Port already in use
**Solution:** Check for conflicting processes.

```bash
# Find process using port 8080
sudo lsof -i :8080

# Stop conflicting service or change port in docker-compose.yml
```

### Issue: Health check failing
**Solution:** Check container logs.

```bash
docker compose logs mri-marshal
docker compose logs robot-marshal
```

### Issue: Permission denied on data directories
**Solution:** Adjust directory permissions.

```bash
sudo chown -R $USER:$USER ./data
chmod -R 755 ./data
```

### Issue: Mock clients fail to connect
**Solution:** Verify services are running and healthy.

```bash
docker compose ps
curl http://localhost:8080/health
```

## Network Access

### Access from Other Machines

If you need to access the marshals from other computers on the network:

1. Find the host machine's IP address:
   ```bash
   hostname -I
   ```

2. Use that IP in client connections:
   ```bash
   python3 mock_clients/ecg_client.py --endpoint http://192.168.1.50:8080
   ```

3. Ensure firewall allows connections:
   ```bash
   sudo ufw allow 8080/tcp
   sudo ufw allow 8081/tcp
   sudo ufw allow 8090/tcp
   ```

## Data Persistence

All data is stored in the `./data/` directory:
- MRI data: `./data/mri_data/*.h5`
- Robot data: `./data/robot_data/*.json`

**Backup your data** before removing containers:
```bash
# Data persists after stopping services
docker compose down

# Your data is still safe in ./data/
ls -lh data/mri_data/
ls -lh data/robot_data/
```

## Removal

To completely remove the deployment:

```bash
# Stop and remove containers
docker compose down

# Remove images
docker rmi cwru/mri-marshal:latest cwru/robot-marshal:latest

# Remove data (CAUTION: This deletes all data!)
rm -rf data/mri_data/* data/robot_data/*
```

## Support

For issues or questions:
- Check logs: `docker compose logs`
- Review mock client docs: `mock_clients/README.md`
- Project repository: https://github.com/cwru-mercis/cwru_data_marshal

---

**Package created:** $(date)
**Docker Compose version required:** 2.0+
**Docker Engine version required:** 20.10+
EOF

# Summary
echo ""
echo "============================================"
echo "Export Complete!"
echo "============================================"
echo ""
echo "Output directory: $OUT_DIR"
echo ""
echo "Directory structure:"
tree -L 2 "$OUT_DIR" 2>/dev/null || find "$OUT_DIR" -maxdepth 2 -type f -o -type d | sort
echo ""
echo "Total size:"
du -sh "$OUT_DIR"
echo ""
echo "Next steps:"
echo "  1. Copy $OUT_DIR to USB drive or transfer to receiving machine"
echo "  2. On receiving machine, follow instructions in README.md"
echo "  3. Load images with: docker load -i images/*.tar"
echo "  4. Start services with: docker compose up -d"
echo ""
