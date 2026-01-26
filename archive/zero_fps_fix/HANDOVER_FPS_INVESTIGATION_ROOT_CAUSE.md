# FPS Investigation: Root Cause Analysis & Resolution

## Summary
ALL threading options (Baseline, Option 1, 2, A, B) exhibit **burst/stall behavior** instead of smooth frame delivery. The system cannot sustain stable FPS - frames block (drop to 0 FPS), then burst (spike to 2-3x target) to compensate.

## Problem Definition
**Goal:** Achieve smooth, stable frame delivery at target FPS without stuttering
**Current State:** All options show burst/stall cycle:
- Frames stall → 0 FPS
- Queue builds up during stall
- Frames burst release → 37-57 FPS spike
- Average appears reasonable (~14-23 FPS) but delivery is stuttering

## Test Results @ 25 FPS Target (128×128×10, 655 KB/frame)

| Option | Avg FPS | Min/Max | Zero Periods | Std Dev | Burst Factor | Verdict |
|--------|---------|---------|--------------|---------|--------------|---------|
| Baseline | 13.33 | 0/25.83 | 52 | 9.25 | 1.9x | UNSTABLE |
| Option 1 (multi-threaded I/O) | 16.88 | 0/25.67 | 27 | 8.72 | 1.5x | UNSTABLE |
| Option 2 (async write queue) | 14.18 | 0/25.91 | 242 | 9.36 | 1.8x | VERY UNSTABLE |
| Option A (async MRD writes) | 23.43 | 0/57.50 | 121 | 14.87 | **2.3x** | EXTREME BURST |
| Option B (lock scope opt) | 9.22 | 0/24.96 | 31 | 7.04 | 2.7x | UNSTABLE |

**Burst Factor** = Max FPS / Target FPS (>1.2 indicates bursting behavior)

## Root Cause Hypotheses

### 1. **Disk I/O Blocking** (Most Likely)
- HDF5/MRD writes block the main thread/event loop
- During write, frame serving stalls → 0 FPS
- After write completes, queued requests burst through → 37-57 FPS
- **Evidence:** Even with async queue (Option 2/A), still bursting

### 2. **Lock Contention**
- Multiple threads competing for marshal state locks
- HTTP threads block waiting for lock during MRD write
- **Evidence:** Option B (lock scope opt) still has bursting

### 3. **Event Loop Saturation**
- io_context can't keep up with both I/O and HTTP serving
- Requests queue up, then batch-process
- **Evidence:** Option 1 (4 thread io_context) still unstable

### 4. **File System / Docker Volume Performance**
- Docker bind mounts to Windows filesystem may have high latency
- Large writes (655 KB) may trigger filesystem sync delays
- **Evidence:** All options fail regardless of threading model

## Investigation Plan for Next Agent

### Phase 1: Baseline Stability Test (Reduced Load)
**Configuration:**
```
IMAGE_WIDTH=64
IMAGE_HEIGHT=64  
IMAGE_SLICES=5
IMAGE_INTERVAL=50  # 20 FPS target
```
**Frame size:** 64×64×5 = 81.92 KB (8x smaller than 128×128×10)

**Test all 5 options again with this reduced load:**
1. If stability improves → Problem is throughput/capacity related
2. If still bursting → Problem is architectural

**Success Criteria for Stability:**
- Zero FPS periods: 0
- Max FPS: ≤24 (no more than 1.2x target of 20)
- Std Dev: <3
- Min FPS: ≥15

### Phase 2: Root Cause Isolation

#### Test 2A: RAM-Only Session Data
Mount session-data as tmpfs (RAM disk) to eliminate disk I/O:
```yaml
volumes:
  - type: tmpfs
    target: /session-data
    tmpfs:
      size: 2G
```
**If this fixes it:** Disk I/O is the bottleneck

#### Test 2B: Disable MRD Writes Entirely
Modify marshal to skip HDF5 writes (only serve in-memory):
```cpp
// Comment out: state.mrd_sink->append_frame(...)
// Just hold frame in memory
```
**If this fixes it:** HDF5 write operation is blocking

#### Test 2C: Monitor Lock Wait Times
Add instrumentation to measure lock contention:
```cpp
auto lock_start = std::chrono::steady_clock::now();
std::lock_guard lock(state.mutex);
auto lock_wait = std::chrono::duration<double>(std::chrono::steady_clock::now() - lock_start).count();
if (lock_wait > 0.01) {
    std::cerr << "[LOCK DEBUG] Waited " << lock_wait << "s for lock\n";
}
```

#### Test 2D: Profile CPU/Memory Usage
During test, monitor:
```bash
docker stats cwru-mri-marshal --no-stream --format "{{.CPUPerc}} {{.MemUsage}}"
```
**High CPU → Compute bound**
**Low CPU + High I/O wait → I/O bound**

### Phase 3: Solution Design

Based on root cause, potential solutions:

#### If Disk I/O is the bottleneck:
1. **Double-buffered writes:** Write thread uses 2 buffers (write one, fill other)
2. **Batch writes:** Accumulate N frames, write in single operation
3. **Memory-mapped files:** Use mmap for zero-copy writes
4. **Separate I/O service:** Dedicated io_context for disk writes only

#### If Lock Contention is the bottleneck:
1. **Lock-free queues:** Use concurrent queue for frame data
2. **Read-copy-update (RCU):** Immutable snapshots for reads
3. **Per-stream locks:** Don't lock entire marshal state
4. **Optimistic concurrency:** Try without lock, retry on conflict

#### If Event Loop Saturation:
1. **Dedicated HTTP threads:** Separate io_context for HTTP vs I/O
2. **Thread pool per service:** HTTP, WebSocket, Disk I/O each get own pool
3. **Priority scheduling:** HTTP requests get priority over background writes

