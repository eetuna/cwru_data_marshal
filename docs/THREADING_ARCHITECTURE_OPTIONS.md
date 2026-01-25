# Threading Architecture Options for MRI Marshal

## Problem Statement

The MRI Marshal currently uses a **single-threaded** `boost::asio::io_context` (line 171 of `src/marshal_main.cpp`):

```cpp
boost::asio::io_context ioc{1};  // Single thread
```

With high-frequency image streaming (20+ fps) and concurrent bio/pose/viz clients, the single thread becomes a bottleneck:
- HDF5 writes block the thread for 10-50ms
- Other HTTP requests (ECG, pose, viz GET) timeout waiting for the thread
- Result: Client crashes with `TimeoutError: timed out`

This document describes two architecture options to fix this problem.

---

## Option 1: Multi-threaded io_context

### Overview
Run multiple threads to process HTTP/WebSocket requests concurrently. While one thread is blocked on HDF5 write, other threads handle incoming requests.

### Files to Modify
- `src/marshal_main.cpp` (lines 171 and 290)

### Code Changes

#### Step 1: Increase io_context thread count (line 171)

**Current:**
```cpp
boost::asio::io_context ioc{1};
```

**Change to:**
```cpp
// Use 4 threads (or adjust based on workload)
boost::asio::io_context ioc{4};

// Alternative: Use all CPU cores
// boost::asio::io_context ioc{std::thread::hardware_concurrency()};
```

#### Step 2: Run io_context on multiple threads (line 290)

**Current:**
```cpp
ioc.run();
```

**Change to:**
```cpp
// Run io_context on multiple threads
const unsigned int num_threads = 4;  // Match ioc{4} above
std::vector<std::thread> io_threads;

// Launch worker threads
for (unsigned int i = 0; i < num_threads - 1; ++i) {
    io_threads.emplace_back([&ioc]() {
        ioc.run();
    });
}

// Main thread also runs io_context
ioc.run();

// Wait for all threads to complete
for (auto& t : io_threads) {
    if (t.joinable()) {
        t.join();
    }
}
```

### Thread Safety Considerations

With multiple threads, shared state needs protection:

1. **MarshalState access**: Already has `session_mtx` mutex. Verify all shared state is protected.

2. **MrdSink access**: HDF5 with SWMR mode handles concurrent reads, but writes should be serialized:
   ```cpp
   // In marshal_state.hpp, add if not present:
   std::mutex mrd_write_mtx;  // Serialize HDF5 writes
   ```

3. **WebSocket broadcast**: The `ws_emit` function should be thread-safe. Verify WsServer implementation.

4. **stream_map in MrdSink**: Needs mutex protection for concurrent access.

### Pros
- Simple implementation (10-15 minutes)
- Standard Boost.Asio pattern
- Allows concurrent request handling
- Low risk

### Cons
- HDF5 writes still block individual threads
- May need additional mutexes for thread safety
- Thread contention under heavy load

---

## Option 2: Async Write Queue

### Overview
Move all disk I/O (HDF5 writes, JSONL appends) to a background worker thread. HTTP handlers push write requests to a queue and return immediately.

### Files to Modify
- `src/marshal_state.hpp` - Add queue and synchronization primitives
- `src/marshal_main.cpp` - Start/stop background writer thread
- `src/marshal_http.hpp` - Change handlers to queue writes instead of sync writes
- `src/mrd_sink.cpp` - Add queue-based write methods (optional)

### Code Changes

#### Step 1: Add write queue to MarshalState (marshal_state.hpp)

Add after line 21 (includes):
```cpp
#include <queue>
#include <condition_variable>
```

Add after line 51 (before MarshalState):
```cpp
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
    std::filesystem::path file_path;  // For FILE_APPEND
};
```

Add to MarshalState struct (after line 74):
```cpp
    // Async write queue (Option 2)
    std::queue<WriteRequest> write_queue;
    std::mutex write_queue_mtx;
    std::condition_variable write_queue_cv;
    std::atomic<bool> writer_running{true};
```

#### Step 2: Add background writer thread (marshal_main.cpp)

Add after includes (around line 20):
```cpp
#include <queue>
```

Add worker function before main() (around line 50):
```cpp
void background_writer(MarshalState& state) {
    while (state.writer_running.load()) {
        WriteRequest req;

        {
            std::unique_lock<std::mutex> lock(state.write_queue_mtx);
            state.write_queue_cv.wait(lock, [&state] {
                return !state.write_queue.empty() || !state.writer_running.load();
            });

            if (!state.writer_running.load() && state.write_queue.empty()) {
                break;  // Shutdown and queue empty
            }

            if (state.write_queue.empty()) {
                continue;
            }

            req = std::move(state.write_queue.front());
            state.write_queue.pop();
        }

        // Process write (outside lock - this is the slow part)
        try {
            switch (req.type) {
                case WriteRequest::Type::MRD_FRAME:
                    if (state.mrd_sink) {
                        state.mrd_sink->write_frame_sync(
                            req.stream_name, req.frame_data,
                            req.dims, req.channels
                        );
                    }
                    break;

                case WriteRequest::Type::BIO_SIGNAL:
                case WriteRequest::Type::POSE_UPDATE:
                case WriteRequest::Type::FILE_APPEND:
                    mrd::append_line(req.file_path, req.json_payload);
                    break;
            }
        } catch (const std::exception& e) {
            std::cerr << "[WRITER] Error: " << e.what() << "\n";
        }
    }

    std::cerr << "[WRITER] Background writer stopped.\n";
}
```

