# Handoff Document: MRI Data Marshal Documentation Tasks

**Status:** Feature complete, demo working, ready for documentation
**Branch:** feature/viz-single-slice-navigator
**Last Commit:** 452f28a "Rewrite viz_client: single-slice navigator with keyboard navigation"

---

## Tasks for Next Claude Agent

Generate **4 comprehensive documents** for the MRI Data Marshal project:

### 1. PROFESSIONAL PRESENTATION (Faculty/Academic)
**Output file:** `docs/MRI_DATA_MARSHAL_PRESENTATION.md`

**Content to include:**
- Executive summary (1 page)
- What is MRI Data Marshal? (architecture overview)
- Key features:
  - Real-time data ingestion (HTTP/WebSocket APIs)
  - SWMR HDF5 storage
  - Multi-client simultaneous access
  - State blackboard for robot integration
- Performance characteristics (use benchmark data below):
  - SWMR write: 19 fps @ 50ms, 40 MB/s throughput
  - Read: 124 RPS, 8ms latency
  - Full ingest: 1 fps, 2 MB/s
- Use cases and applications
- Known limitations (HDF5 SWMR not suitable for <50ms intervals)
- Comparison with alternatives (Zarr, binary, etc.)
- Future improvements

**Benchmark data to reference:**
```
SWMR @ 50ms intervals:  19 fps,  40 MB/s,  Real-time
SWMR @ 10ms intervals:  0.4 fps, 0.84 MB/s, Not viable
Full ingest:            1 fps,   2 MB/s
GET /v1/mrd/latest:     124 RPS, 8ms latency
Frame size:             192x192x15 = 2.2 MB
```

**Audience:** Faculty, researchers, non-engineers
**Tone:** Educational, honest about limitations

---

### 2. DEMO GUIDE (How to Run the System)
**Output file:** `docs/DEMO_GUIDE.md`

**Content to include:**
- Prerequisites and setup
- Quick start (5 minutes):
  - Build command: `make` or `cmake ... && ninja`
  - Run demo: `./scripts/run_demo_simultaneous.sh`
  - Expected output and timing
- What happens in each step:
  1. MRI Marshal starts (HTTP 8080, WebSocket 8090)
  2. Robot Marshal starts (HTTP 8081)
  3. Visualizer launches (OpenCV window showing slices)
  4. Image streamer generates frames (50ms intervals, 192x192x10)
  5. Robot clients run circular data flow
  6. ECG/Pose data being ingested simultaneously
- Controls:
  - Visualizer: UP/DOWN arrows for slice navigation, ESC to exit
  - Press ENTER to cleanup
- Expected results:
  - Visualizer shows ~300 frames over 15 seconds
  - Robot clients show 2000+ operations
  - Terminal shows ECG/Pose/Robot activity
- Troubleshooting:
  - If X11 fails: try WSLg or VcXsrv setup
  - If marshal crashes: check port availability (8080, 8081)
  - If slow: check CPU/disk, normal on WSL2

---

### 3. USAGE INSTRUCTIONS & API REFERENCE
**Output file:** `docs/USAGE_AND_API.md`

**Content to include:**

#### Entry Points
```bash
# MRI Marshal (data ingestion + HDF5 SWMR storage)
./build/marshal --http 127.0.0.1:8080 \
                --ws 127.0.0.1:8090 \
                --data ./data_mri \
                --flush-frames 1

# Image Streamer (generates synthetic frames)
./build/image_streamer --http http://127.0.0.1:8080 \
                       --frames 300 \
                       --dt-ms 50 \
                       --size 192 \
                       --nslices 10

# Visualizer (real-time display)
./build/viz_client --http http://127.0.0.1:8080/v1/mrd/latest

# Robot Marshal (state blackboard)
./build/robot_marshal_demo 8081
```

#### Configuration Parameters

| Parameter | Marshal Type | Description | Default | Example |
|-----------|---|---|---|---|
| `--http` | Both | HTTP server address | 127.0.0.1:8080 | `127.0.0.1:9000` |
| `--ws` | MRI | WebSocket port | 8090 | `127.0.0.1:8091` |
| `--data` | MRI | HDF5 storage directory | ./data | `./mri_data` |
| `--flush-frames` | MRI | Flush after N frames | 1 | `10` (for batching) |
| `--frames` | image_streamer | Total frames to generate | 0 (infinite) | `1000` |
| `--dt-ms` | image_streamer | Interval between frames | 50 | `100` |
| `--size` | image_streamer | Frame width/height | 32 | `192` |
| `--nslices` | image_streamer | Z slices per frame | 4 | `15` |

#### HTTP API Endpoints

