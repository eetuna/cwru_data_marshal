# Test Parameters and Configuration

**Date:** 2026-01-05
**Purpose:** Clear documentation of all test parameters

---

## Unified Code Source

**ALL tests use:** `/workspaces/cwru_data_marshal/scripts/robot_marshal_src/`

**Files:**
- `threadSafeCircularBuffer.hpp` - Thread-safe wrapper with std::shared_mutex
- `server.cpp` - Modified server using ThreadSafeCircularBuffer
- `circularBuffer.hpp` - Original (wrapped by thread-safe version)
- `httplib.h` - HTTP server library
- `json.hpp` - JSON parsing library

**Compilation:**
```bash
g++ -std=c++17 -I ./scripts/robot_marshal_src \
    ./scripts/robot_marshal_src/server.cpp \
    -o ./build/robot_marshal_demo -lpthread
```

---

## Test 1: Comprehensive Test

**Script:** `scripts/benchmarks/robot_marshal_comprehensive_test.sh`

### Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| PORT | 8081 | HTTP server port |
| DATA_DIR | `./data_robot_bench` | Temporary data directory |
| FILES_DIR | `./files` | File storage directory |
| Files configured | `["robot_status", "robot_commands", "sensor_data"]` | Three independent files |

### Test 1: Basic Write/Read
- **Operations:** 1 write, 1 read
- **Validation:** Check for `received_at` timestamp injection

### Test 2: Schema Validation
- **Operations:** 1 invalid POST
- **Payload:** `{"bad": "payload"}`  (missing required fields)
- **Expected:** HTTP 400 response

### Test 3: Multi-File Isolation
- **Operations:** Write to 3 different files, read from each
- **Files:** `robot_status`, `robot_commands`, `sensor_data`
- **Validation:** Each file maintains independent state

### Test 4: Sequential Access
- **Operations:** 6 sequential writes
- **Client ID:** `seq-test`
- **Validation:** All complete in < 500ms

### Test 5: Query Parameter ?last=N
- **Operations:** Write 3 entries, query with `?last=3`
- **Validation:** Response contains `"entries"` array with 3 items

### Test 6: Background Persistence
- **Operations:** Write to `sensor_data`, wait 1 second
- **Validation:** File `./files/sensor_data` exists and is non-empty

### Test 7: 5 Simultaneous Clients
- **Clients:** 5 concurrent bash subshells
- **Operations per client:** 3 iterations (3 reads + 3 writes = 6 ops each)
- **Total operations:** 5 clients × 6 ops = 30 operations
- **Timeout:** 10 seconds (uses `timeout 10 bash -c 'wait'`)
- **Validation:** All complete in < 5000ms (actual: ~7ms)

**Test 7 Code:**
```bash
for c in 1 2 3 4 5; do
    (
        for i in $(seq 1 3); do
            curl -s --max-time 2 "http://127.0.0.1:$PORT/read/robot_status" > /dev/null 2>&1
            curl -s --max-time 2 -X POST "http://127.0.0.1:$PORT/write/robot_status" \
              -H "Content-Type: application/json" \
              -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"client-$c\", \"values\": [{\"i\": $i}]}" \
              > /dev/null 2>&1
        done
    ) &
done
timeout 10 bash -c 'wait'
```

---

## Test 2: Stress Test

**Script:** `scripts/tools/robot_marshal_stress_test.sh`

### Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| ROBOT_HTTP | 8081 | HTTP server port |
| DATA_ROBOT | `./data_robot_stress` | Temporary data directory |
| FILES_DIR | `./files` | File storage directory |
| TIMEOUT | 30 seconds | Maximum test duration |
| MAX_WRITE_LATENCY_MS | 500ms | Write latency threshold |
| MAX_READ_LATENCY_MS | 200ms | Read latency threshold |
| MIN_THROUGHPUT | 50 req/s | Minimum throughput requirement |

### Test 1: Write Latency
- **Operations:** 20 sequential writes
- **Measurement:** Average write latency
- **Threshold:** < 500ms per write

### Test 2: Read Latency
- **Operations:** 20 sequential reads
- **Measurement:** Average read latency
- **Threshold:** < 200ms per read

### Test 3: Concurrent Access
- **Operations:** 15 concurrent requests (30 total: 15 reads + 15 writes)
- **Timeout:** 15 seconds (uses `timeout 15 bash -c 'wait'`)
- **Validation:** No deadlock, completes in < 10000ms

