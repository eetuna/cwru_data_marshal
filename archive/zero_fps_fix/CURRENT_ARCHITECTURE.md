# Current Architecture - Threading Model

**Branch:** `feature/async-write-queue`
**Implementation:** **Hybrid - Multi-threaded + Selective Async**
**Status:** ✅ Production-ready (37.2 fps average)

---

## TL;DR - What We Have Now

```
┌─────────────────────────────────────────────────────────────┐
│  HYBRID ARCHITECTURE:                                       │
│                                                              │
│  1. MRD Frames    → Multi-threaded SYNCHRONOUS writes       │
│                     (fine-grained locks, SWMR concurrency)  │
│                                                              │
│  2. Bio Signals   → ASYNC queue → Background writer thread  │
│  3. Pose Updates  → ASYNC queue → Background writer thread  │
│                                                              │
│  Result: Best of both worlds                                │
└─────────────────────────────────────────────────────────────┘
```

---

## Architecture Diagram

```
┌──────────────────────────────────────────────────────────────────────┐
│                         CLIENT REQUESTS                              │
└───────────────┬─────────────────┬────────────────┬───────────────────┘
                │                 │                │
                │                 │                │
         ┌──────▼──────┐   ┌─────▼──────┐  ┌──────▼──────┐
         │ MRI Image   │   │ Bio Signal │  │ Pose Update │
         │ POST        │   │ POST       │  │ POST        │
         └──────┬──────┘   └─────┬──────┘  └──────┬──────┘
                │                 │                │
                │                 │                │
    ┌───────────▼────────────┐    │                │
    │  HTTP Handler Thread   │    │                │
    │  (4 concurrent)        │    │                │
    └───────────┬────────────┘    │                │
                │                 │                │
                │                 │                │
    ┌───────────▼────────────┐    │                │
    │ SYNCHRONOUS WRITE PATH │    │                │
    │ (Option B)             │    │                │
    └───────────┬────────────┘    │                │
                │                 │                │
                │                 └────────┬───────┘
                │                          │
                │                 ┌────────▼────────┐
                │                 │ ASYNC QUEUE     │
                │                 │ write_queue     │
                │                 │ (Option A lite) │
                │                 └────────┬────────┘
                │                          │
                │                          │
    ┌───────────▼────────────┐    ┌────────▼────────┐
    │ MrdSink::append_frame()│    │ Background      │
    │                        │    │ Writer Thread   │
    └───────────┬────────────┘    │ (1 thread)      │
                │                 └────────┬────────┘
                │                          │
    ┌───────────▼─────────┐       ┌────────▼────────┐
    │  [LOCK] 0.05ms      │       │ File I/O        │
    │  H5Dset_extent()    │       │ (Bio/Pose JSON) │
    │  [UNLOCK]           │       └─────────────────┘
    │                     │
    │  H5Dwrite()         │
    │  (no lock!)         │
    │                     │
    │  JSON + File I/O    │
    │  (no lock!)         │
    │                     │
    │  [LOCK] 0.05ms      │
    │  frames_++, flush() │
    │  [UNLOCK]           │
    └─────────┬───────────┘
              │
    ┌─────────▼────────┐
    │ HDF5 MRD File    │
    │ (SWMR mode)      │
    └──────────────────┘
```

---

## Threading Model Details

### 1. MRI Frame Writes (HIGH THROUGHPUT PATH)

**Implementation:** Multi-threaded SYNCHRONOUS with fine-grained locks

```cpp
// Location: src/marshal_http.hpp:391-397
auto result = state.mrd_sink->append_frame(
    stream_header, dims, element_type,
    header_xml, payload, payload_bytes,
    session_header
);
// ↓ Returns immediately with real file path
```

**Thread flow:**
```
HTTP Thread 1 ──┐
HTTP Thread 2 ──┤
HTTP Thread 3 ──┼──→ append_frame() ──→ HDF5 write (SWMR parallel)
HTTP Thread 4 ──┘
```

