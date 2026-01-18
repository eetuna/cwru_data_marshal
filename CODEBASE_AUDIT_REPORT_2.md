# Codebase Audit Report 2

**Date**: 2026-01-07
**Branch**: `feature/graceful-shutdown`
**Auditor**: Codex

---

## Executive Summary

**Critical Issues (P0)**: 3
**High Priority (P1)**: 4
**Medium Priority (P2)**: 4
**Low Priority (P3)**: 1

**Top 5 most critical issues**
1. WS broadcast can call `send()` on freed sessions (use-after-free) (`src/marshal_ws.hpp:128`)
2. Graceful shutdown timeout cannot fire if `flush_all()` blocks (`src/marshal_main.cpp:134`)
3. LRU cleanup is never invoked, so stream map still grows unbounded (`include/mrd_sink.hpp:109`)
4. Blocking `fsync()` on every frame in the ingest hot path (`src/mrd_sink.cpp:593`)
5. viz_client keeps stale HDF5 handles across restarts, causing blank GUI on next session (`clients/viz_client/viz_client_main.cpp:50`)

**Estimated effort for critical fixes**: 1-2 days (mostly localized refactors in WS tracking + shutdown path + scheduling cleanup)

---

## Critical Issues (P0)

### 1) Use-after-free in WebSocket broadcast
- **File & Line**: `src/marshal_ws.hpp:128`
- **Severity**: CRITICAL
- **Category**: Bug (race/use-after-free)
- **Description**: `broadcast()`/`broadcast_to()` copy raw `Session*` pointers, release the lock, and then call `send()` on those pointers. If a session is destroyed after the copy but before `send()`, the pointer is dangling.
- **Impact**: Crash or memory corruption under client churn/high load.
- **Example**:
  ```cpp
  std::vector<void *> clients_copy;
  {
      std::scoped_lock lk(state_.ws_mtx);
      clients_copy.assign(state_.ws_clients.begin(), state_.ws_clients.end());
  }
  for (auto h : clients_copy) {
      auto *s = static_cast<Session *>(h);
      s->send(msg); // can be UAF if Session destroyed after copy
  }
  ```
- **Fix**: Store `std::weak_ptr<Session>` in `ws_clients` and lock to `shared_ptr` before sending. Alternatively, keep `shared_ptr` in the set and use an `enable_shared_from_this`-driven removal strategy.

### 2) Shutdown timeout never fires if flush blocks
- **File & Line**: `src/marshal_main.cpp:134`
- **Severity**: CRITICAL
- **Category**: Bug (deadlock/hang)
- **Description**: The shutdown timer is scheduled on the same `io_context` thread that calls `flush_all()`. If `flush_all()` blocks on HDF5 flush/fsync, the timer handler cannot execute, so the forced-exit safeguard never triggers.
- **Impact**: Process can hang indefinitely on shutdown, defeating the new graceful shutdown logic.
- **Example**:
  ```cpp
  shutdown_timer->async_wait([&](...) { std::exit(1); });
  state.mrd_sink->flush_all(); // blocks IO thread, timer never fires
  ```
- **Fix**: Move flushing to a dedicated thread or `std::async`, or run the timer on a separate `io_context`/thread so it can preempt a blocked flush.

### 3) Stream LRU cleanup is never invoked
- **File & Line**: `include/mrd_sink.hpp:109`
- **Severity**: CRITICAL
- **Category**: Bug (memory leak)
- **Description**: `cleanup_idle_streams()` exists but is never scheduled or called. The stream map still grows without bounds under many unique stream IDs.
- **Impact**: Memory usage grows unbounded over time (original P0 leak remains).
- **Example**:
  ```cpp
  void cleanup_idle_streams(std::chrono::seconds idle_timeout = ...);
  // No call site in main server path
  ```
- **Fix**: Call cleanup on a periodic timer (e.g., in `marshal_main.cpp`) or integrate eviction in `append_frame()` (with throttling).

---

## Additional Issues (P1/P2/P3)

### 4) Data race on `last_accessed`
- **File & Line**: `src/mrd_sink.cpp:516`
- **Severity**: HIGH
- **Category**: Bug (data race)
- **Description**: `cleanup_idle_streams()` reads `stream_state->last_accessed` without holding `stream_state->mutex`, while `append_frame()` writes it under the mutex.
- **Impact**: Undefined behavior under concurrent cleanup + ingest.
- **Fix**: Acquire `stream_state->mutex` before reading `last_accessed` or make it atomic.

### 5) Potential long I/O under `map_mutex_` during cleanup
- **File & Line**: `src/mrd_sink.cpp:516`
- **Severity**: HIGH
- **Category**: Performance / Concurrency
- **Description**: Erasing from `streams_` while holding `map_mutex_` can trigger `StreamState` destruction and HDF5 flush/close under the lock.
- **Impact**: Blocks other ingest threads on a global lock, increasing latency spikes.
- **Fix**: Collect `shared_ptr`s to erase, release `map_mutex_`, then let destructors run outside the lock.

### 6) Blocking fsync in MRD ingest hot path
- **File & Line**: `src/mrd_sink.cpp:593`
- **Severity**: HIGH
- **Category**: Performance
- **Description**: `append_line()` and `write_atomic()` both `fsync()` on every frame.
- **Impact**: Caps throughput and adds latency; still a major bottleneck.
- **Fix**: Batch index/latest updates or move fsyncs to an async queue.

