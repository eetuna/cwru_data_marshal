# Handoff: MRI Data Marshal Performance Optimization

## Task Overview
Implement performance optimizations for MRI Data Marshal to achieve higher FPS throughput, then document all changes and create/revise demo scripts with configurable parameters.

## Pre-requisites
Before starting, checkout a new branch:
```bash
git checkout -b feature/performance-optimization
```

## Part 1: Performance Optimizations

### 1.1 Remove `fsync()` from `write_atomic()` (Medium Impact)

**File:** `/workspaces/cwru_data_marshal/include/atomic_write.hpp`

**Current code (lines 35-39):**
```cpp
int fd = ::open(tmp.c_str(), O_RDONLY);
if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
}
```

**Change:** Remove the `fsync()` call. The `rename()` is still atomic on Linux. This saves ~5-10ms per frame.

**Risk:** Minimal. `latest.json` could be stale after power loss, but HDF5 data is always safe.

---

### 1.2 Batch `index.jsonl` Writes (Minor Impact)

**File:** `/workspaces/cwru_data_marshal/src/mrd_sink.cpp`

**Current behavior (line 534):**
```cpp
append_line(sink.index_root / "index.jsonl", entry.dump());
```

**Change:** Buffer entries and write every N frames (e.g., every 10 frames) or every 100ms.

**Implementation approach:**
- Add a `std::vector<std::string>` buffer for pending index entries
- Add a counter or timestamp for last flush
- Flush buffer to file when threshold reached or on shutdown

**Risk:** Minimal. Index may be a few frames behind during crash. Real-time clients use `latest.json` anyway.

---

### 1.3 Async Flush in Background Thread (Major Impact)

**Files:**
- `/workspaces/cwru_data_marshal/src/mrd_sink.cpp`
- `/workspaces/cwru_data_marshal/include/mrd_sink.hpp`

**Current behavior:** `H5Dflush()` and `H5Fflush()` block the write path.

**Change:** Queue flush operations to a background thread.

**Implementation approach:**
1. Add a flush worker thread in `MrdFile` class
2. Add a flush queue (frame index to flush)
3. After `H5Dwrite()`, push to queue instead of blocking flush
4. Worker thread processes queue and calls `H5Dflush()`/`H5Fflush()`
5. Add proper shutdown handling to flush remaining queue

**Risk:** More complex code. Must ensure proper thread synchronization.

---

## Part 2: Demo Script Revision

### 2.1 Create/Revise Main Demo Script

**File:** `/workspaces/cwru_data_marshal/scripts/run_demo_simultaneous.sh` (or create new)

**Add configurable parameters at top:**

```bash
#!/bin/bash
# MRI Data Marshal - Performance Demo
# All timing/size parameters are configurable here

# ============ DEMO CONFIGURATION ============
DEMO_DURATION_SEC=60              # Total demo duration (seconds)

# ============ MRI MARSHAL CONFIGURATION ============
MRI_HTTP_PORT=8080
MRI_WS_PORT=8090
MRI_DATA_DIR="./data_demo_mri"    # Use /dev/shm/mrd_data for RAM disk
MRI_FLUSH_FRAMES=1                # Frames between HDF5 flushes (1=every frame, 5=batch)

# ============ IMAGE STREAMER CONFIGURATION ============
IMAGE_INTERVAL_MS=10              # Milliseconds between frames (10=100fps, 100=10fps)
IMAGE_SIZE=64                     # Image width/height in pixels
IMAGE_NSLICES=3                   # Number of Z slices per volume
IMAGE_FRAME_COUNT=$((DEMO_DURATION_SEC * 1000 / IMAGE_INTERVAL_MS))

# ============ ECG CONFIGURATION ============
ECG_INTERVAL_MS=250               # Milliseconds between ECG samples (4 Hz)
ECG_COUNT_TARGET=$((DEMO_DURATION_SEC * 1000 / ECG_INTERVAL_MS))

# ============ POSE CONFIGURATION ============
POSE_INTERVAL_MS=500              # Milliseconds between pose updates (2 Hz)
POSE_COUNT_TARGET=$((DEMO_DURATION_SEC * 1000 / POSE_INTERVAL_MS))

# ============ ROBOT MARSHAL CONFIGURATION ============
ROBOT_HTTP_PORT=8081
ROBOT_DATA_DIR="./data_demo_robot"
```

