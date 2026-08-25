#!/bin/bash
# Build all CWRU Data Marshal Docker images.
#
# Each image is built from a CLEAN EXPORT (git archive) of a code branch at a
# specific commit — never from a working folder. That makes every image
# reproducible ("Building from: <branch> @ <commit>" is printed and is the
# truth) and removes the failure modes of the old worktree-based flow:
# stale checkouts silently rebuilt, `git pull` on main not moving the code
# branches, and one clone used from two paths (WSL host + devcontainer)
# wrecking each other's worktree registrations.
#
# Which commit is built, per branch: the cwru-mercis tip if reachable and
# not behind the local branch; otherwise the local branch (with a warning).
#
# Local iteration (build uncommitted code on purpose):
#   MRI_BUILD_DIR=/path/to/marshal/checkout ./scripts/build-client-images.sh
#   ROBOT_BUILD_DIR=/path/to/robot/checkout ./scripts/build-client-images.sh
# The script then builds that folder as-is and says so loudly.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

echo "============================================"
echo "  CWRU Data Marshal - Build Docker Images"
echo "============================================"
echo ""

MRI_BRANCH="${MRI_BRANCH:-mri-data-marshal}"
ROBOT_BRANCH="${ROBOT_BRANCH:-robot_data_marshal_with_catheter_system_components}"
UPSTREAM_REPO="${UPSTREAM_REPO:-https://github.com/cwru-mercis/cwru_data_marshal.git}"

# Never hang on an interactive credential prompt; an unreachable/unauthorised
# remote just means "build the local branch".
export GIT_TERMINAL_PROMPT=0

# Print the commit to build for a branch (stdout); diagnostics go to stderr.
resolve_commit() {
    local branch="$1"
    local remote_sha="" local_sha=""
    if git fetch --quiet "$UPSTREAM_REPO" "$branch" 2>/dev/null; then
        remote_sha="$(git rev-parse FETCH_HEAD)"
    fi
    if git rev-parse --verify --quiet "refs/heads/$branch" >/dev/null 2>&1; then
        local_sha="$(git rev-parse "refs/heads/$branch")"
    fi
    if [ -z "$remote_sha" ] && [ -z "$local_sha" ]; then
        echo "" >&2
        echo "  ✗ ERROR: branch '$branch' is neither reachable at $UPSTREAM_REPO nor present locally." >&2
        echo "           This repository is private: you need access to cwru-mercis and GitHub" >&2
        echo "           authentication configured (SSH key or personal access token)." >&2
        exit 1
    fi
    if [ -z "$remote_sha" ]; then
        echo "  ! cwru-mercis unreachable; building local '$branch'." >&2
        echo "$local_sha"; return 0
    fi
    if [ -z "$local_sha" ] || git merge-base --is-ancestor "$local_sha" "$remote_sha" 2>/dev/null; then
        echo "$remote_sha"; return 0
    fi
    if git merge-base --is-ancestor "$remote_sha" "$local_sha" 2>/dev/null; then
        echo "  ! Local '$branch' is AHEAD of cwru-mercis (unpushed commits); building local." >&2
    else
        echo "  ! Local '$branch' and cwru-mercis have DIVERGED; building local. Push/merge to reconcile." >&2
    fi
    echo "$local_sha"
}

# Export a commit into a fresh directory (clean tree: no build artefacts, no
# uncommitted edits, no stray files).
export_commit() {
    local sha="$1" dest="$2"
    rm -rf "$dest"
    mkdir -p "$dest"
    git archive --format=tar "$sha" | tar -x -C "$dest"
}

BUILD_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cwru-build.XXXXXX")"
trap 'rm -rf "$BUILD_ROOT"' EXIT

echo "Resolving source commits..."
if [ -n "${MRI_BUILD_DIR:-}" ]; then
    echo "  ! MRI_BUILD_DIR set: building the marshal from WORKING FOLDER $MRI_BUILD_DIR (uncommitted changes included)."
    MRI_DESC="working folder $MRI_BUILD_DIR"
else
    MRI_SHA="$(resolve_commit "$MRI_BRANCH")"
    MRI_BUILD_DIR="$BUILD_ROOT/mri"
    export_commit "$MRI_SHA" "$MRI_BUILD_DIR"
    MRI_DESC="$(git log --oneline -1 "$MRI_SHA")"
fi
if [ -n "${ROBOT_BUILD_DIR:-}" ]; then
    echo "  ! ROBOT_BUILD_DIR set: building robot/webgl from WORKING FOLDER $ROBOT_BUILD_DIR (uncommitted changes included)."
    ROBOT_DESC="working folder $ROBOT_BUILD_DIR"
else
    ROBOT_SHA="$(resolve_commit "$ROBOT_BRANCH")"
    ROBOT_BUILD_DIR="$BUILD_ROOT/robot"
    export_commit "$ROBOT_SHA" "$ROBOT_BUILD_DIR"
    ROBOT_DESC="$(git log --oneline -1 "$ROBOT_SHA")"
fi

echo ""
echo "Building from:"
echo "  $MRI_BRANCH @ $MRI_DESC"
echo "  $ROBOT_BRANCH @ $ROBOT_DESC"
echo ""

echo "[1/3] Building MRI marshal + recon..."
echo "  - cwru/mri-marshal"
echo "  - fire-python (python-ismrmrd-server recon)"
echo ""

# Build MRI Marshal
echo "Building cwru/mri-marshal..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.mri" -t cwru/mri-marshal:latest "$MRI_BUILD_DIR"

# Build recon = python-ismrmrd-server. Build context is the server root (its
# Dockerfile COPYs the whole source tree); -f points at docker/Dockerfile inside it.
echo "Building fire-python (python-ismrmrd-server recon)..."
docker build -f "$MRI_BUILD_DIR/third_party/python-ismrmrd-server/docker/Dockerfile" \
  -t fire-python:latest "$MRI_BUILD_DIR/third_party/python-ismrmrd-server"

echo ""
echo "[2/3] Building Robot Marshal and Clients..."
echo "  - cwru/robot-marshal"
echo "  - cwru/robot-clients"
echo "  - cwru/webgl-client"
echo ""

# Build Robot Marshal
echo "Building cwru/robot-marshal..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.robot" -t cwru/robot-marshal:latest "$ROBOT_BUILD_DIR"

# Build Robot Clients
echo "Building cwru/robot-clients..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.robot-clients" -t cwru/robot-clients:latest "$ROBOT_BUILD_DIR"

# Build WebGL Client (front-end UI)
echo "Building cwru/webgl-client..."
docker build -f "$PROJECT_ROOT/docker/Dockerfile.webgl-client" -t cwru/webgl-client:latest "$ROBOT_BUILD_DIR"

echo ""
echo "[3/3] Verifying images..."
REQUIRED_IMAGES=(
    "cwru/mri-marshal"
    "fire-python"
    "cwru/robot-marshal"
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
    echo "  Build complete! All 5 images ready."
    echo "  Built from:"
    echo "    $MRI_BRANCH @ $MRI_DESC"
    echo "    $ROBOT_BRANCH @ $ROBOT_DESC"
    echo "  Next: docker compose up -d --force-recreate"
    echo "============================================"
else
    echo "============================================"
    echo "  Build finished with missing images — see above."
    echo "============================================"
    exit 1
fi