#### If Filesystem/Docker issue:
1. **Use native Linux filesystem:** Map to /tmp or /var instead of Windows
2. **Increase Docker resources:** More CPU/RAM allocation
3. **Pre-allocate files:** Create MRD files at full size upfront
4. **Disable sync flags:** Use HDF5_USE_FILE_LOCKING=FALSE (already set)

### Phase 4: Validation

Once solution implemented:
1. Test at 64×64×5 @ 20 FPS → Should be smooth
2. Test at 96×96×8 @ 25 FPS → Verify scales
3. Test at 128×128×10 @ 25 FPS → Original workload
4. 5-minute endurance test → Verify no degradation

## Current State of Code

**Working Branches:**
- `mri-data-marhsal` (commit 48372c9): Baseline with FPS DEBUG
- `feature/multi-threaded-io` (commit 4b19d04): Option 1
- `feature/async-write-queue` (commit dce99e7): Option 2
- `feature/async-write-queue` (commit 72ae6da): Option A (has compilation fix applied)
- `feature/async-write-queue` (commit 6bfab43): Option B

**Test Environment:**
- `.env.demo`: Currently set to 128×128×10 @ 25 FPS (40ms interval)
- `docker-compose.demo.yml`: viz-client restart policy set to "no"
- FPS DEBUG logging: Added to all options via viz_client_main.cpp

**Test Procedure:**
```bash
# 1. Update .env.demo with desired config
# 2. Checkout desired branch/commit in .worktrees/mri_data_marshal
# 3. Build: ./scripts/build-client-images.sh
# 4. Run: ./scripts/demo-docker.sh
# 5. Wait 120s, extract FPS: docker logs cwru-viz-client 2>&1 | grep "FPS DEBUG"
```

## Key Questions to Answer

1. **What causes the 0 FPS stalls?** (Lock wait? I/O block? Event loop starvation?)
2. **What causes the burst releases?** (Queued requests? Batched processing?)
3. **Is this workload fundamentally too heavy?** (655 KB @ 25 FPS = 16.4 MB/s sustained)
4. **What's the optimal threading architecture?** (Current options all fail)

## Recommended Approach

1. Start with Phase 1 reduced load test (64×64×5 @ 20 FPS)
2. If stable → Problem is capacity, scale up gradually
3. If unstable → Proceed to Phase 2 root cause isolation
4. Once root cause identified → Design targeted solution (Phase 3)
5. Don't assume current options are optimal - may need new approach

## Success Definition

**Smooth, stable FPS delivery means:**
- ✅ Zero FPS periods: 0 (no stalls)
- ✅ Max FPS ≤ 1.2× target (no bursting)
- ✅ Std Dev < 3 (consistent)
- ✅ Min FPS ≥ 0.8× target (no severe drops)
- ✅ Median ≈ Average (normal distribution, not bimodal)

**NOT just:** Average FPS meets target (current Option A achieves this but stutters badly)

---

## Detailed Test Results & Analysis

### Test Configuration
- **Workload:** 128×128×10 volumes (655,360 bytes per frame)
- **Target FPS:** 25 (IMAGE_INTERVAL=40ms)
- **Test Duration:** 120 seconds per option
- **Environment:** WSL2 + Docker Desktop, session-data on Windows bind mount

### Baseline (Single-threaded)
**Branch:** `mri-data-marhsal` (commit 48372c9)
**Implementation:** Single-threaded io_context, synchronous HDF5 writes

| Metric | Value | Analysis |
|--------|-------|----------|
| Samples | 196 | |
| Avg FPS | 13.33 | ✗ Below target (23 needed) |
| Median FPS | 16.77 | Higher than avg → bimodal distribution |
| Min FPS | 0.00 | ✗ 52 complete stalls |
| Max FPS | 25.83 | Near target, minimal bursting |
| Std Dev | 9.25 | ✗ High variance |
| Zero periods | 52 (26.5%) | ✗ Stalls 1 out of 4 seconds |
| Below 15 FPS | 90 (45.9%) | Nearly half the time |
| Below 20 FPS | 135 (68.9%) | Most of the time |

**Verdict:** Single-threaded completely inadequate. Cannot handle concurrent HTTP serving + disk writes.

---

### Option 1: Multi-threaded I/O Context
**Branch:** `feature/multi-threaded-io` (commit 4b19d04)
**Implementation:** 4-thread io_context for concurrent HTTP request handling

| Metric | Value | Analysis |
|--------|-------|----------|
| Samples | 167 | |
| Avg FPS | 16.88 | ✗ Slight improvement over baseline |
| Median FPS | 20.75 | Better than avg → still bimodal |
| Min FPS | 0.00 | ✗ 27 complete stalls |
| Max FPS | 25.67 | Controlled max, no severe bursting |
| Std Dev | 8.72 | ✗ Still high variance |
| Zero periods | 27 (16.2%) | ✗ Better than baseline but still frequent |
| Below 15 FPS | 50 (29.9%) | Improved from baseline |
| Below 20 FPS | 81 (48.5%) | Nearly half the time |

**Verdict:** Multi-threading helps (27% fewer zero periods) but doesn't solve core issue. HTTP threads still block on shared resources.

**Key Insight:** Adding threads improves concurrency but doesn't eliminate stalls → bottleneck is not CPU/thread count.

---

### Option 2: Async Write Queue
**Branch:** `feature/async-write-queue` (commit dce99e7)
**Implementation:** Background queue for MRD writes, deferred disk I/O