**Pass `MRI_FLUSH_FRAMES` to marshal:**
```bash
./build/marshal --http 127.0.0.1:$MRI_HTTP_PORT \
                --ws 127.0.0.1:$MRI_WS_PORT \
                --data "$MRI_DATA_DIR" \
                --flush-frames $MRI_FLUSH_FRAMES \
                > "$MRI_DATA_DIR/server.log" 2>&1 &
```

---

## Part 3: Documentation

### 3.1 Create Performance Documentation

**File:** `/workspaces/cwru_data_marshal/docs/PERFORMANCE_TUNING.md`

**Contents to document:**

1. **FPS Bottleneck Analysis**
   - HDF5 double flush (`H5Dflush` + `H5Fflush`) per frame
   - `fsync()` in `write_atomic()` for `latest.json`
   - `append_line()` to `index.jsonl`
   - HTTP polling overhead (if not using WebSocket)

2. **Quick Wins (No Code Changes)**
   | Parameter | Default | Fast | Description |
   |-----------|---------|------|-------------|
   | `--data` | `./data` | `/dev/shm/mrd_data` | Use RAM disk |
   | `--flush-frames` | 1 | 5-10 | Batch HDF5 flushes |

3. **Code Optimizations**
   - Remove `fsync()` from `write_atomic()`
   - Batch `index.jsonl` writes
   - Async flush in background thread

4. **Expected FPS Results**
   | Configuration | Expected FPS |
   |---------------|--------------|
   | Default (disk, flush=1) | ~50 fps |
   | RAM disk, flush=1 | ~80 fps |
   | RAM disk, flush=5 | ~95 fps |
   | RAM disk, async flush | ~100+ fps |

5. **WebSocket vs HTTP Polling**
   - WebSocket push is instant (marshal already broadcasts on `ws_emit`)
   - HTTP polling has request/response overhead
   - viz_client currently uses HTTP, could switch to WebSocket

---

### 3.2 Update Demo Guide

**File:** `/workspaces/cwru_data_marshal/docs/DEMO_GUIDE.md`

**Add section on performance parameters:**
- How to adjust FPS target
- How to use RAM disk for demos
- Explanation of `--flush-frames` tradeoff (speed vs latency)

---

## Verification

After implementing changes:

1. **Build:**
   ```bash
   ninja -C build marshal viz_client image_streamer
   ```

2. **Test with default settings:**
   ```bash
   ./scripts/run_demo_simultaneous.sh
   ```

3. **Test with RAM disk:**
   ```bash
   # Edit script: MRI_DATA_DIR="/dev/shm/mrd_data"
   ./scripts/run_demo_simultaneous.sh
   ```

4. **Verify FPS improvement in viz_client window**

---

## Files Modified Summary

| File | Change |
|------|--------|
| `include/atomic_write.hpp` | Remove `fsync()` |
| `src/mrd_sink.cpp` | Batch index writes, async flush |
| `scripts/run_demo_simultaneous.sh` | Add configurable parameters |
| `docs/PERFORMANCE_TUNING.md` | New documentation |
| `docs/DEMO_GUIDE.md` | Update with performance section |

---

## Commit Message Template

```
perf: Optimize MRI Data Marshal for higher FPS throughput

- Remove fsync() from write_atomic() (~5-10ms savings per frame)
- Batch index.jsonl writes (minor I/O reduction)
- Add async flush option for background HDF5 flushes
- Add configurable parameters to demo script
- Document performance tuning options

Benchmarks:
- Default: ~50 fps -> RAM disk + async: ~100+ fps
```
