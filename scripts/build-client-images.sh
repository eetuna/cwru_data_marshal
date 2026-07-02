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

# Clean up any stale worktree registrations
git worktree prune

# Ensure required branches exist (fetch directly from upstream repo)
MRI_BRANCH="${MRI_BRANCH:-mri-data-marshal}"
ROBOT_BRANCH="${ROBOT_BRANCH:-robot_data_marshal_with_catheter_system_components}"
UPSTREAM_REPO="https://github.com/cwru-mercis/cwru_data_marshal.git"

echo "Checking required branches..."
for BRANCH in "$MRI_BRANCH" "$ROBOT_BRANCH"; do
    if ! git rev-parse --verify "$BRANCH" >/dev/null 2>&1; then
        echo "Branch '$BRANCH' not found locally, fetching from cwru-mercis..."
        if ! git fetch "$UPSTREAM_REPO" "$BRANCH:$BRANCH" 2>/dev/null; then
            echo ""
            echo "  ✗ ERROR: Failed to fetch branch '$BRANCH'"
            echo ""
            echo "  This repository is private. You need:"
            echo "    1. Access to https://github.com/cwru-mercis/cwru_data_marshal"
            echo "    2. GitHub authentication configured (SSH key or personal access token)"
            echo ""
            echo "  To set up SSH authentication:"
            echo "    https://docs.github.com/en/authentication/connecting-to-github-with-ssh"
            echo ""
            exit 1
        fi
    fi
done

# Check if worktrees exist, create if needed
MRI_WORKTREE="$PROJECT_ROOT/.worktrees/mri_data_marshal"
ROBOT_WORKTREE="$PROJECT_ROOT/.worktrees/robot_data_marshal"

# Validate existing worktrees before reuse. An orphaned directory (gitdir
# record pruned but files left behind) or a worktree checked out to a
# different branch would otherwise be used silently and produce stale
# builds. Abort with a clear instruction instead.
check_worktree() {
    local wt="$1"
    local want_branch="$2"
    if [ ! -d "$wt" ]; then
        return 0
    fi
    local got_branch
    if ! got_branch="$(git -C "$wt" symbolic-ref --short HEAD 2>/dev/null)"; then
        echo ""
        echo "  ✗ ERROR: $wt exists but is not a valid git worktree (orphaned)."
        echo "           Delete it and rerun: rm -rf $wt"
        exit 1
    fi
    if [ "$got_branch" != "$want_branch" ]; then
        echo ""
        echo "  ✗ ERROR: $wt is on branch '$got_branch' but this script expects '$want_branch'."
        echo "           Either delete it and rerun (rm -rf $wt) or check out the expected"
        echo "           branch manually inside that worktree."
        exit 1
    fi
}

check_worktree "$MRI_WORKTREE" "$MRI_BRANCH"
check_worktree "$ROBOT_WORKTREE" "$ROBOT_BRANCH"

if [ ! -d "$MRI_WORKTREE" ]; then
    echo "Creating MRI worktree at $MRI_WORKTREE..."
    git worktree add "$MRI_WORKTREE" "$MRI_BRANCH"
fi

if [ ! -d "$ROBOT_WORKTREE" ]; then
    echo "Creating robot worktree at $ROBOT_WORKTREE..."
    git worktree add "$ROBOT_WORKTREE" "$ROBOT_BRANCH"
fi

echo "[1/3] Building MRI marshal + recon..."
echo "  - cwru/mri-marshal"
echo "  - fire-python (python-ismrmrd-server recon)"
echo ""

cd "$MRI_WORKTREE"

# Build MRI Marshal
echo "Building cwru/mri-marshal..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.mri" -t cwru/mri-marshal:latest .

# Build recon = python-ismrmrd-server. Build context is the server root (its
# Dockerfile COPYs the whole source tree); -f points at docker/Dockerfile inside it.
echo "Building fire-python (python-ismrmrd-server recon)..."
docker build -f "$MRI_WORKTREE/third_party/python-ismrmrd-server/docker/Dockerfile" \
  -t fire-python:latest "$MRI_WORKTREE/third_party/python-ismrmrd-server"

echo ""
echo "[2/3] Building Robot Marshal and Clients..."
echo "  - cwru/robot-marshal"
echo "  - cwru/robot-clients"
echo "  - cwru/webgl-client"
echo ""

cd "$ROBOT_WORKTREE"

# Build Robot Marshal
echo "Building cwru/robot-marshal..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.robot" -t cwru/robot-marshal:latest .

# Build Robot Clients
echo "Building cwru/robot-clients..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.robot-clients" -t cwru/robot-clients:latest .

# Build WebGL Client (front-end UI)
echo "Building cwru/webgl-client..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.webgl-client" -t cwru/webgl-client:latest .

echo ""
echo "[3/3] Verifying images..."
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
    "cwru/webgl-client"
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
    echo "  Build complete! All 10 images ready."
    echo "============================================"
    echo ""
    echo "Next steps:"
    echo "  1. Run with:    docker compose up -d   (MARSHAL_DUMP=--dump for dump mode)"
    echo "  2. Export with: ./scripts/export_usb.sh /path/to/usb"
    echo ""
    exit 0
else
    echo "============================================"
    echo "  ERROR: Some images failed to build"
    echo "============================================"
    exit 1
fi