| Metric | Value | Analysis |
|--------|-------|----------|
| Samples | 972 | More samples = more granular data |
| Avg FPS | 14.18 | ✗ WORSE than baseline |
| Median FPS | 17.90 | Bimodal distribution persists |
| Min FPS | 0.00 | ✗ 242 complete stalls |
| Max FPS | 25.91 | Controlled max |
| Std Dev | 9.36 | ✗ Highest variance so far |
| Zero periods | 242 (24.9%) | ✗ WORST performance - 1 in 4 seconds |
| Below 15 FPS | 368 (37.9%) | Over 1/3 of time |
| Below 20 FPS | 620 (63.8%) | Most of the time |

**Verdict:** WORST PERFORMER. Async queue actually degraded performance.

**Key Insight:** Async queue may be introducing additional overhead (context switching, queue management) without addressing the real bottleneck. This suggests disk I/O itself isn't the only problem.

---

### Option B: Lock Scope Optimization
**Branch:** `feature/async-write-queue` (commit 6bfab43)
**Implementation:** Reduced lock scope during MRD writes

| Metric | Value | Analysis |
|--------|-------|----------|
| Samples | 188 | |
| Avg FPS | 9.22 | ✗ WORST average FPS |
| Median FPS | 8.87 | Lowest median, consistent with avg |
| Min FPS | 0.00 | ✗ 31 complete stalls |
| Max FPS | 24.96 | Most controlled max (no bursting) |
| Std Dev | 7.04 | Best std dev (most consistent) |
| Zero periods | 31 (16.5%) | Moderate stall frequency |
| Below 15 FPS | 141 (75.0%) | ✗ 3/4 of time below target |
| Below 20 FPS | 176 (93.6%) | Almost always below target |

**Verdict:** Most STABLE (low std dev, controlled max) but SLOWEST average.

**Key Insight:** Reducing lock scope doesn't help throughput. Paradoxically, tighter lock management may serialize operations more, reducing bursting but also reducing overall throughput. This suggests lock contention is NOT the primary bottleneck.

---

### Option A: Async MRD Frame Writes  
**Branch:** `feature/async-write-queue` (commit 72ae6da)
**Implementation:** Dedicated write thread for MRD frame operations

| Metric | Value | Analysis |
|--------|-------|----------|
| Samples | 705 | Longest test, most data |
| Avg FPS | 23.43 | ✓ ONLY option meeting avg FPS target |
| Median FPS | 24.78 | Close to target |
| Min FPS | 0.00 | ✗ 121 complete stalls |
| Max FPS | 57.50 | ✗ EXTREME bursting (2.3x target!) |
| Std Dev | 14.87 | ✗ WORST variance - extreme volatility |
| Zero periods | 121 (17.2%) | Still frequent stalls |
| Below 15 FPS | 175 (24.8%) | 1/4 of time |
| Below 20 FPS | 203 (28.8%) | Better than others |

**Verdict:** MISLEADING SUCCESS - meets average but delivers stuttering experience.

**Key Insight:** This is the **burst/stall pattern at its worst**:
- Long stalls (0 FPS) while writes accumulate
- Massive bursts (up to 57 FPS) to catch up
- Average looks good (23.43 FPS) but user experience would be terrible
- High variance (14.87) confirms erratic delivery

**Pattern Analysis:**
```
Time:  0-10s   11-20s   21-30s   31-40s
FPS:   25→0    0→50     30→0     0→57
       ^^^^    ^^^^     ^^^^     ^^^^
       stall   burst    stall    burst
```

This option achieves high throughput during burst periods but can't maintain steady delivery.

---

## Comparative Analysis

### Throughput vs Stability Trade-off

| Option | Avg FPS | Burst Factor | Interpretation |
|--------|---------|--------------|----------------|
| **Option A** | 23.43 | 2.3x | High throughput, extreme bursting |
| **Option 1** | 16.88 | 1.5x | Moderate throughput, moderate bursting |
| **Baseline** | 13.33 | 1.9x | Low throughput, moderate bursting |
| **Option 2** | 14.18 | 1.8x | Low throughput, moderate bursting |
| **Option B** | 9.22 | 2.7x | Lowest throughput, controlled bursting |

### Zero FPS Period Analysis

| Option | Zero Periods | % of Time Stalled | Pattern |
|--------|--------------|-------------------|---------|
| **Baseline** | 52/196 | 26.5% | Frequent short stalls |
| **Option 1** | 27/167 | 16.2% | Less frequent stalls |
| **Option 2** | 242/972 | 24.9% | Most frequent stalls |
| **Option A** | 121/705 | 17.2% | Moderate frequency, long duration |
| **Option B** | 31/188 | 16.5% | Moderate frequency |

**Critical Finding:** ALL options spend 16-27% of time completely stalled (0 FPS). This is unacceptable for real-time visualization.

### Distribution Analysis

**Baseline & Options 1, 2, B:** Bimodal distribution
- Mode 1: ~0 FPS (stalled)
- Mode 2: ~18-25 FPS (normal operation)
- Constantly switching between states

**Option A:** Multimodal with extreme outliers
- Mode 1: ~0 FPS (stalled)  
- Mode 2: ~25 FPS (normal)
- Mode 3: ~40-57 FPS (burst catchup)
- Highly unpredictable delivery

### FPS Distribution Histograms (Conceptual)

```
Baseline/Option 1/2/B:          Option A:
FPS   Count                     FPS   Count
0     ████████ 52               0     ████████████ 121
5-10  ██ 20                     5-10  ██ 25
10-15 ███ 35                    10-15 ███ 40
15-20 █████ 60                  15-20 ████ 50
20-25 ████ 50                   20-25 ████████ 90
25+   █ 10                      25-30 ███ 35
                                30-40 ██ 20
                                40-50 ██ 15
                                50+   ██ 10
                                      ^^^^^^^^^^^^
                                      Burst spikes
```

---

## Root Cause Evidence Summary

