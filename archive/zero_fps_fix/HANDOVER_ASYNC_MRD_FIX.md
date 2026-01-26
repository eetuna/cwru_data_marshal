# HANDOVER - Async MRD Frame Fix (Incomplete)

**Date:** 2026-01-25
**Previous Agent:** Attempted to fix Option 2 async write queue FPS issues
**Status:** FIX IMPLEMENTED BUT UNTESTED - Needs X11 display to verify FPS

---

## What Was Done

### Code Changes (on `feature/async-write-queue` branch, commit cc3c2e0)

**1. Updated WriteRequest struct** - `src/marshal_state.hpp`
```cpp
struct WriteRequest {
    // Added fields for MRD frames:
    std::string element_type;    // e.g., "float", "uint16"
    std::string header_xml;      // ISMRMRD header XML
    std::string session_header;  // Session token

    // Changed from vector<float> to vector<uint8_t> for raw bytes
    std::vector<uint8_t> frame_data;
};
```

**2. Implemented MRD_FRAME case in background_writer()** - `src/marshal_main.cpp:123-145`
```cpp
case WriteRequest::Type::MRD_FRAME:
    {
        auto elem_type = mrd::parse_element_type(req.element_type);
        mrd::ImageDimensions dims_obj;
        dims_obj.spatial[0] = req.dims[0];
        dims_obj.spatial[1] = req.dims[1];
        dims_obj.spatial[2] = req.dims[2];
        dims_obj.channels = req.channels;

        state.mrd_sink->append_frame(
            req.stream_name, dims_obj, elem_type,
            req.header_xml, req.frame_data.data(),
            req.frame_data.size(), req.session_header
        );
    }
    break;
```

**3. Updated HTTP handler to queue frames** - `src/marshal_http.hpp:391-429`
- Instead of calling `state.mrd_sink->append_frame()` directly
- Now creates WriteRequest and pushes to write_queue
- Returns optimistic response with `"path": "[queued]"`

---

## What Needs to Be Done

### 1. Test FPS Performance (CRITICAL)

The environment lacks X11 display, so viz_client can't run. You MUST test on a machine with GUI:

```bash
# Ensure build script uses the fixed branch
grep MRI_BRANCH scripts/build-client-images.sh
# Should show: MRI_BRANCH="feature/async-write-queue"

# Build
./scripts/build-client-images.sh

# Run 60-second test
./scripts/demo-docker.sh > /tmp/demo.log 2>&1 &
sleep 55
docker logs cwru-viz-client 2>&1 | grep "FPS DEBUG" | tee /tmp/fps_async_fix.log
docker stop $(docker ps -q --filter "name=cwru") 2>/dev/null
```

**Success criteria:**
- FPS stays above 8 fps consistently
- No drops below 5 fps
- No 0 FPS periods
- Similar to Option 1: 8.9-19.9 fps range

### 2. Potential Issues to Investigate

**If FPS is still bad, check:**

1. **Queue backup** - Is background_writer keeping up?
   ```cpp
   // Add to background_writer loop:
   if (state.write_queue.size() > 10) {
       std::cerr << "[WRITER] Queue backup: " << state.write_queue.size() << " pending\n";
   }
   ```

2. **HTTP response blocking** - The HTTP handler returns before write completes
   - This is intentional for async, but viz client may not handle `"path": "[queued]"` properly
   - Check if viz client needs actual file path in response

3. **SWMR flush timing** - Viz client reads via SWMR, needs data flushed
   - Check if `flushed: true` appears in index.jsonl
   - May need explicit flush after each frame in background_writer

4. **Lock contention on write_queue_mtx** - Still potential bottleneck
   - 4 HTTP threads + 1 writer thread contending on queue mutex
   - Consider lock-free queue (boost::lockfree::queue)

### 3. Alternative: Abandon Async MRD Writes

If async MRD writes don't work, revert to **Option 1 only**:

