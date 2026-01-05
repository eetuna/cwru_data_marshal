# Robot Marshal Source (Thread-Safe Fork)

**Source:** Extracted from `upstream/robot-data-marshal` branch with thread-safety fixes applied.

## Files

### From Upstream (Unchanged)
- `circularBuffer.hpp` - Original circular buffer implementation
- `httplib.h` - Single-header HTTP server library
- `json.hpp` - nlohmann/json library
- `file_routes.json` - Client routing configuration for circular data flow

### From Upstream (Modified)
- **`server.cpp`** - Modified to use `ThreadSafeCircularBuffer` + path fixes
- **`client-a.cpp`** - Modified IP: `172.28.1.10:8080` → `127.0.0.1:8081`
- **`client-b.cpp`** - Modified IP: `172.28.1.10:8080` → `127.0.0.1:8081`
- **`client-c.cpp`** - Modified IP: `172.28.1.10:8080` → `127.0.0.1:8081`

### Local Additions
- **`threadSafeCircularBuffer.hpp`** - Thread-safe wrapper with `std::shared_mutex`

## Changes Applied

1. Added `ThreadSafeCircularBuffer` wrapper class
2. Changed `file_caches` and `write_queues` to use `ThreadSafeCircularBuffer<std::string>`
3. Made `stop_worker` atomic (`std::atomic<bool>`)
4. Fixed background worker condition variable to protect map access
5. Fixed hardcoded paths: `/files/` → `./files/`, `/log_files/` → `./log_files/`
6. Added `std::filesystem::create_directories()` calls
7. Changed listen IP from `172.28.1.10` to `0.0.0.0`
8. Fixed `emplace()` calls to use `std::piecewise_construct` (mutex not copyable)

## Upstream Status

The `upstream/robot-data-marshal` branch remains **unmodified**. This is a local working copy with thread-safety patches.

## Build

```bash
g++ -std=c++17 -I ./scripts/robot_marshal_src \
    ./scripts/robot_marshal_src/server.cpp \
    -o ./build/robot_marshal_demo -lpthread
```

Used by:
- `scripts/benchmarks/robot_marshal_comprehensive_test.sh`
- `scripts/tools/robot_marshal_stress_test.sh`
- `scripts/run_demo.sh`