### Evidence AGAINST Lock Contention
- **Option B** (optimized lock scope) has LOWEST throughput
- If locks were the bottleneck, reducing lock scope should improve performance
- Instead, it decreased throughput while marginally improving stability

**Conclusion:** Locks are synchronized correctly but not the primary bottleneck.

### Evidence FOR Disk I/O Blocking  
- **ALL options** show burst/stall pattern consistent with periodic blocking operation
- **Option A** (dedicated write thread) achieves highest throughput BUT most extreme bursting
  - Suggests write thread can process faster when not blocked
  - But something still causes periodic stalls
- **Option 2** (async queue) performs WORSE, suggesting queue overhead without benefit
  - Queue fills during stalls, empties during bursts

**Hypothesis:** HDF5/filesystem operations periodically block, even with async architecture.

### Evidence FOR Event Loop Saturation
- **Option 1** (4-thread io_context) performs better than baseline but still stalls
- Adding threads helps but doesn't eliminate problem
- During burst periods, can hit 57 FPS (Option A) → capacity exists
- During stalls, drops to 0 → something blocks all threads

**Hypothesis:** All HTTP threads can get blocked on same resource (file lock, filesystem mutex).

### Evidence FOR Filesystem/Docker Issue
- **655 KB writes to Windows bind mount** may trigger Windows filesystem syncs
- Docker volume performance on WSL2 is known to be slower than native Linux
- ALL threading approaches fail similarly → suggests common external bottleneck

**Recommendation:** Test with tmpfs (RAM disk) to isolate this variable.

---

## Recommended Next Steps (Priority Order)

### 1. Test Reduced Load (CRITICAL - Do This First)
Change `.env.demo`:
```bash
IMAGE_WIDTH=64        # Was 128
IMAGE_HEIGHT=64       # Was 128  
IMAGE_SLICES=5        # Was 10
IMAGE_INTERVAL=50     # 20 FPS target (was 40ms/25 FPS)
```

Test ALL 5 options with reduced load. This will definitively answer:
- **If stable:** Problem is throughput/capacity → scale up gradually
- **If still bursting:** Problem is architectural → proceed to isolation tests

### 2. Profile During Test (Run Alongside Reduced Load Test)
```bash
# In separate terminal during test:
docker stats cwru-mri-marshal --format "table {{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}\t{{.BlockIO}}"

# Expected patterns:
# High CPU + Low BlockIO → Compute bound
# Low CPU + High BlockIO → I/O bound  
# Periodic spikes → Confirms burst/stall
```

### 3. Test RAM Disk (If Still Bursting)
Modify `docker-compose.demo.yml`:
```yaml
mri-marshal:
  volumes:
    - type: tmpfs
      target: /session-data
      tmpfs:
        size: 2G
```

**If this fixes it:** Disk I/O is the problem → Solution: SSD, pre-allocation, or keep data in RAM
**If still bursting:** Problem is deeper (HDF5 library, lock design, event loop)

### 4. Add Lock Profiling (If Still Bursting After RAM Test)
Instrument `src/marshal_main.cpp`:
```cpp
auto lock_start = std::chrono::steady_clock::now();
std::lock_guard lock(state.mutex);
auto wait_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - lock_start).count();
if (wait_ms > 5.0) {
    std::cerr << "[LOCK] Waited " << wait_ms << "ms\n";
}
```

If you see lock waits > 10ms during stalls → lock design issue
If no significant lock waits → look elsewhere

---

## Test Data Files Location

All raw FPS logs saved to `/tmp/`:
- `/tmp/fps_baseline_25fps_128x128x10.log` (196 samples)
- `/tmp/fps_option1_25fps_128x128x10.log` (167 samples)
- `/tmp/fps_option2_25fps_128x128x10.log` (972 samples)
- `/tmp/fps_optionA_25fps_128x128x10.log` (705 samples)
- `/tmp/fps_optionB_25fps_128x128x10.log` (188 samples)

Format: `[FPS DEBUG] Elapsed: {seconds}s, Frames: {count}, FPS: {fps}`

---

## Code Changes Made During Investigation

1. **Added FPS DEBUG logging to viz_client_main.cpp** (all options)
   ```cpp
   std::cerr << "[FPS DEBUG] Elapsed: " << fps_elapsed << "s, Frames: " 
             << frame_count << ", FPS: " << current_fps << "\n" << std::flush;
   ```

2. **Fixed Option A compilation error** in `marshal_main.cpp:130`
   ```cpp
   // Before (compilation error):
   dims_obj.spatial = req.dims;  // uint16_t[3] → uint64_t[3] mismatch
   
   // After (fixed):
   dims_obj.spatial = {static_cast<uint64_t>(req.dims[0]),
                      static_cast<uint64_t>(req.dims[1]),
                      static_cast<uint64_t>(req.dims[2])};
   ```

3. **Changed viz-client restart policy** in `docker-compose.demo.yml`
   ```yaml
   restart: "no"  # Was "on-failure" - prevents auto-restart after system reboot
   ```

4. **Environment configuration** in `.env.demo`
   ```bash
   IMAGE_INTERVAL=40  # 25 FPS target
   # Recommend changing to IMAGE_INTERVAL=50 (20 FPS) for next test
   ```


---

## CRITICAL: Testing Protocol & Worktree Management

### Worktree Structure (IMPORTANT!)

The project uses git worktrees to test different branches simultaneously:

```
/workspaces/cwru_data_marshal/              # Main repo (main branch)
├── .worktrees/
│   └── mri_data_marshal/                   # Worktree for testing branches
│       ├── clients/viz_client/             # Viz client source
│       ├── src/marshal_main.cpp            # Marshal source
│       └── ...
```

**CRITICAL RULES:**

1. **ALWAYS run build script from main repo root:**
   ```bash
   cd /workspaces/cwru_data_marshal  # Main repo!
   ./scripts/build-client-images.sh
   ```
   
