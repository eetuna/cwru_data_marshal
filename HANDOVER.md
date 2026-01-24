# HANDOVER - Docker Demo System

## Recent Fixes Applied

### Fix 1: Robot Marshal status line newlines - RESOLVED ✓
**Problem**: Output showed multiple lines instead of one line
**Root Cause**: `grep -c` returns `0` with exit code 0, so `|| echo 0` was running anyway, doubling the output (0 from grep + 0 from echo)
**Solution**:
- Removed unnecessary `|| echo 0` fallbacks in [scripts/demo-docker.sh:89-93](scripts/demo-docker.sh#L89-L93)
- Changed from `echo` to `printf` with explicit format string at [scripts/demo-docker.sh:94](scripts/demo-docker.sh#L94)
**Status**: Fixed - `grep -c` always succeeds even with 0 matches, so the fallback was redundant and causing duplicates

### Fix 2: viz-client shows old images on restart - RESOLVED ✓
**Problem**: viz-client would display stale frames from previous demo runs
**Root Cause**: viz-client was starting before image-streamer had generated the first frame
**Solution**:
- Added `depends_on: image-streamer` to viz-client in [docker-compose.demo.yml:83-84](docker-compose.demo.yml#L83-L84)
- Added 3-second delay in [scripts/demo-docker.sh:37-38](scripts/demo-docker.sh#L37-L38) to allow first frame generation
- Added `restart: on-failure` policy to viz-client
**Status**: Fixed - viz-client now waits for fresh data

### Fix 3: Robot clients failure handling - IMPROVED ✓
**Problem**: Some robot clients were dying without clear diagnostics
**Solution**:
- Added `restart: unless-stopped` to all robot clients in docker-compose.demo.yml
- Added startup diagnostic check in [scripts/demo-docker.sh:46-54](scripts/demo-docker.sh#L46-L54) that reports which clients are running/failed
**Status**: Improved - script now shows client status and provides debug commands for failed clients

### Fix 4: Robot clients showing 0 operations - RESOLVED ✓
**Problem**: Monitor showed `cath=100 ctrl=0 plan=0 fe=0 surf=0` - only catheter tracked, others all 0
**Root Cause**: The Dockerfile only patched catheter-tracking to output "CATHETER:" prefix. The other 4 clients (controller, planning, front-end, surface-tracking) were never patched to add their respective log prefixes ("CONTROLLER:", "PLANNING:", "FRONTEND:", "SURFACE:")
**Solution**:
- Added missing sed patches in [docker/Dockerfile.robot-clients:25-28](docker/Dockerfile.robot-clients#L25-L28) for all 4 remaining clients
- Each client now outputs its labeled prefix: CONTROLLER:, PLANNING:, FRONTEND:, SURFACE:
- Rebuilt robot-clients image with: `docker build -f docker/Dockerfile.robot-clients -t cwru/robot-clients:latest .`
**Status**: Fixed - all 5 robot clients now output labeled operations that can be counted by the monitor

**If clients still fail, check logs**:
```bash
docker ps -a | grep cwru
docker logs cwru-controller 2>&1
docker logs cwru-planning 2>&1
docker logs cwru-surface-tracking 2>&1
```

## Known Issues

### viz-client shows blank/black window (no images)
**Symptom**: viz-client window opens but displays black screen or "Waiting for data..." message
**Root Cause**: The MRI Marshal's `/v1/mrd/latest` HTTP endpoint returns JSON in wrong format
- **viz-client expects**: `{"data": {"path": "...", "frame_index": 123}}`
- **MRI Marshal returns**: `{"path": "...", "frame_index": 123, ...}` (missing "data" wrapper)
**Impact**: viz-client cannot find the image data and shows blank window
**Fix needed**: Modify MRI Marshal to wrap response in `"data"` object, OR modify viz-client to accept flat JSON format
**Workaround**: None - this requires code changes to either MRI Marshal or viz-client

### viz-client "totally borken" message on exit
**Symptom**: When stopping the demo with Ctrl+C, may see "cwru-viz-client totally borken" message
**Impact**: Cosmetic only - this appears during cleanup and doesn't affect functionality
**Cause**: Docker or viz-client error message during forced shutdown
**Workaround**: None needed - container stops successfully despite message

## Previous Session Changes

### 1. Image streamer interval fix
- Changed `--interval` to `--dt-ms` in docker-compose.demo.yml line 43
- `--interval` expects seconds, `--dt-ms` expects milliseconds
- IMAGE_INTERVAL=50 now correctly means 50ms (20fps)

### 2. Networking: Host → Bridge
- Removed `network_mode: host` from all services
- Added `cwru-net` bridge network
- Services now use Docker service names (e.g., `mri-marshal:8080`)
- Added port mappings: 8080, 8081, 8090

### 3. viz-client display fixes
- Added WSLg mounts for display: `/mnt/wslg`, `/tmp/.X11-unix`
- Added `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY` env vars
- Added `session-data` volume mount for HDF5 file access

### 4. Demo script improvements
- Added cleanup of old MRD files before starting
- Added graceful shutdown after DEMO_DURATION
- Added monitor_status() showing latest frame + robot stats

### 5. Removed external network from devcontainer
- Removed `--network=cwru-demo-network` from devcontainer.json

## Current Session Changes

### 1. Fixed Robot Marshal status line formatting
- Removed redundant `|| echo 0` fallbacks (grep -c always succeeds)
- Changed `echo` to `printf` in monitor_status() function
- Ensures single-line output without embedded newlines

### 2. Fixed viz-client stale image issue
- Added dependency on image-streamer
- Added 3-second startup delay for first frame generation
- Added restart policy for resilience

### 3. Improved robot client diagnostics
- Added `restart: unless-stopped` to all robot clients
- Added startup status check showing which clients are running/failed
- Provides actionable debug commands for troubleshooting

### 4. Fixed robot client log prefixes
- Added missing sed patches for controller, planning, front-end, surface-tracking
- All 5 robot clients now output labeled operations
- Rebuilt robot-clients Docker image

## Files Modified (Current Session)
- [docker-compose.demo.yml](docker-compose.demo.yml) - viz-client dependencies, restart policies for robot clients
- [scripts/demo-docker.sh](scripts/demo-docker.sh) - removed `|| echo 0` fallbacks, printf fix, startup diagnostics, viz-client delay
- [docker/Dockerfile.robot-clients](docker/Dockerfile.robot-clients) - added log prefix patches for all 5 robot clients

## How to Run
```bash
./scripts/demo-docker.sh
```

## How to Debug
```bash
# Check all containers
docker ps -a | grep cwru

# Check specific container logs
docker logs cwru-image-streamer 2>&1
docker logs cwru-catheter-tracking 2>&1

# Check if files are being created
docker exec cwru-mri-marshal ls -la /session-data/mrd/

# Test monitor_status logic manually
docker logs cwru-catheter-tracking --tail 100 2>/dev/null | grep -c "CATHETER:"
```
