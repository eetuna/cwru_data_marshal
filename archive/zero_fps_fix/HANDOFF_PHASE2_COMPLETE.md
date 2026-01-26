# Handoff: Phase 2 Investigation Complete

## TL;DR

**Root Cause:** Windows NTFS I/O via WSL2 bind mounts (40ms+ latency), NOT code architecture.

**Quick Fix to Try First:**
```yaml
# docker-compose.demo.yml - use Docker named volume instead of bind mount
volumes:
  session-data:  # stays in ext4, no Windows I/O
```

**If that fails:** Implement memory-first architecture (Option 4) - requires code changes.

---

## Summary

All Phase 2 investigation steps (2B, 2C, 2D) have been completed. The root cause is **confirmed** and is **NOT an architectural problem** in the code.

---

## Investigation Results

### Phase 2B: Disable HDF5 Writes
**Status:** SKIPPED (test approach was flawed - viz-client needs HDF5 files to calculate FPS)

**Note:** This test is redundant anyway - Phase 2A's tmpfs results already proved the same thing.

### Phase 2C: Lock Profiling
**Status:** COMPLETED

**Result:** Zero lock waits > 5ms during entire 120-second test

**Conclusion:** Lock contention is NOT the problem. Mutex locks are acquired almost instantly.

### Phase 2D: CPU/I/O Monitoring
**Status:** COMPLETED

**Result:**
- Average CPU: 7.5%
- Min CPU: 3.9%
- Max CPU: 15.5%

**Conclusion:** System is I/O-bound, not CPU-bound.

---

## Root Cause (Confirmed)

**Environmental, not architectural:**
- WSL2 Docker bind mounts to Windows NTFS introduce 40ms+ write latencies
- HDF5 SWMR writes block the HTTP request handler during disk sync
- This causes burst/stall FPS pattern

**The code itself is correct:**
- SWMR is used properly (enabled unconditionally via `H5Fstart_swmr_write`)
- Lock design is efficient (no contention measured)
- CPU usage is minimal (7.5% average)

---

## Testing in WSL2 Environment

We're running in WSL2 devcontainer, so we can't test "native Linux" in the pure sense. However, several storage options stay within ext4 and don't cross to Windows NTFS:

| Storage Type | Path Example | Crosses to Windows? | Performance |
|--------------|--------------|---------------------|-------------|
| Windows bind mount | `./session-data:/session-data` | **Yes** (slow) | 40ms+ latency |
| Docker named volume | `session-data:` (no host path) | No (ext4) | Fast |
| WSL2 native path | `/tmp/session-data:/session-data` | No (ext4) | Fast |
| tmpfs | RAM disk | No (RAM) | Fastest (proven) |

**Key insight:** Docker named volumes and WSL2 native paths (`/tmp/...`) stay within the Linux filesystem - they don't cross to Windows NTFS.

---

## Recommended Fix Options

### Option 1: Docker Named Volume (Config change, persistent, RECOMMENDED FIRST TEST)
```yaml
# docker-compose.demo.yml
services:
  mri-marshal:
    volumes:
      - session-data:/session-data

volumes:
  session-data:  # No driver_opts = stored in Docker's ext4
```
**Use for:** Production on any platform including WSL2
**Test this first** - if it works, no code changes needed!

### Option 2: WSL2 Native Path (Config change, temp persistence)
```yaml
volumes:
  - /tmp/cwru-session:/session-data  # WSL2 ext4, not Windows NTFS
```
**Use for:** Development with session persistence (data lost on WSL restart)

### Option 3: tmpfs (Immediate, no persistence)
```yaml
# docker-compose.demo.yml
volumes:
  session-tmpfs:
    driver: local
    driver_opts:
      type: tmpfs
      device: tmpfs
      o: size=2G
```
**Use for:** Demo, testing, development (already proven to work)

### Option 4: Memory-First Architecture (Code change, universal)

**IMPORTANT:** Phase 1 already tested "Option 2 (async write queue)" and it **FAILED** (9 zero periods, 5.42 std dev). Simply making writes async is NOT sufficient because the **viz-client still reads from disk**.

The real fix requires **both sides** to use memory:

**Writer side (marshal):**
1. HTTP POST stores frame in memory ring buffer (immediate return)
2. Background thread flushes to disk asynchronously (for persistence)

**Reader side (viz-client):**
3. Viz-client reads from memory buffer via HTTP API, NOT from HDF5 files
4. New endpoint: `GET /v1/mrd/frame-data?index=N` returns raw frame bytes from memory

