#!/bin/bash
# Build all CWRU Data Marshal Docker images
# This script builds images from the source branches using worktrees

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "============================================"
echo "  CWRU Data Marshal - Build Docker Images"
echo "============================================"
echo ""

# Check if worktrees exist, create if needed
MRI_WORKTREE="/workspaces/mri_data_marshal_worktree"
ROBOT_WORKTREE="/workspaces/robot_data_marshal_worktree"

if [ ! -d "$MRI_WORKTREE" ]; then
    echo "Creating MRI worktree at $MRI_WORKTREE..."
    git worktree add "$MRI_WORKTREE" mri-data-marhsal
fi

if [ ! -d "$ROBOT_WORKTREE" ]; then
    echo "Creating robot worktree at $ROBOT_WORKTREE..."
    git worktree add "$ROBOT_WORKTREE" robot_data_marshal_with_catheter_system_components
fi

echo "[1/3] Building MRI Marshal and Clients..."
echo "  - cwru/mri-marshal"
echo "  - cwru/ecg-client"
echo "  - cwru/pose-client"
echo "  - cwru/image-streamer"
echo "  - cwru/viz-client"
echo ""

cd "$MRI_WORKTREE"

# Build MRI Marshal
echo "Building cwru/mri-marshal..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.mri" -t cwru/mri-marshal:latest .

# Build ECG Client
echo "Building cwru/ecg-client..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.ecg-client" -t cwru/ecg-client:latest .

# Build Pose Client
echo "Building cwru/pose-client..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.pose-client" -t cwru/pose-client:latest .

# Build Image Streamer (C++ - takes longer)
echo "Building cwru/image-streamer (C++ compilation)..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.image-streamer" -t cwru/image-streamer:latest .

# Build Viz Client (C++ with OpenCV - takes longest)
echo "Building cwru/viz-client (C++ with OpenCV)..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.viz-client" -t cwru/viz-client:latest .

echo ""
echo "[2/3] Building Robot Marshal and Clients..."
echo "  - cwru/robot-marshal"
echo "  - cwru/robot-clients"
echo ""

cd "$ROBOT_WORKTREE"

# Build Robot Marshal
echo "Building cwru/robot-marshal..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.robot" -t cwru/robot-marshal:latest .

# Build Robot Clients
echo "Building cwru/robot-clients..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.robot-clients" -t cwru/robot-clients:latest .

echo ""
echo "[3/3] Verifying images..."
REQUIRED_IMAGES=(
    "cwru/mri-marshal"
    "cwru/robot-marshal"
    "cwru/image-streamer"
    "cwru/ecg-client"
    "cwru/pose-client"
    "cwru/viz-client"
    "cwru/robot-clients"
)

ALL_GOOD=true
for img in "${REQUIRED_IMAGES[@]}"; do
    if docker images --format "{{.Repository}}" | grep -q "^$img$"; then
        SIZE=$(docker images --format "{{.Size}}" "$img:latest" | head -1)
        echo "  ✓ $img:latest ($SIZE)"
    else
        echo "  ✗ $img:latest - NOT FOUND"
        ALL_GOOD=false
    fi
done

echo ""
if [ "$ALL_GOOD" = true ]; then
    echo "============================================"
    echo "  Build complete! All 7 images ready."
    echo "============================================"
    echo ""
    echo "Next steps:"
    echo "  1. Test with: ./scripts/demo-docker.sh"
    echo "  2. Export with: ./scripts/export_usb.sh /path/to/usb"
    echo ""
    exit 0
else
    echo "============================================"
    echo "  ERROR: Some images failed to build"
    echo "============================================"
    exit 1
fi