**MRI Marshal:**
- `POST /v1/mrd/frame` - SWMR append frame to active HDF5
- `POST /v1/mrd/ingest` - Create new HDF5 file with single frame
- `GET /v1/mrd/latest` - Read latest frame (SWMR compatible)
- `GET /health` - Server health check

**Robot Marshal:**
- `POST /write/<filename>` - Write state to file
- `GET /read/<filename>` - Read latest state
- `GET /read/<filename>?last=N` - Read N most recent entries

#### Visualizer Controls
```
UP arrow    - Previous slice
DOWN arrow  - Next slice
ESC         - Exit
```

#### Performance Tuning

**For real-time display:**
```bash
--flush-frames 1        # Immediate flush (default, SWMR works)
--dt-ms 50              # 20 fps (achievable with HDF5)
--size 128 --nslices 10 # Or smaller for faster processing
```

**For maximum throughput:**
```bash
--flush-frames 10       # Batch 10 frames (loses real-time)
--dt-ms 50              # Still limited by HDF5 to ~4-8 fps
# Result: 40-80 MB/s throughput
```

**For 10ms frames (if using batching/Zarr):**
```bash
# Not achievable with HDF5 SWMR - requires architectural change
```

---

### 4. IMPROVEMENTS & OPTIMIZATION GUIDE
**Output file:** `docs/IMPROVEMENTS_AND_OPTIMIZATION.md`

**Content to include:**

#### Current Limitations

| Limitation | Root Cause | Impact |
|---|---|---|
| Max 19 fps sustained | HDF5 SWMR metadata sync | Real-time but not high-speed |
| 10ms intervals fail | Metadata overhead per frame | 0.4 fps instead of 100 fps |
| WSL2 adds 5-10ms | Syscall translation | Minor compared to HDF5 |
| Robot clients low ops | Upstream design (multi-read fails) | Demo shows concept, acceptable |

#### Potential Improvements

##### 1. Batching Implementation (10x improvement)
**Difficulty:** Medium | **Effort:** 2-3 days | **Gain:** 4-8 fps

Strategy:
```cpp
// In image_streamer main loop:
- Collect 10 frames in RAM buffer
- After 10 frames OR timeout: write all in one HDF5 operation
- Benefit: 1 metadata sync per 100ms instead of per 10ms
- Tradeoff: 100ms latency for visualization
```

**When to use:** When throughput > real-time is priority

##### 2. Zarr Format Alternative (8-15x improvement)
**Difficulty:** Hard | **Effort:** 1 week | **Gain:** 8-15 fps

Strategy:
```
Replace HDF5 with Zarr (cloud-native format)
- Better SWMR design
- Per-chunk metadata (not global)
- Same structured data benefits as HDF5
- Compatible with existing readers
```

**When to use:** New projects, maximum performance needed

##### 3. Separate Write/Read Paths (Real-time + batch)
**Difficulty:** Medium | **Effort:** 3-4 days | **Gain:** ~19 fps read-only while batching writes

Strategy:
```
- Writer: batch 10 frames to temporary file
- Reader: poll previous complete batches
- Result: Fast writes + responsive visualization
```

##### 4. Persistent HDF5 Handles (2-3x improvement)
**Difficulty:** Easy | **Effort:** 1 day | **Gain:** ~30-40% faster per-frame

Strategy:
```cpp
// Currently: open/close HDF5 file per frame
// Instead: keep file handle open for batch
- Saves ~30-40% of overhead per frame
- Less impact than batching, but simpler
```

##### 5. Compression Tuning
**Difficulty:** Easy | **Effort:** 2-4 hours | **Gain:** 2x disk space, varies FPS

Options:
```
--compress gzip    // Slower writes, smaller files
--compress zstd    // Faster than gzip, modern
--no-compress      // Fastest writes, large files
```

##### 6. Native Linux/Docker (5-10x improvement in WSL2 overhead)
**Difficulty:** Easy | **Effort:** 1 hour | **Gain:** Removes WSL2 syscall tax

```bash
# Use native Linux container instead of WSL2
# Improves by ~5-10x but still limited by HDF5 to 19 fps
```

#### Implementation Recommendations

**For 20 fps real-time (current goal):**
✅ Keep current 50ms interval design
✅ Consider persistent HDF5 handles (easy win)
❌ No need for batching (loses real-time)

**For 100 fps throughput (high-speed scanning):**
⚠️ Requires architecture change
Options:
1. Batching + 100ms latency (simplest)
2. Zarr format (best long-term)
3. Binary format (fastest but loses structure)

**For 10ms real-time (medical/robotics):**
❌ Not achievable with HDF5 SWMR
Alternatives:
- Use Zarr format
- Use in-memory buffer + async disk write
- Use different storage (database, message queue)

#### Performance Debugging Guide