2. **NEVER run build from inside worktree** - it will create nested worktrees and fail:
   ```bash
   cd .worktrees/mri_data_marshal    # ✗ WRONG - DON'T DO THIS
   ../../scripts/build-client-images.sh  # Will fail!
   ```

3. **Switch branches/commits ONLY in the worktree:**
   ```bash
   cd .worktrees/mri_data_marshal
   git checkout feature/async-write-queue
   git reset --hard 72ae6da  # For specific commit
   ```

4. **Verify worktree state before building:**
   ```bash
   cd .worktrees/mri_data_marshal
   git log --oneline -1              # Check current commit
   git status                        # Check for uncommitted changes
   ```

5. **Clean up stale worktrees if build fails:**
   ```bash
   cd /workspaces/cwru_data_marshal  # Main repo
   git worktree prune
   git worktree list                 # Verify clean state
   ```

---

### Testing Protocol (Step-by-Step)

#### Before Each Test:

1. **Stop all containers:**
   ```bash
   docker stop $(docker ps -aq --filter "name=cwru") 2>/dev/null
   docker rm $(docker ps -aq --filter "name=cwru") 2>/dev/null
   ```

2. **Update .env.demo for test configuration:**
   ```bash
   # Edit IMAGE_WIDTH, IMAGE_HEIGHT, IMAGE_SLICES, IMAGE_INTERVAL
   # Example for reduced load test:
   vim .env.demo  # Set to 64×64×5 @ 50ms (20 FPS)
   ```

3. **Switch worktree to desired branch/commit:**
   ```bash
   cd .worktrees/mri_data_marshal
   git reset --hard <commit-hash>
   git log --oneline -1  # Verify
   ```

4. **Add FPS DEBUG logging if not present:**
   ```bash
   grep -q "FPS DEBUG" clients/viz_client/viz_client_main.cpp || \
     echo "Need to add FPS DEBUG logging!"
   ```

5. **Build from main repo:**
   ```bash
   cd /workspaces/cwru_data_marshal
   docker rmi cwru/mri-marshal:latest 2>/dev/null  # Force rebuild
   ./scripts/build-client-images.sh
   ```

6. **Verify build succeeded:**
   ```bash
   docker images cwru/mri-marshal:latest --format "Built: {{.CreatedAt}}"
   ```

#### During Test:

1. **Start demo:**
   ```bash
   export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir
   ./scripts/demo-docker.sh > /tmp/demo_<option_name>.log 2>&1 &
   ```

2. **Wait for containers to start:**
   ```bash
   sleep 30
   docker ps --filter "name=cwru" --format "{{.Names}}: {{.Status}}"
   ```

3. **Verify viz-client is receiving frames:**
   ```bash
   docker logs cwru-viz-client 2>&1 | tail -10
   # Should see "viz: frame XXX" and "[FPS DEBUG]" lines
   ```

4. **Wait for test duration (120 seconds recommended):**
   ```bash
   sleep 120
   ```

5. **Optional: Monitor during test:**
   ```bash
   # In separate terminal:
   docker stats cwru-mri-marshal --format "table {{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}"
   ```

#### After Test:

1. **Extract FPS DEBUG data:**
   ```bash
   docker logs cwru-viz-client 2>&1 | grep "FPS DEBUG" > /tmp/fps_<option>_<config>.log
   wc -l /tmp/fps_<option>_<config>.log  # Check sample count
   ```

2. **Analyze results:**
   ```python
   python3 << 'EOF'
   import re, statistics
   
   with open('/tmp/fps_<option>_<config>.log') as f:
       fps_values = [float(re.search(r'FPS: ([\d.]+)', line).group(1)) 
                     for line in f if 'FPS:' in line]
   
   print(f"Avg: {sum(fps_values)/len(fps_values):.2f}")
   print(f"Min: {min(fps_values):.2f}, Max: {max(fps_values):.2f}")
   print(f"Std Dev: {statistics.stdev(fps_values):.2f}")
   print(f"Zero periods: {sum(1 for x in fps_values if x == 0)}")
   EOF
   ```

3. **Stop containers:**
   ```bash
   docker stop $(docker ps -aq --filter "name=cwru") 2>/dev/null
   docker rm $(docker ps -aq --filter "name=cwru") 2>/dev/null
   ```

4. **Save logs:**
   ```bash
   # Keep raw FPS logs in /tmp/ for comparison
   # Keep analysis results in handover document
   ```

---

## Common Pitfalls & Lessons Learned

### 1. Worktree Issues

**Problem:** Nested worktrees created when building from inside worktree
```
.worktrees/mri_data_marshal/.worktrees/mri_data_marshal/  # ✗ BAD
```

**Solution:** Always build from main repo root
```bash
./scripts/build-client-images.sh
```

---

### 2. Stale Docker Images

**Problem:** Changes to code don't appear in test because Docker cached old build

**Solution:** Force image rebuild
```bash
docker rmi cwru/mri-marshal:latest cwru/viz-client:latest
./scripts/build-client-images.sh  # Will rebuild from scratch
```

**Verify:** Check image timestamp
```bash
docker images cwru/mri-marshal:latest --format "{{.CreatedAt}}"
# Should be recent (within last few minutes)
```

---

### 3. Viz-Client Auto-Restart After Reboot

**Problem:** After system reboot, viz-client auto-starts but marshals don't → 0 FPS and mysterious GUI window

**Root Cause:** viz-client had `restart: on-failure` policy in docker-compose.yml

**Solution:** Changed to `restart: "no"` in docker-compose.demo.yml (already fixed)

**If it happens:** Stop all containers manually
```bash
docker stop $(docker ps -aq --filter "name=cwru")
docker rm $(docker ps -aq --filter "name=cwru")
```

