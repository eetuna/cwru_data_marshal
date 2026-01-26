# HANDOVER - Fix Option 2 (Async Write Queue) FPS Issues

**Date:** 2026-01-25
**Previous Agent:** Diagnosed that Option 2 causes FPS drops (2.9-19.8 fps) vs stable Option 1 (8.9-19.9 fps)
**Your Task:** Fix Option 2 or implement a better async write solution

---

## Problem Summary

**Option 2 (feature/async-write-queue) causes severe FPS instability:**
- FPS drops to as low as 2.95 fps
- Repeated patterns: 6.94 → 4.97 → 3.96 fps
- Then recovers to 19-20 fps, then drops again

**Actual test data:**
- Original branch: FPS 7.8-20.7 (stable) ✅
- Option 1 only: FPS 8.9-19.9 (stable) ✅
- Option 1+2: FPS 2.9-19.8 (UNSTABLE) ❌

See [VIZ_FPS_INVESTIGATION_FINDINGS.md](VIZ_FPS_INVESTIGATION_FINDINGS.md) for complete test logs.

---

## Root Cause Analysis

### Current Implementation Issues

**Branch:** `feature/async-write-queue`

**What's Actually Async:**
- Bio signals (`/bio`) → queued to `background_writer`
- Pose updates (`/pose`) → queued to `background_writer`

**What's Still Synchronous:**
- **MRD image frames** → written directly via `mrd_sink->append_frame()` in HTTP thread

**Key Code Locations:**

1. **`src/marshal_main.cpp:92-135`** - `background_writer()`
   - Only processes bio/pose writes (lines 117-121)
   - MRD_FRAME case not implemented (line 123-127)
   - Comment says "MRD frames use mrd_sink->append_frame directly"

2. **`src/marshal_http.hpp:391-397`** - Image POST handler
   - Calls `state.mrd_sink->append_frame()` synchronously in HTTP thread
   - With 4 HTTP threads, this creates contention

3. **`src/mrd_sink.cpp`** - Heavy locking
   - Multiple mutexes: `write_mutex_`, `map_mutex_`, per-stream `mutex`
   - Lock contention when multiple threads write frames
   - Lines with locks: 172, 317, 429, 462, 480, 499, 505, 511, 526, 533, 556, 565, 588

### Why FPS Drops Occur

**Hypothesis:** Lock contention in mrd_sink with multi-threaded writes

With Option 1+2:
1. Image streamer sends frames at 20 fps
2. Multiple HTTP threads (4) handle requests
3. Each thread calls `mrd_sink->append_frame()` which acquires locks
4. **Lock contention** → some threads block waiting for locks
5. While blocked, those threads can't respond to viz client `/latest` requests
6. Viz client's `/latest` request times out or gets delayed response
7. Viz client's `H5Drefresh()` may also block waiting for SWMR data
8. Result: FPS drops to 3-5 fps

**Why Option 1 doesn't have this issue:**
- Option 1 has multi-threading but synchronous writes
- Without async queue complexity, locking patterns are simpler
- No additional contention from background writer

**Why Original doesn't have this issue:**
- Single-threaded io_context → no lock contention
- Only one HTTP request at a time

---

## Recommended Fixes (In Priority Order)

### Option A: Fix Option 2 with Proper Async MRD Writes

**Goal:** Actually queue MRD frames for background writing

**Changes needed:**

#### 1. Enable MRD frame queuing in background_writer

**File:** `src/marshal_main.cpp` lines 123-127

**Current code:**
```cpp
case WriteRequest::Type::MRD_FRAME:
    // MRD frames use mrd_sink->append_frame directly (has its own flush policy)
    // This case is reserved for future async MRD write implementation
    std::cerr << "[WRITER] MRD_FRAME queuing not yet implemented\n";
    break;
```

**Replace with:**
```cpp
case WriteRequest::Type::MRD_FRAME:
    state.mrd_sink->append_frame(req.stream_name,
                                  req.dims,
                                  req.element_type,
                                  req.header_xml,
                                  req.frame_data.data(),
                                  req.frame_data.size() * sizeof(float),
                                  req.session_header);
    break;
```