```bash
# Monitor actual throughput
watch -n 1 'du -sh ./data_mri/mrd/'

# Profile HDF5 operations
export HDF5_DEBUG=all

# Check frame timing
grep "frame.*ms" /tmp/streamer.log | head -20

# Monitor system resources
htop  # CPU, memory usage
iotop # Disk I/O
```

---

## Reference Data & Figures

### Benchmark Results
```
TEST 1 (SWMR @ 50ms):
  Frames:  100
  Time:    5.2s
  FPS:     19.2
  Throughput: 40.4 MB/s
  Status:  ✓ PASS

TEST 2 (Full Ingest):
  Frames:  20
  Time:    21.3s
  FPS:     0.94
  Throughput: 1.98 MB/s
  Status:  ✓ PASS

TEST 3 (Read /v1/mrd/latest):
  Requests: 100
  Time:     808ms
  RPS:      123.76
  Latency:  8.08ms avg
  Status:   ✓ PASS

TEST 4 (SWMR @ 10ms - aggressive):
  Target:   3000 frames
  Actual:   100 frames
  Time:     247s
  FPS:      0.40
  Success:  3.3%
  Status:   ✗ FAIL (HDF5 metadata limited)
```

### System Architecture Diagram
```
┌─────────────────────────────────────────┐
│         Image Streamer (HTTP POST)       │
│    Generates frames @ --dt-ms interval   │
└──────────────┬──────────────────────────┘
               │ /v1/mrd/frame or /v1/mrd/ingest
       ┌───────▼────────────────┐
       │   MRI Data Marshal     │
       │  (HTTP/WebSocket)      │
       │ ┌────────────────────┐ │
       │ │ HDF5 SWMR Storage  │ │
       │ │ (Real-time write)  │ │
       │ └────────────────────┘ │
       └───────┬────────────────┘
               │ /v1/mrd/latest
       ┌───────▼──────────────┐
       │  Visualizer (OpenCV) │
       │  (Slice Navigation)  │
       └──────────────────────┘
```

### File Organization
```
/workspaces/cwru_data_marshal/
├── build/
│   ├── marshal (MRI data marshal)
│   ├── image_streamer (frame generator)
│   ├── viz_client (visualizer)
│   └── robot_marshal_demo (state blackboard)
├── src/ (source code)
├── clients/ (image_streamer, viz_client)
├── scripts/
│   ├── run_demo_simultaneous.sh (main demo)
│   └── benchmarks/
│       ├── mri_marshal_stress_test.sh
│       └── swmr_continuous_bench.sh
└── docs/ (to be generated)
    ├── MRI_DATA_MARSHAL_PRESENTATION.md
    ├── DEMO_GUIDE.md
    ├── USAGE_AND_API.md
    └── IMPROVEMENTS_AND_OPTIMIZATION.md
```

---

## Context for Documentation Writer

### Key Points to Emphasize
1. **What it does:** Real-time MRI frame ingestion with simultaneous multi-client access
2. **How it works:** HDF5 SWMR + HTTP APIs + circular buffering
3. **What it's good at:** 20 fps real-time streaming, multi-client access, structured storage
4. **What it can't do:** 10ms frames with HDF5 (architectural limit, not a bug)
5. **When to use:** Medical imaging, robotics, real-time data fusion
6. **When NOT to use:** High-speed acquisition (>100 fps), non-scientific data

### Tone & Style
- **Professional but accessible** for academic audience
- **Honest about limitations** (HDF5 metadata overhead is real)
- **Data-driven** (include benchmark results)
- **Practical** (include copy-paste commands)
- **Forward-looking** (suggest improvements without being defensive)

### Benchmark Scripts Location
Run these to verify claims:
- `scripts/benchmarks/mri_marshal_stress_test.sh` (~5 min, 5 tests)
- `scripts/benchmarks/swmr_continuous_bench.sh` (~4 min, aggressive test)

---

## What NOT to Include
- Robot-data-marshal internals (mention integration but don't explain)
- Detailed C++ code implementation
- WSL2-specific troubleshooting (keep it brief)
- Historical development notes

## Deliverables Checklist
- [ ] MRI_DATA_MARSHAL_PRESENTATION.md (4-6 pages)
- [ ] DEMO_GUIDE.md (3-4 pages)
- [ ] USAGE_AND_API.md (4-5 pages, technical reference)
- [ ] IMPROVEMENTS_AND_OPTIMIZATION.md (5-7 pages)
- [ ] All docs placed in `/docs/` folder
- [ ] All docs use markdown format
- [ ] Include code blocks for examples
- [ ] Include benchmark data tables
- [ ] Cross-reference between documents

---

**Ready for next agent.** All context provided. Handoff complete.
