# Threading Implementation Summary

## Branch: `feature/async-write-queue`

**Date:** 2026-01-25
**Status:** ⚠️ **NEEDS INVESTIGATION** - Viz client showing inconsistent FPS (44→30→0→6)

---

## Changes Made

### Marshal Server Changes (API-Compatible, No Client Modifications Required)

#### 1. **src/marshal_main.cpp** - Multi-threaded I/O and Background Writer

**Commit 4d3edb2:** Multi-threaded io_context
```cpp
// Line 19: Added include
#include <vector>

// Line 172: Changed from single thread to 4 threads
boost::asio::io_context ioc{4};  // 4 threads for concurrent request handling

// Lines 342-352: Added thread pool
const unsigned int num_threads = 4;
std::vector<std::thread> io_threads;
for (unsigned int i = 0; i < num_threads - 1; ++i) {
    io_threads.emplace_back([&ioc]() { ioc.run(); });
}
ioc.run();  // Main thread also runs io_context
for (auto& t : io_threads) {
    if (t.joinable()) t.join();
}
```

**Commit dce99e7:** Async write queue
```cpp
// Lines 92-138: Added background_writer() function
void background_writer(MarshalState& state) {
    while (state.writer_running.load()) {
        WriteRequest req;
        {
            std::unique_lock<std::mutex> lock(state.write_queue_mtx);
            state.write_queue_cv.wait(lock, [&state] {
                return !state.write_queue.empty() || !state.writer_running.load();
            });
            // ... dequeue and process writes
        }
    }
}

// Line 262: Start writer thread after mrd_sink initialization
std::thread writer_thread(background_writer, std::ref(state));

// Lines 354-359: Graceful shutdown
state.writer_running.store(false);
state.write_queue_cv.notify_all();
if (writer_thread.joinable()) {
    writer_thread.join();
}
```

#### 2. **src/marshal_state.hpp** - Write Queue Infrastructure

```cpp
// Lines 19-22: Added includes
#include <queue>
#include <condition_variable>
#include <filesystem>
#include <fstream>

// Lines 55-68: Added WriteRequest struct
struct WriteRequest {
    enum class Type { MRD_FRAME, BIO_SIGNAL, POSE_UPDATE, FILE_APPEND };
    Type type;

    // For MRD_FRAME
    std::string stream_name;
    std::vector<float> frame_data;
    std::array<uint16_t, 3> dims;
    uint16_t channels;

    // For BIO_SIGNAL / POSE_UPDATE / FILE_APPEND
    std::string json_payload;
    std::filesystem::path file_path;
};

// Lines 95-99: Added queue members
std::queue<WriteRequest> write_queue;
std::mutex write_queue_mtx;
std::condition_variable write_queue_cv;
std::atomic<bool> writer_running{true};

// Lines 101-116: Added drain helper for tests
void drain_write_queue() {
    // Synchronously processes queue for testing
}
```

#### 3. **src/marshal_http.hpp** - Async Endpoint Updates

**Bio signal endpoint** (lines 258-273):
```cpp
// BEFORE (synchronous):
mrd::append_line(bio_log, body.dump());

// AFTER (async queue):
WriteRequest req;
req.type = WriteRequest::Type::BIO_SIGNAL;
req.json_payload = body.dump();
req.file_path = mrd::resolve_sink_paths(state).index_root / "bio.jsonl";

std::lock_guard<std::mutex> lock(state.write_queue_mtx);
state.write_queue.push(std::move(req));
state.write_queue_cv.notify_one();
```

**Pose endpoint** (lines 192-209):
```cpp
// BEFORE (synchronous):
mrd::append_line(pose_log, j_persist.dump());

// AFTER (async queue):
WriteRequest req;
req.type = WriteRequest::Type::POSE_UPDATE;
req.json_payload = j_persist.dump();
req.file_path = mrd::resolve_sink_paths(state).index_root / "poses.jsonl";

std::lock_guard<std::mutex> lock(state.write_queue_mtx);
state.write_queue.push(std::move(req));
state.write_queue_cv.notify_one();
```

**MRD frame endpoint:** NO CHANGES (still uses mrd_sink->append_frame with flush policy)

#### 4. **tests/test_http_handlers.cpp** - Test Compatibility

```cpp
// Line 99: Added drain call before checking file existence
state.drain_write_queue();

// Verify persistence
fs::path pose_log = fs::path(temp) / "mrd" / "poses.jsonl";
CHECK(fs::exists(pose_log));
```

---

## Client Compatibility

### ✅ No Client Changes Required

All clients remain **100% API-compatible**:

- **ecg_client.py** - No changes
- **pose_client.py** - No changes
- **image_streamer** - No changes
- **viz_client** - No changes