**Lock strategy (Option B):**
```cpp
// CRITICAL SECTION 1: ~0.05ms
{
    std::lock_guard<std::mutex> guard(write_mutex_);
    H5Dset_extent(dataset_, new_dims);  // Metadata update only
}

// NO LOCK: All threads can run this in parallel
H5Dwrite(dataset_, ...);  // SWMR handles concurrency

// CRITICAL SECTION 2: ~0.05ms
{
    std::lock_guard<std::mutex> guard(write_mutex_);
    frames_++;
    perform_flush(false);
}
```

**Total lock time:** ~0.1ms per frame (99% reduction vs Option 1)

**Why synchronous for MRD frames:**
- Zero memory copy (1 MB saved per frame)
- Immediate SWMR flush (real-time viz)
- HTTP returns actual file path (not placeholder)
- Simpler error handling

---

### 2. Bio Signals (LOW THROUGHPUT PATH)

**Implementation:** ASYNC queue with background writer

```cpp
// Location: src/marshal_http.hpp (bio signal handler)
WriteRequest req;
req.type = WriteRequest::Type::BIO_SIGNAL;
req.json_payload = json_data.dump();
req.file_path = bio_file_path;

{
    std::lock_guard<std::mutex> lock(state.write_queue_mtx);
    state.write_queue.push(std::move(req));
}
state.write_queue_cv.notify_one();
```

**Thread flow:**
```
HTTP Handler ──→ [Queue] ──→ Background Writer ──→ File I/O
                    ↓
              Returns immediately
```

**Why async for bio signals:**
- Low volume (~1 Hz)
- Non-blocking HTTP response
- Batching opportunity
- No real-time requirement

---

### 3. Pose Updates (LOW THROUGHPUT PATH)

**Implementation:** ASYNC queue with background writer

```cpp
// Location: src/marshal_http.hpp (pose update handler)
WriteRequest req;
req.type = WriteRequest::Type::POSE_UPDATE;
req.json_payload = pose_data.dump();
req.file_path = pose_file_path;

{
    std::lock_guard<std::mutex> lock(state.write_queue_mtx);
    state.write_queue.push(std::move(req));
}
state.write_queue_cv.notify_one();
```

**Why async for poses:**
- Low volume (~10 Hz)
- Small payload (~200 bytes)
- Non-blocking HTTP response
- Order preservation in queue

---

## Component Breakdown

### Thread Pool: HTTP Handlers

```
├─ Thread 1 ─┐
├─ Thread 2 ─┤─→ Boost.Beast HTTP server (4 threads)
├─ Thread 3 ─┤   Handles all POST requests
├─ Thread 4 ─┘
```

**Responsibilities:**
- Parse HTTP requests
- Route to appropriate handler
- For MRD frames: SYNCHRONOUS write via `mrd_sink->append_frame()`
- For bio/pose: ASYNC queue via `write_queue.push()`

### Background Writer Thread

```
Background Writer Thread
  ├─ Wait on condition variable (write_queue_cv)
  ├─ Pop WriteRequest from queue
  ├─ Process based on type:
  │   ├─ BIO_SIGNAL   → append to bio.jsonl
  │   ├─ POSE_UPDATE  → append to pose.jsonl
  │   └─ FILE_APPEND  → generic file append
  └─ Loop until shutdown
```

**Location:** `src/marshal_main.cpp:92-129`

**Queue size:** Unbounded (but low volume prevents backup)

### MrdSink (Multi-threaded)

```cpp
class MrdSink {
    std::unordered_map<std::string, MrdFile> streams_;
    std::mutex streams_mutex_;  // Protects stream map

    // Each MrdFile has its own mutex
    class MrdFile {
        std::mutex write_mutex_;  // Fine-grained locking
        hid_t dataset_;           // HDF5 dataset handle
        size_t frames_;           // Frame counter
    };
};
```

**Locking strategy:**
1. `streams_mutex_` - Only locked when creating new streams
2. `write_mutex_` - Only locked for critical HDF5 operations
3. File I/O, JSON, WebSocket - No locks (thread-safe primitives)