#### 2. Queue MRD frames in HTTP handler

**File:** `src/marshal_http.hpp` lines 388-397

**Current code:**
```cpp
auto result = state.mrd_sink->append_frame(stream_header,
                                          dims,
                                          element_type,
                                          header_xml,
                                          payload,
                                          payload_bytes,
                                          session_header);
```

**Replace with:**
```cpp
// Queue frame write for background thread
try {
    WriteRequest req;
    req.type = WriteRequest::Type::MRD_FRAME;
    req.stream_name = stream_header;
    req.dims = dims;
    req.element_type = element_type;
    req.header_xml = header_xml;
    req.session_header = session_header;

    // Copy frame data
    req.frame_data.resize(payload_bytes / sizeof(float));
    std::memcpy(req.frame_data.data(), payload, payload_bytes);

    {
        std::lock_guard<std::mutex> lock(state.write_queue_mtx);
        state.write_queue.push(std::move(req));
        state.write_queue_cv.notify_one();
    }

    // Return optimistic response (frame will be written soon)
    json resp_data = {
        {"path", "[queued]"},
        {"stream", stream_header},
        {"frame_index", 0},  // Don't know yet
        {"flushed", false},
        {"ts", mrd::iso8601_now_ms()},
        {"t_ms", mrd::now_ms_epoch()},
        // ...
    };
}
catch (const std::exception& e) {
    std::cerr << "MRD frame queue push failed: " << e.what() << "\n";
    // Fall back to synchronous write
}
```

#### 3. Update WriteRequest struct

**File:** `src/marshal_state.hpp` lines 55-68

**Add missing fields:**
```cpp
struct WriteRequest {
    enum class Type { MRD_FRAME, BIO_SIGNAL, POSE_UPDATE, FILE_APPEND };
    Type type;

    // For MRD_FRAME
    std::string stream_name;
    std::vector<float> frame_data;
    std::array<uint16_t, 3> dims;
    uint16_t channels;
    std::string element_type;        // ADD THIS
    std::string header_xml;          // ADD THIS
    std::string session_header;      // ADD THIS

    // For BIO_SIGNAL / POSE_UPDATE / FILE_APPEND
    std::string json_payload;
    std::filesystem::path file_path;
};
```

#### 4. Add explicit HDF5 flush after async write

**File:** `src/marshal_main.cpp` in `background_writer()` after MRD frame write

```cpp
case WriteRequest::Type::MRD_FRAME:
    {
        auto result = state.mrd_sink->append_frame(...);

        // CRITICAL: Force flush for SWMR compatibility
        // This ensures viz client can read the data immediately
        // Note: This may reduce async write benefits but is necessary for correctness
    }
    break;
```

**Problem with this approach:** If you add explicit flush, you lose most async write benefits.

---

### Option B: Hybrid Approach (RECOMMENDED)

**Goal:** Keep bio/pose async, but optimize MRD writes differently

