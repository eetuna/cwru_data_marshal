# HANDOVER TO NEXT AGENT - MRI Data Marshal

## STATUS: Threading Architecture Implementation Required

**Branch:** `mri-data-marhsal`

**Critical Issue:** Single-threaded server causing client timeouts under high load.

---

## Problem Summary

The MRI marshal uses a **single-threaded** `boost::asio::io_context`:

```cpp
// src/marshal_main.cpp:171
boost::asio::io_context ioc{1};  // Single thread!
```

With concurrent clients (image_streamer at 20fps + ecg + pose + viz), the single thread blocks on HDF5 writes, causing other clients to timeout:

```
cwru-ecg-client  | TimeoutError: timed out
cwru-pose-client | TimeoutError: timed out
```

---

## Your Task: Implement Threading Architecture

### Documentation

**READ FIRST:** `docs/THREADING_ARCHITECTURE_OPTIONS.md`

Contains complete implementation details for:
- **Option 1:** Multi-threaded io_context (simple, 10 min)
- **Option 2:** Async write queue (better architecture, 2-3 hours)
- **Option 1+2:** Combined (production-ready)

---

## Implementation Steps

### Step 1: Create branch for Option 1

```bash
git checkout mri-data-marhsal
git checkout -b feature/multi-threaded-io
```

### Step 2: Implement Option 1

**File:** `src/marshal_main.cpp`

**Change line 171:**
```cpp
// OLD:
boost::asio::io_context ioc{1};

// NEW:
boost::asio::io_context ioc{4};  // 4 threads
```

**Change line 290:**
```cpp
// OLD:
ioc.run();

// NEW:
const unsigned int num_threads = 4;
std::vector<std::thread> io_threads;
for (unsigned int i = 0; i < num_threads - 1; ++i) {
    io_threads.emplace_back([&ioc]() { ioc.run(); });
}
ioc.run();
for (auto& t : io_threads) {
    if (t.joinable()) t.join();
}
```

**Add include at top:**
```cpp
#include <vector>
```

### Step 3: Build and Test

```bash
mkdir -p build && cd build
cmake .. -DBUILD_TESTING=ON
cmake --build . --parallel
ctest --output-on-failure
```

### Step 4: Rebuild Docker image

```bash
cd /workspaces/cwru_data_marshal
./scripts/build-client-images.sh
```

### Step 5: Run full demo test

```bash
docker compose --env-file .env.demo -f docker-compose.demo.yml up
```

**Verify:**
- No TimeoutError in ecg-client, pose-client logs
- Demo runs for 5+ minutes without crashes
- All clients remain connected

### Step 6: Commit and push

```bash
git add src/marshal_main.cpp
git commit -m "feat: Multi-threaded io_context for concurrent request handling

- Changed io_context from 1 to 4 threads
- Added thread pool to run io_context concurrently
- Fixes timeout issues with concurrent bio/pose/viz clients

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
git push origin feature/multi-threaded-io
```

---

## Optional: Implement Option 2 (Async Write Queue)

If Option 1 performance is insufficient, add Option 2:

```bash
git checkout -b feature/async-write-queue
```

See `docs/THREADING_ARCHITECTURE_OPTIONS.md` for full implementation details.

---

## Benchmark Comparison

After implementation, compare:

| Metric | Before | Option 1 | Option 1+2 |
|--------|--------|----------|------------|
| ECG timeout rate | High | Low | None |
| Response latency | 20-50ms | 10-30ms | 1-5ms |
| Max sustained fps | ~10 | ~30 | ~100+ |

---

## Files Reference

### Key files to modify:
- `src/marshal_main.cpp` - io_context and thread pool
- `src/marshal_state.hpp` - add queue members (Option 2 only)
- `src/marshal_http.hpp` - async write handlers (Option 2 only)

### Documentation:
- `docs/THREADING_ARCHITECTURE_OPTIONS.md` - **Complete implementation guide**
- `docs/USAGE_AND_API.md` - API reference
- `docs/DEMO_GUIDE.md` - Demo instructions

---

## Previous Session Notes

### Already completed:
- ✅ Fixed flush parameters to `{1, 0ms}` (flush every frame)
- ✅ Added `--log-stride` CLI arg to image_streamer
- ✅ Worktrees cleaned up (`git worktree prune`)
- ✅ Created threading architecture documentation

### Branch structure:
- `main` - Docker demo configs
- `mri-data-marhsal` - Current working branch (start here)
- `integrate/robot-catheter` - Older integration (reference only)
- `robot_data_marshal_with_catheter_system_components` - Robot marshal with async caching (reference for Option 2)

---

## Quick Start

```bash
# 1. Switch to branch
git checkout mri-data-marhsal

# 2. Read the docs
cat docs/THREADING_ARCHITECTURE_OPTIONS.md

# 3. Create feature branch
git checkout -b feature/multi-threaded-io

# 4. Make changes to src/marshal_main.cpp (lines 171 and 290)

# 5. Build and test
mkdir -p build && cd build && cmake .. && make -j

# 6. Run docker demo and verify no timeouts
docker compose --env-file .env.demo -f docker-compose.demo.yml up
```

Good luck!