In main(), after creating mrd_sink (around line 210), start the writer thread:
```cpp
// Start background writer thread
std::thread writer_thread(background_writer, std::ref(state));
```

Before `ioc.run()` (around line 290), add graceful shutdown:
```cpp
// In shutdown handler, stop writer thread
state.writer_running.store(false);
state.write_queue_cv.notify_all();
if (writer_thread.joinable()) {
    writer_thread.join();
}
```

#### Step 3: Change HTTP handlers to use queue (marshal_http.hpp)

**For bio signal endpoint (around line 258-263):**

**Current:**
```cpp
// Persist to disk
try {
    auto paths = mrd::resolve_sink_paths(state);
    fs::path bio_log = paths.index_root / "bio.jsonl";
    mrd::append_line(bio_log, body.dump());
}
```

**Change to:**
```cpp
// Queue for async persistence
{
    WriteRequest req;
    req.type = WriteRequest::Type::BIO_SIGNAL;
    req.json_payload = body.dump();
    req.file_path = mrd::resolve_sink_paths(state).index_root / "bio.jsonl";

    std::lock_guard<std::mutex> lock(state.write_queue_mtx);
    state.write_queue.push(std::move(req));
}
state.write_queue_cv.notify_one();
```

**Apply similar changes to:**
- Pose endpoint (around line 200-216)
- MRD frame POST endpoint (uses mrd_sink->write_frame)

### Pros
- HTTP responses are instant (1-2ms vs 20-50ms)
- Single writer thread eliminates HDF5 lock contention
- Scales to very high request rates
- Best long-term architecture

### Cons
- More complex implementation (2-3 hours)
- Small delay before data hits disk (acceptable for real-time viz)
- Need to handle queue overflow scenarios
- Shutdown must drain queue

---

## Option 1 + Option 2 Combined (Recommended for Production)

### Overview
Use multi-threaded io_context (Option 1) for concurrent HTTP handling, plus async write queue (Option 2) for non-blocking disk I/O.

### Architecture Diagram

```
┌───────────────────────────────────────────────────────────┐
│  HTTP/WebSocket Server (Multi-threaded io_context)       │
│                                                           │
│  Thread 1 ─┬─ Handle HTTP request ─┬─ Push to queue ─┐   │
│  Thread 2 ─┤                       │                  │   │
│  Thread 3 ─┤                       │                  │   │
│  Thread 4 ─┘                       │                  │   │
│                                    │                  │   │
└────────────────────────────────────┼──────────────────┼───┘
                                     │                  │
                                     ▼                  │
┌───────────────────────────────────────────────────────┼───┐
│  Thread-safe Write Queue                              │   │
│  ┌─────────────────────────────────────────────────┐  │   │
│  │ Frame1 │ Bio1 │ Pose1 │ Frame2 │ Bio2 │ ...    │◄─┘   │
│  └─────────────────────────────────────────────────┘      │
└───────────────────────────────────┬───────────────────────┘
                                    │
                                    ▼
┌───────────────────────────────────────────────────────────┐
│  Background Writer Thread (Single)                        │
│                                                           │
│  Pop from queue ──► Write to HDF5 ──► Flush to disk      │
│                     (slow, but doesn't block HTTP)        │
└───────────────────────────────────────────────────────────┘
```

### Benefits
- **4 HTTP threads** handle concurrent connections
- **Instant responses** (queue, don't block)
- **Single HDF5 writer** = no lock contention
- **Maximum throughput** AND **minimum latency**

---

## Implementation Order

### Recommended Approach

1. **Phase 1: Implement Option 1** (multi-threaded io_context)
   - Branch: `feature/multi-threaded-io`
   - Time: 10-15 minutes
   - Test with full demo
   - If performance is acceptable, ship it

2. **Phase 2: Add Option 2** (async write queue) - if needed
   - Branch: `feature/async-write-queue` (from Phase 1)
   - Time: 2-3 hours
   - Test and benchmark
   - Compare with Phase 1

### Testing Procedure

```bash
# Start demo with all clients
docker compose --env-file .env.demo -f docker-compose.demo.yml up

# Verify no timeouts in logs after 5+ minutes
# Check: ecg-client, pose-client, viz-client, image-streamer

# Benchmark (optional)
# Measure response latency with curl:
time curl -X POST http://localhost:8080/v1/bio/signal \
  -H "Content-Type: application/json" \
  -d '{"source":"test","data":[0.5],"rate_hz":100}'
```

---

## Quick Reference

| Aspect | Option 1 | Option 2 | Option 1+2 |
|--------|----------|----------|------------|
| Implementation time | 10 min | 2-3 hours | 3 hours |
| HTTP response latency | 10-50ms | 1-2ms | 1-2ms |
| Thread count | 4 HTTP | 1 HTTP + 1 writer | 4 HTTP + 1 writer |
| HDF5 thread safety | Need mutex | Single writer | Single writer |
| Complexity | Low | Medium | Medium |
| Production ready | Yes | Yes | Best |

---

## Files Modified Summary

### Option 1
- `src/marshal_main.cpp` (2 changes: line 171, line 290)

### Option 2
- `src/marshal_state.hpp` (add WriteRequest struct, queue members)
- `src/marshal_main.cpp` (add worker function, start/stop thread)
- `src/marshal_http.hpp` (change sync writes to queue push)

### Option 1+2
- All of the above
