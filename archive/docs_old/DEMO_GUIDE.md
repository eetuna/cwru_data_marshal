# MRI Data Marshal Demo Guide

**Quick guide to running and demonstrating the MRI Data Marshal system**

---

## Prerequisites

### Required Software
- **Build tools:** CMake 3.16+, Ninja or Make, C++17 compiler (GCC 9+ or Clang 10+)
- **Libraries:** HDF5, OpenCV, cpp-httplib, nlohmann-json
- **Display:** X11 server (native Linux, WSLg, or VcXsrv on Windows)

### Verify Build
```bash
# Check if binaries exist
ls -la build/marshal build/image_streamer build/viz_client

# If not built, build everything
make
```

---

## Quick Start (5 Minutes)

### Option 1: Full Automated Demo

```bash
# Run the complete demo (MRI + Robot + Visualizer + Streamer)
./scripts/run_demo_simultaneous.sh
```

**What happens:**
1. MRI Marshal starts (HTTP 8080, WebSocket 8090)
2. Robot Marshal starts (HTTP 8081)
3. Visualizer window opens (OpenCV display)
4. Image streamer generates 300 frames at 50ms intervals
5. Robot clients perform read/write operations
6. Demo runs for ~15 seconds, then prompts for cleanup

### Option 2: Manual Step-by-Step

```bash
# Terminal 1: Start MRI Marshal
./build/marshal --http 127.0.0.1:8080 --ws 127.0.0.1:8090 --data ./data_mri --flush-frames 1

# Terminal 2: Start Visualizer (wait for marshal to start)
./build/viz_client --http http://127.0.0.1:8080/v1/mrd/latest

# Terminal 3: Generate frames
./build/image_streamer --http http://127.0.0.1:8080 --frames 300 --dt-ms 50 --size 192 --nslices 10
```

---

## Demo Walkthrough

### Step 1: MRI Marshal Initialization

When the demo starts, you'll see:

```
=== Starting MRI Data Marshal ===
HTTP server listening on 127.0.0.1:8080
WebSocket server listening on 127.0.0.1:8090
SWMR mode enabled
Ready for frames...
```

**What this means:**
- HTTP API is ready at port 8080
- WebSocket streaming is ready at port 8090
- HDF5 SWMR storage is initialized

### Step 2: Robot Marshal Initialization

```
=== Starting Robot Data Marshal ===
HTTP server listening on 127.0.0.1:8081
State blackboard initialized
```

**What this means:**
- Key-value state storage is available
- Robot clients can read/write state data

### Step 3: Visualizer Launch

An OpenCV window opens showing:
- Current slice of the MRI frame
- Slice number indicator
- Frame counter

**Controls:**
| Key | Action |
|-----|--------|
| UP Arrow | Previous slice |
| DOWN Arrow | Next slice |
| ESC | Exit visualizer |

### Step 4: Frame Streaming

The image streamer outputs:

```
Streaming 300 frames @ 50ms interval (20 fps target)
Frame size: 192x192x10 = 1.47 MB
[====================>          ] 150/300 frames, 7.5s elapsed
```

**What's happening:**
- Synthetic MRI frames are generated
- Each frame is POSTed to `/v1/mrd/frame`
- SWMR allows visualizer to read while writing

### Step 5: Robot Data Flow

```
Robot client 1: Writing ECG data...
Robot client 2: Writing pose data...
Robot reader: Retrieved 2000 entries
```

**What's happening:**
- ECG and pose data are written to state files
- Readers retrieve historical data
- Demonstrates multi-client coordination

### Step 6: Demo Completion

```
=== Demo Complete ===
Frames streamed: 300
Time elapsed: 15.2s
Effective FPS: 19.7

Press ENTER to cleanup and exit...
```

---

## Expected Results

### Successful Demo Output

| Metric | Expected Value | Notes |
|--------|---------------|-------|
| Frames displayed | ~300 | Should match streamer output |
| Effective FPS | 18-20 | HDF5 SWMR sustainable rate |
| Robot operations | 2000+ | Read/write cycles |
| Visualizer response | Real-time | Slight lag acceptable |

### Visual Verification

The visualizer should show:
- Grayscale MRI slice images
- Smooth transitions between frames
- Slice navigation working with arrow keys

### Terminal Verification

```bash
# Check HDF5 files were created
ls -la data_mri/mrd/

# Verify frame count in latest file
h5dump -H data_mri/mrd/latest.h5 | grep DATASPACE
```

---

## Demo Variations

### Minimal Demo (MRI Only)