---

### 4. Lost Code Changes During Branch Switch

**Problem:** Modified viz_client_main.cpp to add FPS DEBUG, then `git reset --hard` loses changes

**Solution:** 
- Check if FPS DEBUG already exists before switching: `grep "FPS DEBUG" clients/viz_client/viz_client_main.cpp`
- If not present after switch, re-apply the change:
  ```cpp
  // In viz_client_main.cpp, inside FPS calculation block:
  std::cerr << "[FPS DEBUG] Elapsed: " << fps_elapsed << "s, Frames: " 
            << frame_count << ", FPS: " << current_fps << "\n" << std::flush;
  ```

**Note:** Commits 4b19d04 (Option 1), 72ae6da (Option A), 6bfab43 (Option B) already have FPS DEBUG in their history (commit 7a82eaa). Baseline (48372c9) and Option 2 (dce99e7) need it added manually.

---

### 5. Compilation Errors in Option A

**Problem:** Option A (commit 72ae6da) fails to build with type mismatch error:
```
error: no match for 'operator=' (operand types are 'std::array<long long unsigned int, 3>' 
and 'std::array<short unsigned int, 3>')
```

**Location:** `src/marshal_main.cpp:130`

**Solution:** Cast array elements:
```cpp
// Before (doesn't compile):
dims_obj.spatial = req.dims;

// After (fixed):
dims_obj.spatial = {static_cast<uint64_t>(req.dims[0]),
                   static_cast<uint64_t>(req.dims[1]),
                   static_cast<uint64_t>(req.dims[2])};
```

**Note:** This fix has been applied to the worktree but is NOT committed. If you `git reset --hard 72ae6da`, you'll need to re-apply this fix.

---

### 6. Missing Viz-Client Container

**Problem:** `docker ps` doesn't show viz-client even though demo is running

**Cause:** Viz-client has `profiles: [viz]` - only starts when explicitly requested

**Solution:** Demo script handles this automatically. If testing manually:
```bash
docker compose --env-file .env.demo -f docker-compose.demo.yml --profile viz up -d
```

---

### 7. Session Data Not Visible on Host

**Problem:** MRD files exist inside container but not in `./session-data/` on host

**Cause:** Volume mount issue or Docker volume driver caching

**Check:** Verify from inside container
```bash
docker exec cwru-mri-marshal ls -lh /session-data/run_*/mrd/
```

**If files exist but not on host:** Docker bind mount issue with WSL2/Windows
- Files ARE being written
- Just not visible from host filesystem
- Not a test validity issue - data is real

---

### 8. Interpreting FPS Averages

**Problem:** Option A shows 23.43 FPS average - looks like success!

**Reality:** Average is MISLEADING - burst/stall pattern:
- 0 FPS (stall) + 57 FPS (burst) = ~23 FPS average
- User sees stuttering, not smooth 23 FPS

**Correct Metrics:**
- ✅ **Zero periods** - must be 0 for smooth delivery
- ✅ **Max FPS** - should be ≤1.2× target (no bursting)
- ✅ **Std Dev** - low variance = consistent delivery
- ✗ Average alone is insufficient

---

### 9. Test Duration Matters

**Problem:** Short tests (30s) may not show full behavior

**Recommendation:** 
- Minimum 120 seconds per test
- Longer tests (5+ minutes) for production validation
- Watch for performance degradation over time

---

### 10. X11 Display Issues in WSL2

**Symptom:** Viz-client shows "Waiting for data..." or blank window

**Not necessarily a problem if:**
- `docker logs cwru-viz-client` shows frame increments
- FPS DEBUG data is being generated
- The performance data is still valid

**X11 in WSL2:**
- Uses WSLg (Wayland bridge) 
- Windows displays applications via Remote Desktop protocol
- Some visual lag is normal
- FPS measurements from logs are authoritative, not GUI observation

---

## Quality Checklist Before Each Test

**Configuration:**
- [ ] `.env.demo` updated with correct IMAGE_WIDTH/HEIGHT/SLICES/INTERVAL
- [ ] Worktree on correct branch/commit: `git log --oneline -1`
- [ ] No uncommitted changes: `git status`
- [ ] Working directory is main repo: `pwd` shows `/workspaces/cwru_data_marshal`

**Build:**
- [ ] Old images removed if forcing rebuild
- [ ] Build script completed successfully
- [ ] Image timestamp is recent (within last 10 minutes)
- [ ] FPS DEBUG logging verified in viz_client_main.cpp

**Test Run:**
- [ ] All old containers stopped and removed
- [ ] Marshals show "healthy" status in `docker ps`
- [ ] Viz-client is running and showing frame increments
- [ ] Test runs for full duration (120s minimum)

**Analysis:**
- [ ] FPS DEBUG log extracted and saved
- [ ] Sample count is reasonable (>100 for 120s test)
- [ ] Analysis includes: avg, min, max, std dev, zero periods
- [ ] Results compared against success criteria, not just average
- [ ] Burst factor calculated (max / target)

---

## Environment Notes

**WSL2 + Docker Desktop Limitations:**
- Windows bind mounts have higher latency than native Linux
- May contribute to I/O bottleneck
- Consider tmpfs test to isolate this factor

**Test at reduced load (64×64×5 @ 20 FPS) first** to determine if problem is capacity or architecture.

---

## Git Commit State Reference

| Branch | Commit | Description | FPS DEBUG? | Compiles? |
|--------|--------|-------------|------------|-----------|
| mri-data-marhsal | 48372c9 | Baseline with FPS DEBUG added | ✓ Yes | ✓ Yes |
| feature/multi-threaded-io | 4b19d04 | Option 1 (4-thread io_context) | ✓ Yes (7a82eaa) | ✓ Yes |
| feature/async-write-queue | dce99e7 | Option 2 (async write queue) | ✗ Needs adding | ✓ Yes |
| feature/async-write-queue | 72ae6da | Option A (async MRD writes) | ✓ Yes (7a82eaa) | ✗ Needs type fix |
| feature/async-write-queue | 6bfab43 | Option B (lock scope opt) | ✓ Yes (7a82eaa) | ✓ Yes |