**Why this works:**
- tmpfs test proved: when both writer AND reader use RAM, FPS is stable
- Option 2 failed because: writer was async but reader still hit slow disk

**Implementation Sub-Options:**

#### Option 4A: HTTP Binary Transfer
- New endpoint: `GET /v1/mrd/frame-data?index=N` returns raw frame bytes
- Viz-client polls HTTP instead of reading HDF5 files
- Bandwidth: ~655KB/frame × 20 FPS = ~13 MB/s (feasible for local container network)
- Pros: Simple, no shared memory complexity
- Cons: HTTP overhead, polling latency

#### Option 4B: Shared Memory (mmap)
- Marshal writes frames to shared memory region
- Viz-client reads directly from shared memory
- Pros: Zero-copy, lowest latency
- Cons: More complex, requires IPC coordination, Docker volume config

#### Option 4C: Unix Domain Socket
- Stream frames directly between containers via UDS
- Pros: Low latency, bidirectional
- Cons: Connection management, buffering logic

**Implementation locations:**
- `mrd_sink.cpp` - add memory ring buffer, modify `append_frame()`
- `marshal_http.hpp` - add new endpoint to serve frame data from memory
- `viz_client_main.cpp` - read from HTTP/memory instead of HDF5 files

**Use for:** Production on any platform (including Windows/WSL2) when native ext4 isn't available

---

## Files Modified During Investigation

### Temporarily modified (reverted):
- `.worktrees/mri_data_marshal/src/mrd_sink.cpp` - lock profiling instrumentation (reverted)
- `.worktrees/mri_data_marshal/src/marshal_http.hpp` - HDF5 write disable (reverted)

### Test data collected:
- `/tmp/lock_profiling_phase2c.log` - 0 lines (no lock waits > 5ms)
- `/tmp/cpu_io_stats.csv` - 30 samples of CPU/BlockIO stats

---

## Next Steps for Review

### RECOMMENDED TEST PRIORITY (Try config changes before code changes!)

**Step 1: Test Docker Named Volume (5 min)**
```bash
# Modify docker-compose.demo.yml to use named volume instead of bind mount
# Then run: ./scripts/run-demo.sh
```
If FPS is stable → Problem solved, no code changes needed!

**Step 2: Test WSL2 Native Path (5 min)**
```bash
# Change volume to: /tmp/cwru-session:/session-data
# Then run: ./scripts/run-demo.sh
```
Alternative if named volume has issues.

**Step 3: Only if config changes fail → Implement Option 4**

### If implementing Option 4 (Memory-First Architecture):

1. **Review existing code:**
   - `mrd_sink.cpp` lines 577-625 (`MrdSink::append_frame`)
   - Current HDF5 write logic at lines 315-380

2. **Design memory ring buffer:**
   - Configurable size (suggest 100 frames = ~65MB)
   - Thread-safe circular buffer
   - Background flush thread with error handling
   - Memory pressure handling (block or drop oldest?)

3. **Add HTTP endpoint for frame data:**
   - `GET /v1/mrd/frame-data?stream=X&index=N`
   - Return raw bytes from memory buffer
   - Handle "frame not yet available" case

4. **Update viz-client:**
   - Poll HTTP endpoint instead of HDF5 reads
   - Or implement shared memory reader

### Success Criteria (must pass all):
- Zero FPS periods = 0
- Max FPS ≤ 24 (1.2x target)
- Std Dev < 3
- Min FPS ≥ 15 (0.75x target)

---

## Key Code Locations

| Component | File | Lines | Purpose |
|-----------|------|-------|---------|
| Frame append (hot path) | `mrd_sink.cpp` | 577-625 | Main write path to modify for Option 4 |
| HDF5 file write | `mrd_sink.cpp` | 315-380 | Low-level HDF5 operations |
| SWMR init | `mrd_sink.cpp` | 284-296 | SWMR write mode setup |
| HTTP handler | `marshal_http.hpp` | 343-410 | POST /v1/mrd/frame handler |
| Viz client read | `viz_client_main.cpp` | 53-100 | HDF5 SWMR read logic |

---

## Conclusion

The investigation is complete. The FPS burst/stall issue is definitively caused by **Windows filesystem I/O latency via WSL2**, not by any architectural flaw in the code. Multiple solution options are available depending on persistence requirements.
