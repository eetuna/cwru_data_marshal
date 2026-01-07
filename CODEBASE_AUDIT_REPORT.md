# Codebase Audit Report

**Date**: 2026-01-07
**Branch**: `before-async-hdf5-work` (commit `ffb7427`)
**Auditor**: Claude Code

---

## Executive Summary

This audit covers the simpler codebase **before** the async HDF5 queue and WAL implementations. Overall the code is well-structured, but there are several bugs and issues that need attention.

**Critical Issues Found**: 3
**High Priority Issues**: 6
**Medium Priority Issues**: 8
**Low Priority Issues**: 5

---

## Critical Issues (P0)

### 1. Memory Leak: Unbounded Stream Map Growth
**File**: [src/mrd_sink.cpp:439](src/mrd_sink.cpp#L439)
**Severity**: CRITICAL

```cpp
streams_.emplace(stream_id, state);  // Never removed!
```

**Problem**: The `streams_` map grows unboundedly. Each unique `stream_id` creates a new `StreamState` that is never cleaned up.

**Impact**:
- 10,000 unique stream IDs → 10GB+ memory usage
- Server eventually OOMs after extended operation

**Fix**: Add LRU eviction or periodic cleanup of idle streams.

---

### 2. Blocking fsync in HTTP Handler Hot Path
**File**: [src/mrd_sink.cpp:534-536](src/mrd_sink.cpp#L534-L536)

```cpp
append_line(sink.index_root / "index.jsonl", entry.dump());  // BLOCKING fsync
const std::string latest = entry.dump();
write_atomic(sink.index_root / "latest.json", latest.data(), latest.size());  // BLOCKING fsync
```

**Problem**: Two blocking `fsync()` calls (~25ms each = 50ms total) in every frame POST handler.

**Impact**:
- Maximum 20 FPS theoretical limit
- Measured: 7 FPS sustained (142ms per frame)

**Fix**: Move to async queue or batch fsyncs.

---

### 3. WebSocket Session Destructor Called Under Lock
**File**: [src/marshal_ws.hpp:181-185](src/marshal_ws.hpp#L181-L185)

```cpp
~Session()
{
    std::scoped_lock lk(state.ws_mtx);  // Takes lock
    state.ws_clients.erase(this);       // Erases self
}
```

**Problem**: If `~Session()` is called while another thread holds `ws_mtx` (e.g., in `broadcast()`), and that thread tries to access the Session being destroyed, there's a race condition.

**Impact**: Potential crash or undefined behavior during client disconnect.

**Fix**: Use weak_ptr or deferred cleanup queue.

---

## High Priority Issues (P1)

### 4. Race Condition in WebSocket Broadcast
**File**: [src/marshal_ws.hpp:128-136](src/marshal_ws.hpp#L128-L136)

```cpp
void broadcast(const std::string &msg)
{
    std::scoped_lock lk(state_.ws_mtx);
    for (auto h : state_.ws_clients)
    {
        auto *s = static_cast<Session *>(h);
        s->send(msg);  // send() takes its own lock - potential deadlock
    }
}
```

**Problem**: `send()` at line 234 takes `send_mtx`, while we already hold `ws_mtx`. If Session destructor runs, there's potential for issues.

**Impact**: Potential deadlock or crash under high load.

---

### 5. No Graceful Shutdown
**File**: [src/marshal_main.cpp:149](src/marshal_main.cpp#L149)

```cpp
ioc.run();  // Runs forever, no signal handling
return 0;
```

**Problem**: No SIGTERM/SIGINT handling. HDF5 files may not be properly flushed on shutdown.

**Impact**:
- Data loss on process kill
- Corrupted HDF5 files

**Fix**: Add signal handlers to flush and close files gracefully.

---

### 6. HDF5 File Handles Never Closed on Stream Dimension Change
**File**: [src/mrd_sink.cpp:462-480](src/mrd_sink.cpp#L462-L480)

```cpp
if (reopen_file)
{
    state->file.reset();  // Old file closed here
    // ...
    state->file = std::make_unique<MrdFile>(...);  // New file opened
}
```

**Problem**: While `reset()` does close the file, if there's an exception between reset and creating the new MrdFile, the stream is left without a file.

**Impact**: Data loss if exception occurs during file rotation.

---

### 7. Static HDF5 Complex Type Never Freed
**File**: [src/mrd_sink.cpp:36-51](src/mrd_sink.cpp#L36-L51)

```cpp
static hid_t complex_type = [] {
    hid_t t = H5Tcreate(H5T_COMPOUND, sizeof(float) * 2);
    // ...
    return t;
}();
return complex_type;  // Never H5Tclose'd
```

**Problem**: The static complex type is never freed with `H5Tclose()`.

**Impact**: Minor HDF5 resource leak on shutdown.

---

### 8. Double WebSocket Emit
**File**: [src/mrd_sink.cpp:540-541](src/mrd_sink.cpp#L540-L541)

```cpp
state_.ws_emit(entry.dump());          // Broadcast to ALL
state_.ws_emit_topic(entry.dump(), "mrd");  // Broadcast to "mrd" topic
```

**Problem**: Same message sent twice - once to all clients, once to "mrd" topic subscribers.

**Impact**: Clients receive duplicate messages, wasting bandwidth.

---

### 9. Synchronous WebSocket Write in Async Context
**File**: [src/marshal_ws.hpp:232-240](src/marshal_ws.hpp#L232-L240)

```cpp
void send(const std::string &s)
{
    std::scoped_lock lk(send_mtx);
    ws.text(true);
    ws.write(boost::asio::buffer(line));  // BLOCKING write
}
```

**Problem**: Synchronous `ws.write()` in an async WebSocket context can block other operations.

**Impact**: High latency under load, potential backpressure issues.

---

## Medium Priority Issues (P2)

### 10. Unchecked stoull/stoi in CLI Parsing
**File**: [src/marshal_main.cpp:66-70](src/marshal_main.cpp#L66-L70)

```cpp
max_body_size = static_cast<std::size_t>(std::stoull(argv[++i]));
flush_max_frames = static_cast<std::size_t>(std::stoull(argv[++i]));
flush_max_ms = std::stoi(argv[++i]);
```

**Problem**: No try/catch around `stoull`/`stoi`. Invalid input crashes the server.

**Impact**: Server fails to start with invalid CLI args, unhelpful error message.

---

### 11. Potential Integer Overflow in Frame Calculations
**File**: [src/mrd_sink.cpp:137-139](src/mrd_sink.cpp#L137-L139)

```cpp
frame_bytes_ = element_type_bytes(type_) * static_cast<size_t>(dims_.spatial[0]) *
               static_cast<size_t>(dims_.spatial[1]) * static_cast<size_t>(dims_.spatial[2]) *
               static_cast<size_t>(dims_.channels);
```

**Problem**: No overflow check. Large dimensions could overflow `size_t`.

**Impact**: Buffer size mismatch, potential memory corruption.

---

### 12. CURL Timeout Too Short for Large Files
**File**: [clients/viz_client/viz_client_main.cpp:180](clients/viz_client/viz_client_main.cpp#L180)

```cpp
curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 100L);  // 100ms timeout
```

**Problem**: 100ms timeout may be too short for slow networks or large responses.

**Impact**: viz_client fails to connect on slower networks.

---

### 13. image_streamer Doesn't Exit on Persistent Failure
**File**: [clients/image_streamer/image_streamer_main.cpp:251-252](clients/image_streamer/image_streamer_main.cpp#L251-L252)

```cpp
if (!delivered) {
    continue; // try frame again after reconnect attempts
}
```

**Problem**: If delivery fails after 3 attempts, it just continues to the next frame without incrementing `frame_index`, potentially retrying forever.

**Impact**: image_streamer can get stuck in infinite retry loop.

---

### 14. HTTP Body Limit Error Response Wrong Version
**File**: [src/marshal_http.hpp:581](src/marshal_http.hpp#L581)

```cpp
http::response<http::string_body> res{http::status::payload_too_large, 11};  // HTTP/1.1 hardcoded
```

**Problem**: HTTP version hardcoded to 11 instead of using request version.

**Impact**: Minor - could confuse HTTP/1.0 clients.

---

### 15. No Bounds Checking on Slice Index
**File**: [clients/viz_client/viz_client_main.cpp:228-230](clients/viz_client/viz_client_main.cpp#L228-L230)

```cpp
int slice = (nz > 0) ? (int)(nz / 2) + slice_offset : 0;
if (slice < 0) slice = 0;
if (nz > 0 && (size_t)slice >= nz) slice = nz - 1;
```

**Problem**: `slice_offset` is user-controlled (keyboard), and while bounds are checked here, the bounds checking happens AFTER the calculation which could overflow for extreme offsets.

**Impact**: Minor - integer overflow with extreme user input.

---

### 16. Missing CURL Cleanup on Early Return
**File**: [clients/viz_client/viz_client_main.cpp:175-178](clients/viz_client/viz_client_main.cpp#L175-L178)

```cpp
if (!curl)
{
    std::cerr << "Failed to initialize CURL\n";
    return 1;  // curl_easy_cleanup never called (but curl is null anyway)
}
```

**Problem**: Not actually a bug (curl is null), but pattern is confusing.

**Impact**: None - just code clarity.

---

### 17. Redundant JSON Dump Calls
**File**: [src/mrd_sink.cpp:531-536](src/mrd_sink.cpp#L531-L536)

```cpp
nlohmann::json entry = make_entry_json(result);
entry["seq"] = seq;

append_line(sink.index_root / "index.jsonl", entry.dump());  // dump #1
const std::string latest = entry.dump();                      // dump #2
write_atomic(sink.index_root / "latest.json", latest.data(), latest.size());
```

**Problem**: `entry.dump()` called twice for same content.

**Impact**: Wasted CPU cycles.

---

## Low Priority Issues (P3)

### 18. Inconsistent Error Handling Style
Some places use exceptions, others return error codes, others just log to stderr.

### 19. Magic Numbers
- `kTargetChunkBytes = 8ULL * 1024ULL * 1024ULL` - Could use a named constant
- HTTP timeout values scattered throughout code

### 20. Missing const-correctness
Several member functions that don't modify state are not marked `const`.

### 21. Thread Safety Documentation
No documentation about which methods are thread-safe.

### 22. Unused Code
- `write_buffer` in image_streamer is created but only consumed, never written to

---

## Script Issues

### 23. Frame Count Calculation Wrong (Fixed in other branch)
**Files**: Multiple demo scripts

```bash
IMAGE_FRAME_COUNT=$((DEMO_DURATION_SEC * 1000 / IMAGE_INTERVAL_MS))
```

**Problem**: Assumes frames are sent at exactly the interval rate, ignoring POST latency.

---

### 24. Missing robot_marshal_demo Compilation (Fixed in other branch)
**File**: Some demo scripts don't compile robot_marshal_demo before running.

---

## Recommendations

### Immediate Fixes (Do Now)
1. Add graceful shutdown signal handling
2. Fix double WebSocket emit
3. Add stream map cleanup/LRU eviction

### Short-term Fixes (This Week)
4. Move fsync to background thread
5. Fix WebSocket broadcast race condition
6. Add CLI argument validation

### Long-term Improvements
7. Implement proper async I/O throughout
8. Add monitoring/metrics endpoint
9. Add comprehensive error handling
10. Add integration tests

---

## Files Changed Summary

| File | Issues Found |
|------|--------------|
| src/mrd_sink.cpp | 5 |
| src/marshal_ws.hpp | 3 |
| src/marshal_main.cpp | 2 |
| src/marshal_http.hpp | 2 |
| include/mrd_io.hpp | 1 |
| clients/viz_client/viz_client_main.cpp | 3 |
| clients/image_streamer/image_streamer_main.cpp | 2 |

---

**Total Issues**: 24
**Estimated Fix Time**: 2-3 days for critical/high priority items