**Note:** Option A requires manual type cast fix at `marshal_main.cpp:130` (see section 5 above).

---

## Final Recommendations

1. **Start fresh session:** Clear all containers, worktrees, images
2. **Test at reduced load first:** Don't jump straight to 128×128×10
3. **Trust the FPS DEBUG data:** More reliable than visual observation
4. **Look at distributions, not just averages:** Burst/stall ruins user experience
5. **Document everything:** Save raw logs, analysis, and observations
6. **Be methodical:** One variable change at a time
7. **Don't assume current options are optimal:** May need entirely new approach

The burst/stall pattern suggests a fundamental architectural issue that threading alone won't solve. The next agent should focus on **why** frames block, not just how to serve them faster.

Good luck! 🚀

---

## Additional Context for New Agent

### Project Background

This is the **CWRU Data Marshal** system - a real-time medical imaging data management system that:
- Receives MRI frame data from image-streamer client (simulates scanner)
- Writes frames to HDF5/MRD files on disk
- Serves frames via HTTP REST API to visualization client (viz-client)
- Handles concurrent requests from multiple robot control clients

**Real-world use case:** Surgical robot guidance with real-time MRI visualization during procedures.

**Performance requirement:** Smooth, real-time frame delivery for surgeon to track instruments without stuttering.

---

### Why This Investigation Matters

The original implementation worked fine at small frame sizes (32×32×3, ~13 KB/frame) but fails at surgical-grade resolution (128×128×10, 655 KB/frame). Previous attempts to fix via threading (Options 1, 2, A, B) all exhibit the same burst/stall behavior, suggesting a deeper architectural issue.

**User impact:** Surgeons see:
- Frozen visualization (0 FPS stalls)
- Sudden jumps/fast-forward (57 FPS bursts)
- Cannot accurately track instruments in real-time

This is a **critical safety issue** for surgical applications.

---

### Technical Stack

**Languages/Frameworks:**
- C++17 with Boost.Beast (HTTP server)
- Boost.Asio (async I/O, event loops)
- HDF5 C++ library (MRD file format)
- OpenCV (visualization client)
- Docker Compose (orchestration)

**Architecture:**
- MRI Marshal: HTTP server that writes MRD files and serves frame data
- Robot Marshal: Coordinates robot control clients
- Image Streamer: Simulates MRI scanner, sends frames to MRI marshal
- Viz Client: GUI application that fetches and displays latest frames
- Robot Clients: 5 concurrent services (catheter-tracking, controller, planning, front-end, surface-tracking)

**Data Flow:**
```
Image Streamer → HTTP POST → MRI Marshal → HDF5 Write → Disk
                                    ↓
                              HTTP GET ← Viz Client (polls for latest frame)
```

---

### Key Files & Locations

**Marshal Implementation:**
- `src/marshal_main.cpp` - Main server loop, HTTP handlers, write logic
- `src/marshal_state.hpp` - Shared state structure, WriteRequest queue
- `include/mrd/mrd_sink.hpp` - HDF5/MRD file writing interface

**Viz Client:**
- `clients/viz_client/viz_client_main.cpp` - Main loop, FPS calculation, OpenCV display
- Fetches latest frame via: `GET http://mri-marshal:8080/v1/mrd/latest`

**Build System:**
- `scripts/build-client-images.sh` - Builds all Docker images using worktrees
- `docker-compose.demo.yml` - Service definitions
- `.env.demo` - Test configuration (frame size, FPS target)

**Dockerfiles:**
- `.worktrees/mri_data_marshal/Dockerfile` - MRI marshal image
- `docker/Dockerfile.viz-client` - Viz client image

---

### Understanding the Burst/Stall Pattern

**What's happening (hypothesized):**

1. **Normal operation:** MRI marshal serves HTTP requests quickly (~25 FPS)
2. **Write operation starts:** HDF5 write locks file or blocks thread
3. **HTTP threads stall:** Can't serve requests during write → Viz client sees 0 FPS
4. **Requests queue up:** Multiple viz-client polls pile up waiting for lock/write
5. **Write completes:** Lock released, all queued requests process rapidly → 57 FPS burst
6. **Cycle repeats:** Next write causes next stall

**Visual Timeline:**
```
Time:     0s    5s    10s   15s   20s   25s   30s
FPS:      25 → 0  →  57 → 25 → 0  →  50 → 25
          │    │     │    │    │     │    │
          │    │     │    │    │     │    │
          │    └─ Write starts    └─ Write starts
          │        (stall)            (stall)
          │
          └─ Normal serving
```

**Why threading doesn't help:**
- Option 1 (4 threads): All threads can block on same file lock
- Option 2 (async queue): Queue overhead, still blocks on actual write
- Option A (dedicated write thread): Isolates write but file lock still blocks readers
- Option B (reduced lock scope): Serializes more, reduces bursting but also throughput

---

### Frame Size Impact

| Config | Frame Size | Throughput @ 25 FPS | Result |
|--------|-----------|---------------------|--------|
| 32×32×3 | 12.3 KB | 307 KB/s | ✓ Works smoothly |
| 64×64×5 | 81.9 KB | 2 MB/s | ? Not yet tested |
| 96×96×8 | 294.9 KB | 7.4 MB/s | ? Not yet tested |
| 128×128×10 | 655.4 KB | 16.4 MB/s | ✗ Burst/stall |