```bash
# Start marshal
./build/marshal --http 127.0.0.1:8080 --data ./data_mri &

# Stream 50 frames
./build/image_streamer --http http://127.0.0.1:8080 --frames 50 --dt-ms 100
```

### High-Speed Demo (Stress Test)

```bash
# Run the stress test benchmark
./scripts/benchmarks/mri_marshal_stress_test.sh

# Or run aggressive SWMR test
./scripts/benchmarks/swmr_continuous_bench.sh
```

### Visualizer Only (With Existing Data)

```bash
# If you have existing HDF5 data
./build/viz_client --http http://127.0.0.1:8080/v1/mrd/latest --data ./existing_data
```

---

## Troubleshooting

### X11 Display Issues

**Symptom:** Visualizer fails to open window

```
error: cannot open display
```

**Solutions:**

1. **WSLg (Windows 11):** Should work automatically
   ```bash
   echo $DISPLAY  # Should show :0 or similar
   ```

2. **VcXsrv (Windows 10):**
   ```bash
   export DISPLAY=$(cat /etc/resolv.conf | grep nameserver | awk '{print $2}'):0.0
   ```

3. **Headless mode:** Skip visualizer
   ```bash
   ./build/image_streamer --http http://127.0.0.1:8080 --frames 100 --dt-ms 50
   # Monitor via HTTP instead
   curl http://127.0.0.1:8080/v1/mrd/latest
   ```

### Port Already in Use

**Symptom:** Marshal fails to start

```
error: bind failed, port 8080 already in use
```

**Solution:**
```bash
# Find and kill existing process
lsof -i :8080
kill <PID>

# Or use different port
./build/marshal --http 127.0.0.1:9080 --ws 127.0.0.1:9090
```

### Slow Performance

**Symptom:** FPS much lower than 19

**Checks:**
```bash
# Check CPU usage
htop

# Check disk I/O
iotop

# Check if running on SSD
df -h ./data_mri
```

**Common causes:**
- Running on HDD instead of SSD
- Other processes consuming I/O
- WSL2 overhead (5-10ms per operation is normal)

### Marshal Crashes

**Symptom:** Segfault or immediate exit

**Checks:**
```bash
# Verify data directory exists
mkdir -p ./data_mri/mrd

# Check permissions
ls -la ./data_mri

# Run with debug output
./build/marshal --http 127.0.0.1:8080 --data ./data_mri 2>&1 | tee marshal.log
```

---

## Cleanup

### After Demo

```bash
# Stop all processes
pkill marshal
pkill viz_client
pkill image_streamer

# Clean up data files
rm -rf ./data_mri/mrd/*

# Reset robot state
rm -rf ./data_robot/*
```

### Fresh Start

```bash
# Full cleanup and rebuild
make clean
make
rm -rf ./data_mri ./data_robot
mkdir -p ./data_mri/mrd ./data_robot
```

---

## Demo Scripts Reference

| Script | Purpose | Duration |
|--------|---------|----------|
| `run_demo_simultaneous.sh` | Full demo with all components | ~15s |
| `run_demo_manual.sh` | Step-by-step with pauses | Manual |
| `run_demo_automated.sh` | Non-interactive full run | ~20s |
| `benchmarks/mri_marshal_stress_test.sh` | Performance testing | ~5 min |
| `benchmarks/swmr_continuous_bench.sh` | Aggressive SWMR test | ~4 min |

---

## Presentation Tips

### For Live Demos

1. **Pre-warm the system:** Run the demo once before presenting
2. **Use smaller frames:** `--size 128` for smoother display
3. **Reduce frame count:** `--frames 100` for shorter demo
4. **Have backup screenshots:** In case X11 fails

### Key Points to Highlight

1. **Simultaneous access:** "Notice the visualizer updating while frames are still streaming"
2. **Real-time performance:** "We're achieving ~19 fps, which is clinical MRI rate"
3. **Multi-client:** "Robot data is being written while MRI is streaming"
4. **HDF5 format:** "All data is stored in standard HDF5, compatible with Python/MATLAB"

### Questions to Anticipate

- **"Why not faster?"** - HDF5 metadata sync overhead (see [IMPROVEMENTS_AND_OPTIMIZATION.md](IMPROVEMENTS_AND_OPTIMIZATION.md))
- **"Can it handle 4K MRI?"** - Yes, frame size doesn't significantly affect FPS
- **"What about network streaming?"** - WebSocket endpoint available at port 8090

---

*For detailed API reference, see [USAGE_AND_API.md](USAGE_AND_API.md)*
*For performance optimization, see [IMPROVEMENTS_AND_OPTIMIZATION.md](IMPROVEMENTS_AND_OPTIMIZATION.md)*