```bash
# In scripts/build-client-images.sh:
MRI_BRANCH="feature/multi-threaded-io"  # Option 1 only
```

Option 1 gives stable 8.9-19.9 fps. The async queue may not be worth the complexity for MRD frames since:
- They need immediate SWMR flush anyway
- Queuing adds latency without reducing contention

---

## Current Branch State

```
feature/async-write-queue (cc3c2e0)
├── fix: Implement async MRD frame writes (72ae6da)
├── chore: Update build script (1b5b74f → cc3c2e0 amended)
├── Add FPS debug logging (7a82eaa)
└── feat: Add async write queue (dce99e7)
```

**Worktree location:** `.worktrees/mri_data_marshal/`

---

## Key Files

| File | Purpose |
|------|---------|
| `src/marshal_state.hpp` | WriteRequest struct definition |
| `src/marshal_main.cpp:92-150` | background_writer() implementation |
| `src/marshal_http.hpp:387-430` | HTTP image POST handler |
| `src/mrd_sink.cpp` | HDF5 write logic (has heavy locking) |
| `scripts/build-client-images.sh:21` | MRI_BRANCH selection |

---

## Test Data Observed

**Data pipeline works:**
- MRD files created: 23MB in 30 seconds
- Index shows 486+ frames written
- Frames are flushed (`"flushed":true`)

**BUT FPS test showed problems (run 2):**
```
FPS: 22.74, 17.94, 24.74, 22.90, 27.80, 26.90, 22.60
FPS: 6.85   <-- DROP
FPS: 19.74, 35.85, 39.48
FPS: 7.89   <-- DROP
FPS: 0, 0, 0, 0, 0, 0  <-- ZEROES (6 seconds of no frames!)
FPS: 7.90, 38.92, 45.66, 45.41, 33.63
```

The 0 FPS periods are very concerning - suggests either:
- Viz client not running properly (X11 issue in test env)
- Background writer stalling
- Queue overflow

---

## Debugging Commands

```bash
# Check queue size (add logging to background_writer first)
docker logs cwru-mri-marshal 2>&1 | grep "Queue"

# Check for writer errors
docker logs cwru-mri-marshal 2>&1 | grep -i "WRITER\|error"

# Check MRD file growth
watch -n 1 'docker exec cwru-mri-marshal ls -la /session-data/*/mrd/*.mrd'

# Check index write rate
docker exec cwru-mri-marshal tail -f /session-data/*/mrd/index.jsonl
```

---

## Original Problem Reference

See [HANDOVER_FIX_OPTION2.md](HANDOVER_FIX_OPTION2.md) for:
- Root cause analysis (lock contention in mrd_sink)
- Option A/B/C approaches
- Baseline FPS data

---

## Option B Alternative (RECOMMENDED - NOT IMPLEMENTED)

The original handover recommended **Option B** (optimize mrd_sink locks) but I implemented **Option A** (async queue) instead. **Option A didn't work well.** Next agent should try Option B:

**Option B approach:**
1. Revert Option A changes (restore sync writes in HTTP handler)
2. Reduce lock scope in `src/mrd_sink.cpp`
3. Only lock for critical HDF5 metadata operations, not the slow H5Dwrite

### Files to Modify for Option B

**1. Revert marshal_http.hpp (restore sync write)**
```cpp
// src/marshal_http.hpp:391-429
// REVERT TO: Direct call instead of queue
auto result = state.mrd_sink->append_frame(stream_header,
                                          dims,
                                          element_type,
                                          header_xml,
                                          payload,
                                          payload_bytes,
                                          session_header);
```

**2. Optimize MrdFile::append_frame()** - `src/mrd_sink.cpp:315-370`

Current code holds lock for entire function (~50 lines). Split into smaller critical sections:

