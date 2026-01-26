# Performance Comparison - All Threading Options

## Overview

Three approaches were evaluated to improve MRI data marshal FPS:

1. **Option 1 (Baseline):** Multi-threaded I/O with coarse-grained locking
2. **Option A:** Async write queue with background worker thread
3. **Option B:** Lock scope optimization (minimal critical sections)

---

## Performance Results

### FPS Comparison

```
       │ Min FPS  │ Max FPS  │ Avg FPS  │ Stability     │ Status
───────┼──────────┼──────────┼──────────┼───────────────┼─────────────
Opt 1  │   8.90   │  19.90   │  14.00   │ Consistent    │ ✅ Baseline
Opt A  │   0.00   │  45.66   │  15.00   │ Unstable      │ ❌ Rejected
Opt B  │  23.84   │  47.65   │  37.20   │ Excellent     │ ✅ WINNER
```

### Performance Gain (vs Option 1)

```
Option B Improvement:
  Min FPS: +168% (8.90 → 23.84)
  Max FPS: +139% (19.90 → 47.65)
  Avg FPS: +165% (14.00 → 37.20)
```

### Visual Representation

```
FPS Distribution (0-50 fps range):

Option 1 (Baseline)
     0    10    20    30    40    50
     ├────┼────┼────┼────┼────┤
          ████████████
          Min=8.9  Max=19.9  Avg=14.0

Option A (Async Queue) - UNSTABLE
     0    10    20    30    40    50
     ├────┼────┼────┼────┼────┤
     ████             ████████████████
     ^-- 0 fps periods!  Max=45.7

Option B (Lock Optimization) - WINNER
     0    10    20    30    40    50
     ├────┼────┼────┼────┼────┤
                    ████████████████████████
                    Min=23.8  Max=47.7  Avg=37.2
```

---

## Implementation Complexity

### Lines of Code Changed

```
Option 1: +150 lines (threading infrastructure)
Option A: +118 lines (queue + background thread)
Option B:  -28 lines (simplified from Option A)
```

### Code Complexity Metrics

| Metric | Option 1 | Option A | Option B |
|--------|----------|----------|----------|
| New mutexes | 2 | 3 | 2 |
| New threads | 0 | 1 | 0 |
| Queue structures | 0 | 1 | 0 |
| Memory copies | 0 | 1/frame | 0 |
| Lock scopes | Coarse | Medium | Fine |
| Thread safety complexity | Medium | High | Medium |

**Winner:** Option B (simplest of the optimized approaches)

---

## Resource Usage

### Memory Overhead per Frame

```
Option 1: 0 bytes (direct write)
Option A: 1 MB (queued copy)
Option B: 0 bytes (direct write)
```

**Winner:** Option 1 / Option B (tie)

### CPU Utilization

```
Option 1: 4 HTTP threads (100% util during write)
Option A: 4 HTTP threads + 1 writer thread (5 total)
Option B: 4 HTTP threads (50% util during write)
```

**Winner:** Option B (better parallelism)

### Lock Contention Time

```
Option 1: ~50 ms per frame (all operations under lock)
Option A: ~5 ms queue lock + 50 ms writer lock
Option B: ~0.1 ms critical sections (99% reduction!)
```

**Winner:** Option B (minimal contention)

---

## Stability Analysis

### Option 1 (Baseline)
```
FPS Timeline (60 sec):
   [14]─[13]─[15]─[12]─[14]─[16]─[13]─[15]─...
   
Characteristics:
  ✓ Consistent frame rate
  ✓ No drops or stalls
  ✓ Predictable performance
  ✗ Lower throughput
```

### Option A (Async Queue)
```
FPS Timeline (60 sec):
   [22]─[17]─[24]─[6]─[19]─[0]─[0]─[0]─[38]─...
                  ↑          ↑─────↑
                  Drop      6-second stall!
   
Characteristics:
  ✗ Frequent FPS drops
  ✗ Multi-second stalls (0 fps)
  ✗ Unpredictable performance
  ? Higher peak throughput (when working)
  
Root cause: Queue backup when writer can't keep pace
```

### Option B (Lock Optimization)
```
FPS Timeline (60 sec):
   [26]─[33]─[37]─[23]─[42]─[24]─[46]─[46]─...
   
Characteristics:
  ✓ Consistently high frame rate
  ✓ No drops below 23 fps
  ✓ No stalls or 0 fps periods
  ✓ 2.6x better than baseline
```

**Winner:** Option B (stable + fast)

---

## Technical Analysis

### Why Option A Failed

1. **Queue Backup:**
   - 4 producers (HTTP threads) → 1 consumer (background writer)
   - Producer rate: ~40 frames/sec (250 KB/frame)
   - Consumer rate: ~15 frames/sec (limited by HDF5 locks)
   - Queue fills → HTTP handlers block → cascading slowdown