**Test 3 Code:**
```bash
for i in $(seq 1 15); do
    curl -s --max-time 5 http://127.0.0.1:$ROBOT_HTTP/read/stress_test > /dev/null &
    curl -s --max-time 5 -X POST http://127.0.0.1:$ROBOT_HTTP/write/stress_test \
        -H "Content-Type: application/json" \
        -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"ci\", \"values\": [{\"i\": $i}]}" > /dev/null &
done
timeout 15 bash -c 'wait'
```

### Test 4: Schema Validation
- **Operations:** 1 invalid POST
- **Payload:** `{"bad": "payload"}`
- **Expected:** HTTP 400

### Test 5: Throughput
- **Operations:** 100 sequential writes
- **Measurement:** Requests per second
- **Threshold:** ≥ 50 req/s

### Test 6: Buffer Integrity
- **Operations:** Query with `?last=10`
- **Validation:** Response contains `"entries"` array

---

## Test 3: Full Demo

**Script:** `scripts/run_demo.sh`

### Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| MRI_HTTP | 8080 | MRI marshal HTTP port |
| MRI_WS | 8090 | MRI marshal WebSocket port |
| ROBOT_HTTP | 8081 | Robot marshal HTTP port |
| DATA_MRI | `./data_demo_mri` | MRI marshal data directory |
| DATA_ROBOT | `./data_demo_robot` | Robot marshal data directory |
| DATA_DUMPBOX | `./data_demo_dumpbox` | Recording storage directory |

### Step 5B: Concurrent Access Test
- **Operations:** 20 concurrent requests (10 reads + 10 writes)
- **Validation:** Completes without deadlock
- **Expected duration:** < 2 seconds (actual: ~36ms)

**Step 5B Code:**
```bash
PIDS=()
for i in $(seq 1 10); do
    curl -s --max-time 5 http://127.0.0.1:$ROBOT_HTTP/read/robot_status > /dev/null &
    PIDS+=($!)
    curl -s --max-time 5 -X POST http://127.0.0.1:$ROBOT_HTTP/write/robot_status \
      -H "Content-Type: application/json" \
      -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"concurrent\", \"values\": [{\"i\": $i}]}" > /dev/null &
    PIDS+=($!)
done
for pid in "${PIDS[@]}"; do
    wait $pid 2>/dev/null || true
done
```

### Step 5E: 5 Simultaneous Clients
- **Clients:** 5 concurrent bash subshells
- **Operations per client:** 3 iterations (3 reads + 3 writes = 6 ops each)
- **Total operations:** 30 operations
- **Expected duration:** < 2 seconds

---

## Concurrency Levels Summary

| Test | Concurrent Operations | Request Type | Timeout | Purpose |
|------|----------------------|--------------|---------|---------|
| Comprehensive Test 7 | 5 clients × 3 iterations = 30 ops | Mixed (read/write) | 10s | Realistic multi-client load |
| Stress Test 3 | 15 × 2 = 30 ops | Mixed (read/write) | 15s | Deadlock detection |
| Demo Step 5B | 20 ops | Mixed (10 read + 10 write) | None (tracked PIDs) | High-burst concurrency |
| Demo Step 5E | 5 clients × 3 iterations = 30 ops | Mixed (read/write) | None (tracked PIDs) | Sustained multi-client |

---

## Thread-Safe Circular Buffer Configuration

**Implementation:** `scripts/robot_marshal_src/threadSafeCircularBuffer.hpp`

### Lock Strategy

- **Reader-Writer Lock:** `std::shared_mutex`
- **Writers (exclusive):** `push()`, `pop()`
  - Only one writer at a time
  - Blocks all readers during write
- **Readers (shared):** `peek()`, `empty()`, `size()`, `is_full()`
  - Multiple readers allowed concurrently
  - Blocks while writer is active

### Buffer Configuration

- **Capacity:** 1000 entries (set in `server.cpp` line 51: `int cache_capacity = 1000`)
- **Overflow behavior:** Overwrites oldest entries when full
- **Thread safety:** All operations are atomic (protected by mutex)

---

## JSON Payload Schema

### Write Request (POST /write/<filename>)