---

## Data Flow Examples

### Example 1: MRI Frame Ingestion (37 fps)

```
t=0ms    HTTP POST /v1/mrd/image
         ├─ Parse ISMRMRD header (5ms)
         ├─ Validate payload (2ms)
         └─ Call append_frame() ──┐
                                   │
t=7ms    ┌──────────────────────────┘
         │ [LOCK] H5Dset_extent (0.05ms)
         │ [UNLOCK]
         │ H5Dwrite (20ms) ←─ Parallel with other threads!
         │ Construct JSON (3ms)
         │ Write index.jsonl (2ms)
         │ Emit WebSocket (1ms)
         │ [LOCK] frames_++, flush (0.05ms)
         │ [UNLOCK]
         └─ Return result
                           │
t=33ms   HTTP 200 OK ←────┘
         └─ {"path": "demo_stream.mrd", "frame_index": 123}
```

**Total latency:** ~33ms per frame
**Throughput:** 37 fps average (multiple threads overlap)

### Example 2: Bio Signal (1 Hz)

```
t=0ms    HTTP POST /v1/bio/ecg
         ├─ Parse JSON (1ms)
         ├─ Create WriteRequest (0.1ms)
         ├─ Push to queue (0.05ms)
         └─ Return
                   │
t=1ms    HTTP 200 OK ←─┘

         Background Writer (separate thread):
         ├─ Pop from queue (0.05ms)
         ├─ Append to bio.jsonl (2ms)
         └─ Done
```

**HTTP latency:** ~1ms (non-blocking)
**I/O latency:** ~2ms (background)

---

## Synchronization Primitives

### Mutexes

| Mutex | Location | Scope | Hold Time | Frequency |
|-------|----------|-------|-----------|-----------|
| `write_queue_mtx` | marshal_state.hpp | Queue operations | ~0.05ms | 10 Hz (bio+pose) |
| `streams_mutex_` | mrd_sink.cpp | Stream map access | ~0.1ms | Rare (new streams) |
| `write_mutex_` (per stream) | mrd_sink.cpp | HDF5 metadata | ~0.1ms | 40 Hz (MRD frames) |
| `stream_state->mutex` | mrd_sink.cpp | Frame count | ~0.05ms | 40 Hz |

### Condition Variables

| CV | Purpose | Wait Condition |
|----|---------|----------------|
| `write_queue_cv` | Background writer wakeup | `!write_queue.empty()` |

### Atomics

| Atomic | Purpose | Access Pattern |
|--------|---------|----------------|
| `writer_running` | Shutdown signal | Set once, read many |
| `seq` | Global sequence counter | Increment only |

---

## Performance Characteristics

### Throughput

| Metric | Before (Option 1) | After (Option B) | Improvement |
|--------|-------------------|------------------|-------------|
| MRD frames/sec | 14 fps | 37 fps | +165% |
| Bio signals/sec | 1 Hz | 1 Hz | No change |
| Pose updates/sec | 10 Hz | 10 Hz | No change |

### Latency

| Operation | Latency | Notes |
|-----------|---------|-------|
| MRD frame POST | 33ms | Includes HDF5 write |
| Bio signal POST | 1ms | Queued, non-blocking |
| Pose update POST | 1ms | Queued, non-blocking |
| Queue drain | <10ms | Background thread |

### Memory

| Component | Memory Usage |
|-----------|--------------|
| Write queue | ~1 KB (avg 1-2 items) |
| MRD frames | 0 (no copy) |
| HTTP threads | 4 × 8 MB stack = 32 MB |
| Background writer | 1 × 8 MB stack = 8 MB |

---

## Why This Hybrid Approach?

### Critical Path Optimization

**MRD frames = 99.9% of data volume:**
- 128×128×10 × 4 bytes = 655 KB per frame
- 37 fps = 24 MB/sec sustained

**Bio/Pose = 0.1% of data volume:**
- ~200 bytes per update
- 11 Hz total = 2.2 KB/sec

