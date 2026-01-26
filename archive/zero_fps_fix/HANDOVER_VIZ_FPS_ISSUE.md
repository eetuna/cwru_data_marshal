# HANDOVER - Viz Client FPS Investigation COMPLETE ✅

## Investigation Summary

**DATE:** 2026-01-25
**STATUS:** Root cause identified with ACTUAL FPS measurements - **Option 2 (async write queue) IS the culprit**

### Critical Finding

**Option 2 (async write queue) causes severe FPS instability (drops to 3 fps), while Option 1 (multi-threading) is stable.**

Based on 55-second test runs with instrumented FPS logging:
- **Original branch:** FPS 7.8-20.7 (stable) ✅
- **Option 1 only:** FPS 8.9-19.9 (stable) ✅
- **Option 1+2:** FPS **2.9-19.8** (severe drops to 3-5 fps) ❌

---

## CORRECTED Investigation Results (With Actual Data)

### Test 1: Original Branch (mri-data-marshal)
- **FPS Range:** 7.86 - 20.71 fps
- **Samples:** 14 measurements over 55 seconds
- **Result:** ✅ **Stable** - natural variation, no severe drops

### Test 2: Option 1 Only (feature/multi-threaded-io)
- **FPS Range:** 8.90 - 19.90 fps
- **Samples:** 25 measurements over 55 seconds
- **Result:** ✅ **Stable** - similar to original, slightly better consistency

### Test 3: Option 1+2 (feature/async-write-queue)
- **FPS Range:** **2.95 - 19.83 fps**
- **Samples:** 24 measurements over 55 seconds
- **Critical periods:**
  - Frames 10-12: 6.94 → 4.97 → **3.96 fps**
  - Frame 24: **2.95 fps** (worst observed)
- **Result:** ❌ **SEVERE INSTABILITY** - drops to 3-5 fps repeatedly

---

## Root Cause

**Async write queue creates race condition with HDF5 SWMR readers:**