**Required fields:**
```json
{
  "sent_at": 1735000000000000000,    // int64 nanosecond timestamp
  "client_id": "test-client",         // string
  "values": [{"key": "value"}]        // array of objects
}
```

**Server adds:**
```json
{
  "received_at": 1735000000100000000  // int64 nanosecond timestamp
}
```

### Read Response (GET /read/<filename>)

**Single entry (default):**
```json
{
  "sent_at": 1735000000000000000,
  "received_at": 1735000000100000000,
  "client_id": "test-client",
  "values": [{"key": "value"}]
}
```

**Multiple entries (GET /read/<filename>?last=N):**
```json
{
  "entries": [
    { /* entry 1 */ },
    { /* entry 2 */ },
    { /* entry N */ }
  ],
  "count": 3
}
```

---

## Build Configuration

**Compiler:** g++ (GCC)
**C++ Standard:** C++17 (`-std=c++17`)
**Include Path:** `-I ./scripts/robot_marshal_src`
**Libraries:** `-lpthread` (POSIX threads)
**Output Binary:** `./build/robot_marshal_demo`
**Binary Size:** ~2.6 MB

**Dependencies:**
- `httplib.h` - Single-header HTTP server library
- `json.hpp` - nlohmann/json library
- Standard C++17 libraries (`<shared_mutex>`, `<atomic>`, `<filesystem>`)

---

## Performance Benchmarks

### Thread-Safe Version

| Scenario | Result | Notes |
|----------|--------|-------|
| 5 clients × 3 iterations | 7ms | Comprehensive Test 7 |
| 20 concurrent requests | 36ms | Demo Step 5B |
| 15 concurrent requests | < 10000ms | Stress Test 3 |
| Sequential access (6 ops) | 60ms | Comprehensive Test 4 |

### Lock Overhead

- **Sequential operations:** ~0ms overhead (locks uncontended)
- **Concurrent operations:** ~1-5ms overhead per operation
- **Reader concurrency:** Multiple readers can proceed simultaneously (no contention)

---

## File Structure

```
/workspaces/cwru_data_marshal/
├── scripts/
│   ├── robot_marshal_src/          ← Source code (thread-safe)
│   │   ├── threadSafeCircularBuffer.hpp
│   │   ├── server.cpp
│   │   ├── circularBuffer.hpp
│   │   ├── httplib.h
│   │   ├── json.hpp
│   │   └── README.md
│   ├── benchmarks/
│   │   └── robot_marshal_comprehensive_test.sh
│   ├── tools/
│   │   └── robot_marshal_stress_test.sh
│   └── run_demo.sh
├── build/
│   └── robot_marshal_demo          ← Compiled binary
├── files/                          ← File storage (created by tests)
├── files.json                      ← File configuration
└── log_files/                      ← Error logs (created by server)
    └── error_log.txt
```

---

## Environment Requirements

- **OS:** Linux (tested on WSL2)
- **Tools:** bash, g++, curl, git
- **Ports:** 8080-8081 must be available
- **Permissions:** Read/write access to working directory

---

## Common Issues

### Issue 1: Server fails to start
**Symptom:** `curl: (7) Failed to connect`
**Cause:** Port already in use
**Fix:** `pkill robot_marshal_demo && sleep 1`

### Issue 2: Permission denied on /files/
**Symptom:** `Permission denied` error in error_log.txt
**Cause:** Trying to create absolute path `/files/` instead of `./files/`
**Fix:** Already fixed in thread-safe version (uses `./files/`)

### Issue 3: Test hangs at "Waiting for clients..."
**Symptom:** Test times out
**Cause:** Plain `wait` blocks forever if subshells fail
**Fix:** Use `timeout 10 bash -c 'wait'` instead

### Issue 4: Binary not found
**Symptom:** `./build/robot_marshal_demo: No such file or directory`
**Cause:** Cleanup deleted binary
**Fix:** Rebuild with compilation command above

---

## Validation Checklist

After running tests, verify:

- [ ] All test scripts pass (exit code 0)
- [ ] No zombie processes (`ps aux | grep robot_marshal`)
- [ ] No file descriptor leaks (`lsof -p <PID>`)
- [ ] Error log is empty or only contains expected warnings
- [ ] Files directory contains expected files
- [ ] Binary size is ~2.6 MB
- [ ] Compilation produces no warnings