**Strategy:**
1. Keep Option 1 (multi-threaded io_context)
2. Keep bio/pose async (helps with small frequent writes)
3. **Remove async MRD writes** (they don't help due to flush requirement)
4. **Optimize mrd_sink locking** instead

**Changes:**

#### 1. Reduce lock scope in mrd_sink

**File:** `src/mrd_sink.cpp`

Review all `std::lock_guard` usages and reduce lock duration:
- Lines 172, 317: `write_mutex_` locks during H5Dwrite - can this be finer-grained?
- Lines 429, 462, 480, etc: Stream state locks - can we use try_lock?

#### 2. Add per-stream write buffers

Instead of locking for every frame, buffer multiple frames per stream:
- Each stream gets its own write buffer
- Background thread flushes buffers periodically
- Reduces lock contention

---

### Option C: Lock-Free Queue for MRD Frames

**Goal:** Use lock-free concurrent queue instead of mutex-protected std::queue

**Library:** Use `boost::lockfree::queue` or similar

**Changes:**

**File:** `src/marshal_state.hpp`

```cpp
#include <boost/lockfree/queue.hpp>

struct MarshalState {
    // Replace:
    // std::queue<WriteRequest> write_queue;
    // std::mutex write_queue_mtx;

    // With:
    boost::lockfree::queue<WriteRequest*> write_queue{128};  // Fixed size

    // Keep condition_variable for signaling
    std::condition_variable write_queue_cv;
    std::mutex cv_mutex;  // Only for CV, not queue access
};
```

**Pros:** Eliminates lock contention
**Cons:** Fixed queue size, need to manage WriteRequest memory

---

## Testing Requirements

After implementing any fix, you MUST test with FPS debug logging:

### Test Script

```bash
# Add FPS debug logging to viz_client (already done on all branches)
# See clients/viz_client/viz_client_main.cpp lines 504-505

# Build with your changes
./scripts/build-client-images.sh

# Run 60-second test
./scripts/demo-docker.sh > /tmp/demo_test.log 2>&1 &
sleep 55
docker logs cwru-viz-client 2>&1 | grep "FPS DEBUG" | tee /tmp/fps_test.log
docker stop $(docker ps -q --filter "name=cwru") 2>/dev/null

# Analyze results
cat /tmp/fps_test.log
```

### Success Criteria

**Your fix is successful if:**
- ✅ FPS stays above 10 fps consistently
- ✅ No drops below 5 fps
- ✅ FPS range similar to Option 1: 8-20 fps
- ✅ No severe drop patterns (like 6→4→3 fps)

### Comparison Baseline

**Compare your results to:**
- Original: 7.8-20.7 fps (stable)
- Option 1: 8.9-19.9 fps (stable)
- Option 1+2 (broken): 2.9-19.8 fps (unstable)

**Your fixed Option 2 should match or exceed Option 1 performance.**

---

## Alternative: Abandon Option 2

If fixing Option 2 proves too complex, **stick with Option 1 only:**

**Rationale:**
- Option 1 provides significant benefits (multi-threading)
- Stable FPS (8.9-19.9 fps)
- Bio/pose async writes don't provide enough benefit to justify complexity
- MRD frames need immediate flush anyway (SWMR requirement)

**Action:** Keep build script at:
```bash
MRI_BRANCH="feature/multi-threaded-io"  # Option 1 only
```

---

## Key Files to Modify

1. **`src/marshal_main.cpp`** - background_writer() implementation
2. **`src/marshal_http.hpp`** - Image POST handler (line 391)
3. **`src/marshal_state.hpp`** - WriteRequest struct
4. **`src/mrd_sink.cpp`** - Lock optimization (if doing Option B)

---

## Questions to Answer

1. **Is async MRD write actually beneficial?**
   - MRD frames must be flushed immediately for SWMR
   - Async write adds latency and complexity
   - May not be worth it

2. **What's the real bottleneck?**
   - Lock contention in mrd_sink?
   - HDF5 write speed?
   - Network I/O?
   - Profile to confirm

3. **Can we reduce locking in mrd_sink?**
   - Review all mutex acquisitions
   - Can any be lock-free?
   - Can scope be reduced?

---

## Success Metrics

After your fix, demonstrate:
- ✅ FPS test results showing stable 10-20 fps
- ✅ No timeout errors from ECG/pose clients
- ✅ HTTP response latency < 10ms
- ✅ Demo runs 10+ minutes without degradation

---

## Resources

- **FPS test logs:** `/tmp/fps_original.log`, `/tmp/fps_option1.log`, `/tmp/fps_option1+2.log`
- **Investigation report:** [VIZ_FPS_INVESTIGATION_FINDINGS.md](VIZ_FPS_INVESTIGATION_FINDINGS.md)
- **Threading docs:** `docs/THREADING_ARCHITECTURE_OPTIONS.md`
- **Branches:**
  - `feature/multi-threaded-io` - Option 1 (stable baseline)
  - `feature/async-write-queue` - Option 1+2 (needs fixing)

Good luck! The goal is either to fix Option 2 properly or determine it's not worth the complexity.