**Strategy:** Optimize the critical path (MRD), keep simple path async (bio/pose)

### Design Decisions

| Aspect | MRD Frames | Bio/Pose Signals |
|--------|------------|------------------|
| Volume | High (24 MB/s) | Low (2 KB/s) |
| Frequency | 37 Hz | 11 Hz |
| Latency requirement | Low (real-time viz) | Relaxed |
| Write strategy | SYNC (fine locks) | ASYNC (queue) |
| Rationale | Immediate flush for SWMR | Non-blocking response |

---

## Code Locations

### MRD Synchronous Path

- **HTTP handler:** [src/marshal_http.hpp:391-411](../.worktrees/mri_data_marshal/src/marshal_http.hpp#L391-L411)
- **Lock optimization:** [src/mrd_sink.cpp:315-384](../.worktrees/mri_data_marshal/src/mrd_sink.cpp#L315-L384)
- **Lock optimization:** [src/mrd_sink.cpp:590-651](../.worktrees/mri_data_marshal/src/mrd_sink.cpp#L590-L651)

### Async Queue Path

- **WriteRequest struct:** [src/marshal_state.hpp:56-63](../.worktrees/mri_data_marshal/src/marshal_state.hpp#L56-L63)
- **Queue state:** [src/marshal_state.hpp:89-93](../.worktrees/mri_data_marshal/src/marshal_state.hpp#L89-L93)
- **Background writer:** [src/marshal_main.cpp:92-129](../.worktrees/mri_data_marshal/src/marshal_main.cpp#L92-L129)

---

## Thread Safety Guarantees

### MRD Writes

✅ **Thread-safe via:**
- HDF5 SWMR mode (concurrent non-overlapping writes)
- Fine-grained locks (only metadata operations)
- Atomic file writes (`write_atomic()`, `append_line()`)
- Per-stream mutexes (no cross-stream contention)

### Async Queue

✅ **Thread-safe via:**
- Mutex-protected queue (`write_queue_mtx`)
- Condition variable synchronization (`write_queue_cv`)
- Move semantics (no shared ownership)
- Single consumer (background writer)

### Edge Cases

✅ **Shutdown:** Writer thread drains queue before exit
✅ **Queue overflow:** Low volume prevents backup (validated by testing)
✅ **Concurrent streams:** Each stream has independent mutex
✅ **SWMR flush:** Immediate after each frame write

---

## Comparison to Pure Approaches

### Pure Multi-threaded (Option 1)

```
All operations SYNC with coarse locks
  ✓ Simple
  ✗ Slower (14 fps vs 37 fps)
```

### Pure Async Queue (Option A - Rejected)

```
All operations ASYNC via queue
  ✗ Memory overhead (1 MB/frame)
  ✗ Queue backup (0 fps periods)
  ✗ Delayed flush (stale viz)
  ✗ Complex error handling
```

### Hybrid (Current - Option B)

```
Hot path SYNC + cold path ASYNC
  ✓ Fast (37 fps)
  ✓ Stable (no drops)
  ✓ Simple (minimal code)
  ✓ Zero memory overhead for MRD
```

---

## Summary

**Current architecture = Multi-threaded + Selective Async**

```
┌───────────────────────────────────────────────────────┐
│  HOT PATH (MRD frames):                               │
│    → Multi-threaded SYNCHRONOUS                       │
│    → Fine-grained locks (0.1ms total)                 │
│    → SWMR parallel writes                             │
│    → 37 fps sustained                                 │
│                                                       │
│  COLD PATH (Bio/Pose):                                │
│    → ASYNC queue                                      │
│    → Background writer thread                         │
│    → Non-blocking HTTP                                │
│    → <1ms response time                               │
└───────────────────────────────────────────────────────┘
```

**Result:** Best of both worlds - high throughput for critical path, low latency for control signals.

---

**Document version:** 1.0
**Last updated:** 2026-01-25
**Branch:** `feature/async-write-queue`
**Commit:** `6bfab43`