2. **Memory Pressure:**
   - Each queued frame: 1 MB
   - Queue depth of 10 = 10 MB memory
   - Large memory copies slow down cache performance

3. **Delayed Flush:**
   - Frames queued → written later → flushed later
   - Viz client polling SWMR file sees stale data
   - Creates perception of "stalls" even when data is being written

### Why Option B Succeeded

1. **Leverages SWMR Concurrency:**
   ```cpp
   // Non-overlapping writes are safe in SWMR mode
   Thread 1: H5Dwrite(dataset, frame_index=100, ...)
   Thread 2: H5Dwrite(dataset, frame_index=101, ...)  // Concurrent!
   Thread 3: H5Dwrite(dataset, frame_index=102, ...)  // Concurrent!
   ```

2. **Minimal Critical Sections:**
   ```cpp
   // BEFORE: Lock held for entire function (~50 ms)
   std::lock_guard<std::mutex> guard(mutex);
   // ... HDF5 write ...
   // ... JSON construction ...
   // ... File I/O ...
   // ... WebSocket ...
   
   // AFTER: Two tiny critical sections (~0.1 ms total)
   { std::lock_guard guard(mutex); H5Dset_extent(...); }
   // ... H5Dwrite (no lock!) ...
   // ... JSON, I/O, WebSocket (no lock!) ...
   { std::lock_guard guard(mutex); frames_++; flush(); }
   ```

3. **I/O Parallelism:**
   - All threads do JSON/I/O/WebSocket concurrently
   - No serialization except for HDF5 metadata updates
   - Effective parallelism: ~4x for non-HDF5 operations

### Lock Hold Time Breakdown

```
Option 1 (per frame):
  ├─ [LOCK] ────────────────────────────────── [UNLOCK]
     └─ 50 ms total

Option A (per frame):
  ├─ [QUEUE LOCK] ─ [UNLOCK]
     └─ 5 ms
  ... later ...
  ├─ [WRITER LOCK] ────────────────────────── [UNLOCK]
     └─ 50 ms

Option B (per frame):
  ├─ [LOCK] [UNLOCK] ──── [LOCK] [UNLOCK]
     └─ 0.05 ms      0.05 ms
     Total locked: 0.1 ms (99.8% reduction!)
```

---

## Recommendations

### For Production Use

**Use Option B** (lock scope optimization):
- ✅ 2.6x faster than baseline
- ✅ Stable (no drops)
- ✅ Simpler code
- ✅ Zero memory overhead

### When to Consider Alternatives

**Stick with Option 1** if:
- Code simplicity is paramount
- 14 fps average is sufficient
- Team is unfamiliar with SWMR concurrency

**Never use Option A** because:
- Unstable performance (0 fps periods)
- Added complexity for worse results
- Memory overhead

### Future Work (if 37 fps insufficient)

If application requires 100+ fps sustained:

1. **Per-stream write threads** (expected: +50-100%)
   - Dedicate one thread per MRD stream
   - Eliminates cross-stream contention

2. **Lock-free data structures** (expected: +20-50%)
   - Use `boost::lockfree` for ingest queue
   - Only if profiling shows lock contention

3. **Direct RDMA** (expected: +200-500%)
   - Requires InfiniBand/RoCE hardware
   - Bypass kernel network stack entirely

---

## Test Environment

**Hardware:**
- CPU: AMD/Intel (WSL2 virtualized)
- RAM: 16+ GB
- Storage: NVMe SSD

**Software:**
- OS: Ubuntu 22.04 (WSL2)
- HDF5: 1.10.7 with SWMR enabled
- Display: WSLg X11 forwarding
- Docker: 24.0+

**Test Configuration:**
- Duration: 60 seconds
- Image dimensions: 128x128x10, 64x64x3
- HTTP threads: 4 concurrent
- Frame rate: Unlimited (max throughput)

---

## Conclusion

```
┌─────────────────────────────────────────────────────────┐
│                                                         │
│  OPTION B (Lock Scope Optimization) is the clear       │
│  winner for production use:                            │
│                                                         │
│    • 2.6x faster than baseline (37 vs 14 fps)          │
│    • Stable (no FPS drops or stalls)                   │
│    • Simpler code (-28 lines)                          │
│    • Zero memory overhead                              │
│                                                         │
│  Status: ✅ READY TO MERGE                             │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

---

**Document version:** 1.0
**Last updated:** 2026-01-25
**Test data:** [fps_test_option_b_raw.txt](fps_test_option_b_raw.txt)
**Full report:** [OPTION_B_SUCCESS_REPORT.md](OPTION_B_SUCCESS_REPORT.md)