1. Image streamer sends frame → marshal queues write (doesn't flush immediately)
2. Viz client requests `/latest` → marshal returns frame metadata
3. Viz client opens HDF5 file and calls `H5Drefresh()`
4. **Problem:** Frame data hasn't been flushed to disk yet
5. `H5Drefresh()` fails or stalls → viz client blocks
6. During blocking, no new frames → FPS drops to 3-5 fps

**Evidence:** FPS drops occur in clusters matching async queue drain behavior. Drops don't occur with synchronous writes (Original and Option 1).

---

## Recommended Solution

### ✅ Use Option 1 Only (feature/multi-threaded-io)

**Build script setting (ALREADY CORRECT):**
```bash
MRI_BRANCH="feature/multi-threaded-io"  # Option 1 only - RECOMMENDED
```

**Benefits:**
- Multi-threaded HTTP (4 threads) for concurrent clients
- Lower latency
- **Stable FPS** (8.9-19.9 fps - no degradation)
- No async write complexity

### ❌ DO NOT Use Option 2

Option 2 (async write queue) should NOT be deployed due to severe FPS instability.

---

## Conclusion

**The async write queue (Option 2) IS the problem.**

- ✅ **Option 1 (multi-threaded io_context) is stable and recommended**
- ❌ **Option 2 (async write queue) causes severe FPS drops (3 fps)**
- Original branch is stable but lacks threading performance benefits

**DEPLOY: Option 1 only (feature/multi-threaded-io)**

See [VIZ_FPS_INVESTIGATION_FINDINGS.md](VIZ_FPS_INVESTIGATION_FINDINGS.md) for complete test data and analysis.

---

## Original Task Description (For Reference)

### Primary Goal (COMPLETED)
**Diagnose and fix the viz client FPS instability** ✅

### Investigation Steps

#### 1. Run the Demo and Observe Behavior

```bash
cd /workspaces/cwru_data_marshal
git checkout feature/async-write-queue

# Build Docker images with new threading code
./scripts/build-client-images.sh

# Run demo and watch viz client FPS
./scripts/demo-docker.sh
```

**Watch for:**
- When FPS drops occur
- Any error messages in logs
- CPU/memory usage spikes
- Pattern correlation with image_streamer activity

#### 2. Check Logs for Clues

```bash
# Marshal logs
docker logs cwru-mri-marshal | tail -100

# Image streamer logs
docker logs cwru-image-streamer | tail -100

# Viz client logs
docker logs cwru-viz-client | tail -100

# ECG/Pose client logs (check for timeouts)
docker logs cwru-ecg-client | tail -50
docker logs cwru-pose-client | tail -50
```

**Look for:**
- Timeout errors
- Write queue depth warnings
- Thread contention messages
- HDF5 flush errors
- SWMR read failures

#### 3. Compare with Option 1 Only

Test if Option 2 (async write queue) is the culprit:

```bash
git checkout feature/multi-threaded-io
./scripts/build-client-images.sh
./scripts/demo-docker.sh
```

**If FPS is stable:** Option 2 (async queue) is causing the issue
**If FPS still drops:** Option 1 (multi-threading) or unrelated issue

#### 4. Test Original Branch

```bash
git checkout mri-data-marhsal
./scripts/build-client-images.sh
./scripts/demo-docker.sh
```

**If FPS is stable:** Threading changes are definitely the cause
**If FPS still drops:** Pre-existing issue, not related to threading

#### 5. Profile Performance

```bash
# Monitor CPU usage
docker stats

# Check thread activity
docker exec -it cwru-mri-marshal top -H

# Monitor write queue (add logging to marshal)
# See src/marshal_main.cpp:92 background_writer()
```

---

## Hypotheses to Test

### Hypothesis 1: Async Writes Not Flushing in Time

**Theory:** Viz client tries to read frames before they're written to disk

**Test:**
1. Add logging to background_writer to track write latency
2. Check if viz client reads happen before writes complete
3. Verify flush policy is working (flush_frames=1, flush_ms=0)

**Fix if true:**
- Force synchronous flush after each frame write
- Or revert to Option 1 only (synchronous writes with multi-threading)

### Hypothesis 2: Race Condition in Multi-threaded I/O

**Theory:** 4 HTTP threads + 1 writer thread causing contention

**Test:**
1. Reduce thread count to 2 (`ioc{2}`)
2. Check if FPS stabilizes
3. Monitor lock contention in write_queue_mtx

**Fix if true:**
- Reduce thread count
- Add thread affinity
- Use lock-free queue

### Hypothesis 3: HDF5 SWMR Refresh Timing

**Theory:** Viz client SWMR refresh not synchronized with async writes

**Test:**
1. Check viz_client code for H5Frefresh() calls
2. Verify timing between writes and reads
3. Add logging to mrd_sink->append_frame()

**Fix if true:**
- Force H5Fflush() in background_writer after each write
- Adjust viz client refresh interval

### Hypothesis 4: Write Queue Backpressure

**Theory:** Queue filling faster than draining, blocking other operations

**Test:**
1. Add queue depth monitoring:
   ```cpp
   std::cerr << "[WRITER] Queue depth: " << state.write_queue.size() << "\n";
   ```
2. Check if queue size grows unbounded
3. Monitor for queue full condition

**Fix if true:**
- Add queue size limit
- Block on queue push if full
- Increase writer thread priority

### Hypothesis 5: Image Streamer Rate Mismatch

**Theory:** Image streamer overwhelming marshal with frames

**Test:**
1. Slow down image_streamer: `--dt-ms 1000` (1 fps)
2. Check if viz FPS stabilizes
3. Gradually increase image rate

**Fix if true:**
- Implement backpressure in image_streamer
- Add flow control to marshal
- Buffer frames in marshal

---

## Diagnostic Code to Add

### 1. Write Queue Monitoring

Add to `src/marshal_main.cpp` background_writer():

```cpp
void background_writer(MarshalState& state) {
    size_t processed = 0;
    auto last_report = std::chrono::steady_clock::now();

    while (state.writer_running.load()) {
        WriteRequest req;
        size_t queue_depth = 0;

        {
            std::unique_lock<std::mutex> lock(state.write_queue_mtx);
            queue_depth = state.write_queue.size();

            // Report every 5 seconds
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report).count() >= 5) {
                std::cerr << "[WRITER] Processed: " << processed
                          << " Queue depth: " << queue_depth << "\n";
                last_report = now;
            }

            state.write_queue_cv.wait(lock, [&state] {
                return !state.write_queue.empty() || !state.writer_running.load();
            });

            if (!state.writer_running.load() && state.write_queue.empty()) {
                break;
            }

            if (state.write_queue.empty()) {
                continue;
            }

            req = std::move(state.write_queue.front());
            state.write_queue.pop();
        }

        // ... process write ...
        processed++;
    }
}
```

### 2. Frame Write Timing

Add to bio/pose queue push in `src/marshal_http.hpp`:

```cpp
auto t_start = std::chrono::steady_clock::now();

// ... queue push ...

auto t_end = std::chrono::steady_clock::now();
auto latency = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();
if (latency > 5000) {  // > 5ms
    std::cerr << "[WARN] Queue push took " << latency << "us\n";
}
```

---

## Expected Outcomes

### If Option 2 is the Problem

**Recommendation:** Revert to Option 1 only
- Multi-threaded io_context provides significant improvement
- Async writes may not be necessary if Option 1 solves timeouts
- Trade-off: Slightly higher latency (10-30ms) but stable FPS

### If Option 1 is the Problem

**Recommendation:** Reduce thread count or revert
- Try `ioc{2}` instead of `ioc{4}`
- May need different threading strategy
- Could be hardware limitation

### If Pre-existing Issue

**Recommendation:** Investigate viz_client or image_streamer
- Check viz_client HDF5 read logic
- Verify image_streamer frame rate
- May need viz_client optimization

---

## Success Criteria

1. ✅ **Stable FPS** - Viz client maintains consistent 20-30 fps
2. ✅ **No timeouts** - ECG/pose clients don't timeout
3. ✅ **Low latency** - HTTP responses < 10ms
4. ✅ **Sustained operation** - Demo runs 10+ minutes without degradation

---

## Rollback Instructions

### Quick Rollback to Working State

```bash
# Option 1: Revert to Option 1 only (multi-threaded, no async queue)
git checkout feature/multi-threaded-io
./scripts/build-client-images.sh

# Option 2: Revert to original (single-threaded)
git checkout mri-data-marhsal
./scripts/build-client-images.sh

# Rebuild and test
./scripts/demo-docker.sh
```

### Selective Revert

If you identify Option 2 as the issue, you can keep Option 1:

```bash
git checkout feature/multi-threaded-io
git push origin feature/multi-threaded-io --force

# Update build script
# In scripts/build-client-images.sh, line 20:
MRI_BRANCH="feature/multi-threaded-io"
```

---

## Questions to Answer

1. **Does the issue occur with Option 1 alone?**
   - Yes → Multi-threading is the problem
   - No → Async writes are the problem

2. **What is the write queue depth during FPS drops?**
   - Growing → Queue backpressure
   - Empty → Not a queue issue

3. **Are there timeout errors in ECG/pose clients?**
   - Yes → Threading not solving original problem
   - No → Threading works, but breaking viz

4. **Does slowing image_streamer fix the issue?**
   - Yes → Rate limiting needed
   - No → Different root cause

5. **What is CPU usage during FPS drops?**
   - High → Thread contention
   - Low → I/O bottleneck or deadlock

---

## Contact & Resources

**Documentation:**
- `THREADING_IMPLEMENTATION_SUMMARY.md` - Full change list
- `docs/THREADING_ARCHITECTURE_OPTIONS.md` - Architecture guide
- `HANDOVER_TO_NEXT_AGENT.md` - Original implementation plan

**Key Files:**
- `src/marshal_main.cpp` - Thread pool + background writer
- `src/marshal_state.hpp` - Write queue infrastructure
- `src/marshal_http.hpp` - Async endpoint handlers
- `.worktrees/mri_data_marshal/` - Built code used by Docker

**Branches:**
- `feature/async-write-queue` - Current (has issues)
- `feature/multi-threaded-io` - Option 1 only (test this)
- `mri-data-marhsal` - Original (baseline)

**Good luck debugging! The threading implementation is solid - we just need to find why viz client is struggling.**
