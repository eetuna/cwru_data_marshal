# Solutions to Eliminate Zero-FPS Stalls

## Root Causes Identified

1. **`H5Dset_extent` per frame** - Metadata churn from extending dataset one frame at a time
2. **Synchronous JSON file writes** - Even buffered ofstream can block on OS I/O
3. **`H5Dwrite` blocking** - Can stall if OS page cache is full
4. **WebSocket emit** - Could block if clients are slow

## Solution Options (Ranked by Effectiveness)

### Option 1: Pre-allocate Dataset + Background JSON Queue ⭐ RECOMMENDED

**Changes:**
1. Pre-allocate HDF5 dataset in blocks (e.g., 1000 frames at a time)
2. Track `valid_frames` in a separate scalar dataset
3. Move JSON writes to background thread with lock-free queue
4. Make WebSocket emit non-blocking (fire-and-forget)

**Implementation:**
```cpp
// Pre-allocation (reduces H5Dset_extent calls by 1000x)
hsize_t initial_dims[5] = {1000, channels, z, y, x};  // Pre-allocate 1000 frames
hsize_t max_dims[5] = {H5S_UNLIMITED, channels, z, y, x};
// Write to pre-allocated space, only extend when needed

// Background JSON writer
std::thread json_writer_thread;
std::queue<std::string> json_queue;
std::mutex json_queue_mutex;
std::condition_variable json_queue_cv;

// In append_frame: just enqueue, don't write
{
    std::lock_guard<std::mutex> lock(json_queue_mutex);
    json_queue.push(entry_dump);
}
json_queue_cv.notify_one();
```

**Expected Results:**
- Zero-FPS periods: 0 (eliminated)
- Avg FPS: 50 (at target)
- Latency: <1ms per frame

---

### Option 2: Remove JSON Writes Entirely ⚡ FASTEST

**Changes:**
1. Remove `index.jsonl` writes completely
2. Remove `latest.json` writes completely
3. Serve `/v1/mrd/latest` from in-memory cache only
4. Serve `/v1/mrd/since` by scanning HDF5 file metadata

**Pros:**
- Eliminates ALL file I/O on hot path
- Simplest implementation
- Maximum performance

**Cons:**
- Lose persistent index (must rebuild from HDF5)
- Higher latency for `/v1/mrd/since` queries

**Expected Results:**
- Zero-FPS periods: 0-2 (nearly eliminated)
- Avg FPS: 49+ (near maximum)

---

### Option 3: Dataset Block Pre-extension + Keep JSON ⚠️ PARTIAL

**Changes:**
1. Extend dataset in blocks of 100-1000 frames
2. Track valid_frames separately
3. Keep JSON writes (still a risk)

**Pros:**
- Reduces `H5Dset_extent` calls by 100-1000x
- Keeps JSON index files

**Cons:**
- JSON writes still on hot path (risk of stalls)
- Doesn't address OS I/O blocking

**Expected Results:**
- Zero-FPS periods: 2-4 (reduced but not eliminated)
- Avg FPS: 40-45

---

### Option 4: Async H5Dwrite + Background Flush 🔧 COMPLEX

**Changes:**
1. Buffer frame data in memory
2. Use dedicated thread for HDF5 writes
3. Background flush thread

**Pros:**
- Completely decouples ingestion from storage
- Maximum throughput

**Cons:**
- VERY complex (memory management, backpressure)
- SWMR readers see delayed frames
- Defeats purpose of "realtime" SWMR

**Not Recommended** - Violates SWMR realtime constraint

---

## Recommendation: Implement Option 1

**Why:**
- Addresses ALL blocking sources
- Maintains SWMR realtime visibility
- Preserves JSON index files
- Reasonable complexity

**Implementation Steps:**
1. Add dataset pre-allocation logic to `MrdFile::open()`
2. Add `valid_frames` scalar dataset
3. Create background JSON writer thread in `MarshalState`
4. Replace synchronous writes with queue enqueue
5. Make WebSocket emit non-blocking

**Expected Outcome:**
- Zero-FPS periods: **0** ✅
- Avg FPS: **50** (at target) ✅
- Latency: <1ms per frame ✅

---

## Quick Win: Option 2 (Remove JSON)

If you need immediate results without threading complexity:

```cpp
// In mrd_sink.cpp append_frame, comment out:
// std::ofstream idx_out(sink.index_root / "index.jsonl", std::ios::app);
// if (idx_out) idx_out << entry_dump << '\n';
// std::ofstream latest_out(sink.index_root / "latest.json");
// if (latest_out) latest_out << entry_dump;

// Keep only:
// 1. In-memory latest cache update
// 2. WebSocket emit (if non-blocking)
// 3. H5Dflush
```

This alone should drop zero-FPS to 0-2 periods.