```cpp
FrameAppendResult MrdFile::append_frame(const void *data, size_t bytes)
{
    const size_t need = frame_bytes_;
    if (bytes != need)
        throw std::runtime_error("frame payload size mismatch");

    hsize_t new_dims[5] = {frames_ + 1, dims_.channels, dims_.spatial[2],
                           dims_.spatial[1], dims_.spatial[0]};

    // CRITICAL SECTION 1: Extend dataset (must be exclusive)
    {
        std::lock_guard<std::mutex> guard(write_mutex_);
        if (H5Dset_extent(dataset_, new_dims) < 0)
            throw std::runtime_error("H5Dset_extent failed");
    }

    // Setup hyperslab selection (read-only on dataset, can be parallel)
    hid_t filespace = H5Dget_space(dataset_);
    if (filespace < 0)
        throw std::runtime_error("H5Dget_space failed");

    hsize_t start[5] = {frames_, 0, 0, 0, 0};
    hsize_t count[5] = {1, dims_.channels, new_dims[2], new_dims[3], new_dims[4]};
    if (H5Sselect_hyperslab(filespace, H5S_SELECT_SET, start, nullptr, count, nullptr) < 0)
    {
        H5Sclose(filespace);
        throw std::runtime_error("H5Sselect_hyperslab failed");
    }

    hid_t memspace = H5Screate_simple(5, count, nullptr);
    if (memspace < 0)
    {
        H5Sclose(filespace);
        throw std::runtime_error("H5Screate_simple (mem) failed");
    }

    // THE SLOW PART: H5Dwrite - SWMR should handle concurrent writes to different regions
    // Each thread writes to its own frame slot (non-overlapping)
    if (H5Dwrite(dataset_, element_hdf_type(type_), memspace, filespace, H5P_DEFAULT, data) < 0)
    {
        H5Sclose(memspace);
        H5Sclose(filespace);
        throw std::runtime_error("H5Dwrite failed");
    }

    H5Sclose(memspace);
    H5Sclose(filespace);

    // CRITICAL SECTION 2: Update frame count and flush
    bool flushed;
    {
        std::lock_guard<std::mutex> guard(write_mutex_);
        frames_++;
        frames_since_flush_++;
        flushed = perform_flush(false);
    }

    FrameAppendResult result;
    result.file_path = path_;
    result.stream_id = stream_id_;
    result.frame_index = frames_ - 1;
    result.bytes = bytes;
    result.element_type = type_;
    result.dims = dims_;
    result.flushed = flushed;
    result.timestamp = iso8601_now_ms();
    return result;
}
```

**3. Also check MrdSink::append_frame()** - `src/mrd_sink.cpp:577-610`

Has another lock at line 588. May need similar optimization.

### Why Option B Should Work Better

| Issue | Option A (async queue) | Option B (lock optimization) |
|-------|------------------------|------------------------------|
| Memory copy | Copies entire frame to queue | No copy |
| Response | Returns "[queued]" placeholder | Returns actual file path |
| SWMR flush | Delayed until background thread | Immediate |
| Lock contention | Moves to queue mutex | Reduces lock scope |
| Complexity | High (queue + thread) | Low (just lock changes) |

### Key Insight

The H5Dwrite to different frame slots should be safe without lock because:
- SWMR mode is enabled
- Each thread writes to a unique frame index (non-overlapping regions)
- Only H5Dset_extent (metadata) needs exclusive access

### Testing Option B

1. Revert HTTP handler to sync writes
2. Apply lock optimization to mrd_sink.cpp
3. Rebuild: `./scripts/build-client-images.sh`
4. Test FPS: `docker logs cwru-viz-client 2>&1 | grep "FPS DEBUG"`
5. Expected: Stable 10-20 FPS like Option 1

---

## Questions for Next Agent

1. Does the fix work when tested with proper X11 display?
2. Are the 0 FPS periods from viz client issues or data pipeline stalls?
3. Should we add queue size monitoring to detect backups?
4. Is lock-free queue needed (Option C from original handover)?
5. Should we try Option B (lock optimization) instead?
6. Should we just use Option 1 and abandon async MRD writes?

Good luck!