**Why?** The HTTP API endpoints remain identical:
- Same request format
- Same response format
- Only difference: **faster responses** (1-2ms vs 20-50ms)

---

## Configuration Changes

### 1. **scripts/build-client-images.sh**

```bash
# Line 20: Changed branch for Docker builds
MRI_BRANCH="feature/async-write-queue"  # Was: "mri-data-marhsal"
```

### 2. **.gitignore**

```
# Added:
session-data/
.worktrees/
*.bak
```

---

## Expected Benefits

| Metric | Before | After (Expected) |
|--------|--------|------------------|
| HTTP response latency | 20-50ms | 1-2ms |
| ECG timeout rate | High | None |
| Pose timeout rate | High | None |
| Concurrent client support | Limited | Excellent |
| Max sustained fps | ~10 | ~100+ |

---

## ⚠️ CRITICAL ISSUE - NEEDS INVESTIGATION

### Problem Report

**Viz client FPS inconsistent:** 44 → 30 → 0 → 6 fps

**Observations:**
1. FPS drops dramatically and unpredictably
2. Pattern suggests threading or synchronization issue
3. May indicate race condition or deadlock
4. Could be related to multi-threaded io_context or async writes

### Possible Causes to Investigate

1. **Race condition in viz_client WebSocket handling**
   - Multiple threads accessing same WebSocket connection?
   - Message ordering issues?

2. **HDF5 SWMR read conflicts**
   - Async writes may not be flushing before viz reads
   - SWMR refresh timing issues

3. **Thread contention**
   - 4 HTTP threads + 1 writer thread + viz client
   - CPU scheduling issues?

4. **Queue backpressure**
   - Write queue filling up faster than draining?
   - Blocking viz client reads?

5. **Image frame availability**
   - Frames not being written in time for viz to read
   - Flush policy (1 frame, 0ms) not triggering correctly

### Investigation Steps

1. **Check logs:**
   ```bash
   docker logs cwru-image-streamer
   docker logs cwru-viz-client
   docker logs cwru-mri-marshal
   ```

2. **Monitor write queue:**
   - Add logging to background_writer to see queue depth
   - Check if queue is filling up

3. **Test without async writes:**
   - Compare with `feature/multi-threaded-io` (Option 1 only)
   - See if Option 2 is causing the issue

4. **Profile performance:**
   - CPU usage per thread
   - Lock contention
   - I/O wait times

5. **Check HDF5 flush timing:**
   - Verify mrd_sink is actually flushing every frame
   - Check if async writes interfere with SWMR

---

## Rollback Plan

If issues persist:

```bash
# Option A: Use Option 1 only (multi-threaded, no async queue)
git checkout feature/multi-threaded-io

# Option B: Revert to original
git checkout mri-data-marhsal
```

---

## Build Instructions

### Build Docker Images
```bash
./scripts/build-client-images.sh
```

### Run Demo
```bash
./scripts/demo-docker.sh
```

### Test Locally
```bash
mkdir -p build && cd build
cmake .. -DBUILD_TESTING=ON
cmake --build . --parallel
ctest --output-on-failure
```

---

## Files Modified Summary

### Core Implementation (4 files)
- `src/marshal_main.cpp` - Thread pool + background writer
- `src/marshal_state.hpp` - WriteRequest struct + queue
- `src/marshal_http.hpp` - Async bio/pose endpoints
- `tests/test_http_handlers.cpp` - Test drain helper

### Configuration (2 files)
- `scripts/build-client-images.sh` - Branch selection
- `.gitignore` - Session data, worktrees

### Documentation (2 files)
- `docs/THREADING_ARCHITECTURE_OPTIONS.md` - Implementation guide
- `HANDOVER_TO_NEXT_AGENT.md` - Implementation steps

**Total:** 8 files modified, 0 client files changed

---

## Testing Status

✅ **All 9 unit tests passing**
- unit_pose
- it_http
- it_ws
- test_mrd_sink
- unit_http_handlers
- unit_ws_handlers
- unit_mk_mrd
- unit_make_image
- unit_playback

⚠️ **Docker demo test:** NEEDS INVESTIGATION (FPS issues)

---

## Next Agent Tasks

1. **Diagnose viz client FPS drops**
   - Run docker demo and observe behavior
   - Check logs for errors or warnings
   - Profile CPU/memory usage

2. **Verify async writes are working correctly**
   - Check bio.jsonl and poses.jsonl are being written
   - Verify no data loss
   - Check write timing vs read timing

3. **Test under load**
   - Multiple concurrent clients
   - High-frequency data streams
   - Sustained operation (10+ minutes)

4. **Consider fallback to Option 1 only**
   - If Option 2 (async queue) is causing issues
   - Option 1 alone may be sufficient

5. **Document findings**
   - Root cause of FPS drops
   - Recommended solution
   - Performance benchmarks