### 7) viz_client caches stale HDF5 handles across restarts
- **File & Line**: `clients/viz_client/viz_client_main.cpp:50`
- **Severity**: HIGH
- **Category**: Bug
- **Description**: The cache only reopens when the path changes. After server restart, the path can be identical but the file has been recreated, leaving the client with a stale handle and no new images.
- **Impact**: GUI stays open but never updates in the next session.
- **Fix**: Detect file replacement (inode/mtime) and reopen, or force reopen when frame index resets or H5Drefresh fails.

### 8) H5Drefresh errors ignored (intermittent black frames)
- **File & Line**: `clients/viz_client/viz_client_main.cpp:98`
- **Severity**: MEDIUM
- **Category**: Bug
- **Description**: `H5Drefresh()` return value is ignored. If refresh fails during a writer extend/flush window, reads return empty and the GUI can briefly go black.
- **Impact**: Intermittent blank frames during demos.
- **Fix**: Check `H5Drefresh()` and retry/backoff or reopen on error.

### 9) CLI parsing can throw on invalid input
- **File & Line**: `src/marshal_main.cpp:66`
- **Severity**: MEDIUM
- **Category**: Code Quality / Bug
- **Description**: `std::stoi`/`std::stoull` are unguarded, and `split()` assumes `host:port` exists. Invalid CLI arguments crash the server.
- **Impact**: Poor UX, possible crash on misconfiguration.
- **Fix**: Validate inputs and return helpful error messages.

### 10) HDF5 complex type handle not released
- **File & Line**: `src/mrd_sink.cpp:36`
- **Severity**: MEDIUM
- **Category**: Resource leak
- **Description**: Static `complex_type` is never closed via `H5Tclose()`.
- **Impact**: Minor HDF5 resource leak on shutdown.
- **Fix**: Register `H5Tclose()` via `std::atexit` or a static RAII wrapper.

### 11) HTTP error response hardcodes HTTP/1.1
- **File & Line**: `src/marshal_http.hpp:628`
- **Severity**: LOW
- **Category**: Code Quality
- **Description**: Body-limit error uses `11` instead of `req.version()`.
- **Impact**: Minor protocol inconsistency for HTTP/1.0 clients.
- **Fix**: Use `req.version()` or parser version.

### 12) Redundant JSON serialization in hot path
- **File & Line**: `src/mrd_sink.cpp:590`
- **Severity**: LOW
- **Category**: Performance
- **Description**: `entry.dump()` is called multiple times for index/latest/WS.
- **Impact**: Extra CPU per frame.
- **Fix**: Serialize once and reuse the string.

---

## Recommended Fixes (Prioritized)

### Immediate (P0)
1. Replace raw WS client pointers with `weak_ptr`/`shared_ptr` to eliminate UAF (`src/marshal_ws.hpp:128`).
2. Move shutdown flushing off the main `io_context` thread so timeout can fire (`src/marshal_main.cpp:134`).
3. Schedule `cleanup_idle_streams()` (timer or call-site) to stop unbounded stream growth (`include/mrd_sink.hpp:109`).

### Short-term (P1)
4. Fix `last_accessed` data race and avoid running HDF5 close/flush under `map_mutex_` (`src/mrd_sink.cpp:516`).
5. Reduce or async the `fsync()` cost in MRD ingest (`src/mrd_sink.cpp:593`, `include/mrd_io.hpp:97`).
6. Reopen HDF5 files in viz_client when the file is replaced to prevent stale reads (`clients/viz_client/viz_client_main.cpp:50`).

### Long-term (P2/P3)
7. Harden CLI parsing and HTTP version handling (`src/marshal_main.cpp:66`, `src/marshal_http.hpp:628`).
8. Add HDF5 handle cleanup for `complex_type` (`src/mrd_sink.cpp:36`).
9. Deduplicate JSON dumps in the ingest hot path (`src/mrd_sink.cpp:590`).

---

## Performance Analysis

**Bottlenecks**
- Blocking `fsync()` calls on every frame (index + latest) remain in the critical path (`src/mrd_sink.cpp:593`, `include/mrd_io.hpp:97`).

**Optimization Opportunities**
- Async/batched metadata writes: expected 2-4x throughput improvements under load.
- Avoid repeated `entry.dump()` in the per-frame path: small but measurable CPU savings.
- Avoid holding `map_mutex_` while destructing streams: reduces latency spikes during cleanup.

---

## Files Changed Summary (Issue Counts)

| File | Issues |
| --- | --- |
| `src/marshal_ws.hpp` | 1 |
| `src/marshal_main.cpp` | 2 |
| `include/mrd_sink.hpp` | 1 |
| `src/mrd_sink.cpp` | 4 |
| `include/mrd_io.hpp` | 1 |
| `clients/viz_client/viz_client_main.cpp` | 2 |
| `src/marshal_http.hpp` | 1 |

---

## Notes on Reported Demo Issues

- **GUI stays open but doesn’t show images after next session**: consistent with stale HDF5 handle caching in `CachedHDF5Reader::open()` when the file path is reused after server restart.
- **Occasional black frame during demos**: consistent with unchecked `H5Drefresh()` failures or transient empty reads during writer flush/extend windows.