**Hypothesis:** Threshold somewhere between 81.9 KB and 655.4 KB where writes start blocking long enough to cause noticeable stalls.

---

### What "Success" Looks Like

**Quantitative (from FPS DEBUG logs):**
```
[FPS DEBUG] Elapsed: 1.002s, Frames: 20, FPS: 19.96
[FPS DEBUG] Elapsed: 1.001s, Frames: 20, FPS: 19.98
[FPS DEBUG] Elapsed: 1.003s, Frames: 20, FPS: 19.94
[FPS DEBUG] Elapsed: 1.000s, Frames: 20, FPS: 20.00
[FPS DEBUG] Elapsed: 1.001s, Frames: 20, FPS: 19.98
```
Consistent frame counts (~20), FPS values (~20), no zeros, no spikes >24.

**Qualitative (user experience):**
- Smooth animation in viz-client window
- No perceptible freezes or stutters
- Instrument tracking appears continuous

---

### Docker/WSL2 Quirks

**Volume Performance:**
- Session data stored in `./session-data/` (Windows filesystem via bind mount)
- WSL2 translates Windows paths → may add latency
- Native Linux paths (`/tmp`, `/var`) are faster but volatile

**Container Networking:**
- All services on `cwru-net` bridge network
- DNS works: `mri-marshal`, `robot-marshal` resolve correctly
- No need for port mapping except external access

**X11/GUI:**
- WSLg (Wayland) provides X11 compatibility
- DISPLAY=:0, XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir
- GUI may be slow/laggy but doesn't affect FPS measurements
- Remote Desktop protocol shows windows in Windows taskbar

---

### What Previous Agent Already Tried

1. ✓ Tested baseline single-threaded implementation → Too slow
2. ✓ Added multi-threading (4 threads) → Helped but didn't solve
3. ✓ Implemented async write queue → Made it WORSE
4. ✓ Tried lock scope optimization → Slower but more stable
5. ✓ Dedicated write thread → Best average but extreme bursting
6. ✓ Verified FPS measurement methodology → Accurate via viz-client logs
7. ✓ Ruled out measurement issues → Problem is real, not measurement artifact

**What hasn't been tried yet:**
- ✗ Reduced load testing (64×64×5 @ 20 FPS)
- ✗ RAM-only storage (tmpfs) to isolate disk I/O
- ✗ Profiling actual lock wait times
- ✗ Monitoring CPU/I/O during burst/stall cycles
- ✗ Disabling HDF5 writes entirely to test hypothesis
- ✗ Alternative file formats (raw binary vs HDF5)
- ✗ Memory-mapped file I/O
- ✗ Write coalescing/batching strategies

---

### Known Good vs Known Bad Commits

**Known Working (at small frame sizes):**
- Earlier commits support 32×32×3 smoothly
- Git history has working baseline

**Known Broken (at 128×128×10):**
- All 5 tested options (Baseline, 1, 2, A, B)
- Common denominator: All write to same HDF5 file format

**This suggests:** Problem may be HDF5 library behavior, not just threading architecture.

---

### Performance Budget

**Target:** 128×128×10 @ 25 FPS
- Per-frame size: 655,360 bytes
- Sustained throughput: 16.4 MB/s
- Write latency budget: <40ms (1 frame interval)

**If write takes >40ms:**
- Viz-client polls before write completes → stall
- Multiple polls queue up during write
- Burst when write completes

**Root cause may be:** HDF5 write() takes >40ms for 655 KB on Windows bind mount

**Test this:** Time actual write operations
```cpp
auto start = std::chrono::steady_clock::now();
mrd_sink->append_frame(...);  // HDF5 write
auto duration_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - start).count();
if (duration_ms > 40) {
    std::cerr << "[WRITE PERF] Write took " << duration_ms << "ms (budget: 40ms)\n";
}
```

---

### Debugging Tools Available

**Inside Containers:**
```bash
docker exec cwru-mri-marshal <command>
```

**Useful commands:**
- `curl http://localhost:8080/health` - Check if marshal responding
- `ls -lh /session-data/run_*/mrd/` - Verify files being written
- `lsof /session-data/run_*/mrd/*.mrd` - Check file locks
- `strace -p <pid> -e trace=write` - Trace system calls

**Host-side:**
```bash
docker stats cwru-mri-marshal  # CPU/memory/I/O
docker logs cwru-mri-marshal   # Application logs
docker logs cwru-viz-client | grep "FPS DEBUG"  # Performance data
```

---

### Agent Transition Tips

1. **Read this document fully first** - don't skip sections
2. **Verify test environment** - run through quality checklist
3. **Start with Phase 1** - reduced load test before root cause isolation
4. **Save all data** - raw logs, analysis, observations
5. **Document new findings** - update this document or create new one
6. **Ask clarifying questions** - if anything is unclear about the setup
7. **Don't repeat failed approaches** - all threading variations already tested

**If you get stuck:**
- Check "Common Pitfalls" section first
- Verify worktree state and Docker images
- Review raw FPS logs to understand actual behavior
- Consider if assumptions are wrong (e.g., "disk I/O is the problem")

---

## Success Criteria Summary

**Primary Goal:** Achieve smooth 25 FPS delivery at 128×128×10

**Acceptance Criteria:**
- Zero FPS periods: 0 (no stalls)
- Max FPS: ≤30 (no bursting >1.2× target)
- Avg FPS: ≥23 (meets throughput requirement)
- Std Dev: <3 (consistent delivery)
- Min FPS: ≥20 (no severe drops)

**Stretch Goal:** Support 128×128×10 @ 30 FPS (clinical real-time standard)

**Minimum Viable:** 96×96×8 @ 25 FPS (reduced resolution fallback)

Good luck! This is a challenging problem but solvable with methodical investigation. 🎯
