# HANDOVER: Async JSON Queue Architecture

**Date:** 2026-01-26
**Branch:** `fix/async-json-writer`
**Latest Commit:** `f913d3c` - feat: Add async queue for bio/pose writes to fix client timeouts

---

## Problem Solved

Pose-client and ecg-client were timing out after ~8 requests because `POST /v1/pose/update` and `POST /v1/bio/signal` were blocking on `fsync()` (via `mrd::append_line()`).

**Fix:** Queue all JSON writes to a background thread instead of blocking the HTTP handler.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         HTTP HANDLERS                                │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  POST /v1/mrd/frame          POST /v1/pose/update    POST /v1/bio/signal
│         │                           │                       │        │
│         ▼                           ▼                       ▼        │
│  ┌─────────────┐            ┌─────────────┐         ┌─────────────┐ │
│  │ IN-MEMORY   │            │ IN-MEMORY   │         │ IN-MEMORY   │ │
│  │ CACHE       │            │ CACHE       │         │ CACHE      │ │
│  │             │            │             │         │             │ │
│  │ latest_mrd_ │            │ PoseStore   │         │latest_bio_ │ │
│  │ json        │            │ poses       │         │ json        │ │
│  └──────┬──────┘            └──────┬──────┘         └──────┬──────┘ │
│         │                          │                       │        │
│         ▼                          ▼                       ▼        │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                    json_write_queue                           │  │
│  │  std::queue<WriteRequest>                                     │  │
│  │  WriteRequest { WriteType type; std::string data; }           │  │
│  │  WriteType = MRD | BIO | POSE                                 │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                              │                                      │
└──────────────────────────────┼──────────────────────────────────────┘
                               │
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│                 BACKGROUND WRITER THREAD                              │
│                 json_writer_thread_func()                             │
├──────────────────────────────────────────────────────────────────────┤
│                                                                       │
│  while (running) {                                                    │
│      wait for queue items or shutdown                                 │
│      batch = swap(queue)  // grab all pending                         │
│                                                                       │
│      for each item in batch:                                          │
│          switch (type):                                               │
│              MRD  → write to index.jsonl                              │
│              BIO  → write to bio.jsonl                                │
│              POSE → write to poses.jsonl                              │
│                                                                       │
│      flush all streams                                                │
│      update latest.json (MRD only)                                    │
│  }                                                                    │
│                                                                       │
└──────────────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│                         FILES ON DISK                                 │
├──────────────────────────────────────────────────────────────────────┤
│  /session-data/run_*/mrd/index.jsonl   (MRD frame metadata)          │
│  /session-data/run_*/mrd/latest.json   (last MRD entry)              │
│  /session-data/run_*/mrd/bio.jsonl     (ECG/bio signals)             │
│  /session-data/run_*/mrd/poses.jsonl   (pose updates)                │
└──────────────────────────────────────────────────────────────────────┘
```

---

## In-Memory Caches

| Data Type | Cache Variable | Location | Updated On POST? | Used by GET? |
|-----------|----------------|----------|------------------|--------------|
| **MRD** | `state.latest_mrd_json` | marshal_state.hpp:98 | ✅ Yes (mrd_sink.cpp:613) | ✅ Yes (marshal_http.hpp:574) |
| **Pose** | `state.poses` (PoseStore) | marshal_state.hpp:64 | ✅ Yes (marshal_http.hpp:190) | ✅ Yes (marshal_http.hpp:154) |
| **Bio** | `state.latest_bio_json` | marshal_state.hpp:101-102 | ✅ Yes (marshal_http.hpp:282) | ✅ Yes (marshal_http.hpp:324) |

### Cache Details

**MRD Cache (`latest_mrd_json`):**
- Type: `std::string` (raw JSON)
- Mutex: `latest_mrd_mutex`
- Updated: Before queueing in `mrd_sink.cpp:612-614`
- Read: `GET /v1/mrd/latest` reads from cache (marshal_http.hpp:574-575)

**Pose Cache (`PoseStore poses`):**
- Type: Custom `PoseStore` class (see common/pose.hpp)
- Thread-safe: Internal mutex
- Updated: Before queueing in `marshal_http.hpp:190`
- Read: `GET /v1/pose/current` reads from cache (marshal_http.hpp:154)

**Bio Cache (`latest_bio_json`):**
- Declared: `marshal_state.hpp:101-102`
- Updated: Before queueing in `marshal_http.hpp:282`
- Read: `GET /v1/bio/latest` reads from cache (marshal_http.hpp:324)

---

## Queue Infrastructure

**Located in:** `marshal_state.hpp:80-94`

```cpp
enum class WriteType { MRD, BIO, POSE };

struct WriteRequest {
    WriteType type;
    std::string data;  // JSON string to write
};

std::queue<WriteRequest> json_write_queue;
std::mutex json_queue_mutex;
std::condition_variable json_queue_cv;
std::atomic<bool> json_writer_running{true};
std::thread json_writer_thread;

// File paths
std::filesystem::path json_index_path;   // index.jsonl (MRD)
std::filesystem::path json_latest_path;  // latest.json (MRD)
std::filesystem::path json_bio_path;     // bio.jsonl
std::filesystem::path json_pose_path;    // poses.jsonl
```

---

## Background Writer Thread

**Located in:** `marshal_main.cpp:92-154`

**Key behaviors:**
1. Opens all 3 output files once at startup (append mode)
2. Waits on condition variable for queue items
3. Swaps entire queue (batching) for efficiency
4. Writes each item to correct file based on `WriteType`
5. Flushes all streams after each batch
6. Updates `latest.json` only for MRD entries
7. Graceful shutdown: drains queue before exit

---

## POST Flow (Non-Blocking)

### POST /v1/mrd/frame
```
1. HDF5 write (still synchronous)
2. state.latest_mrd_json = entry_dump     ← CACHE UPDATE
3. queue.push({MRD, entry_dump})          ← QUEUE (non-blocking)
4. cv.notify_one()
5. WebSocket broadcast
6. Return HTTP 200
```

### POST /v1/pose/update
```
1. state.poses.set(pose)                  ← CACHE UPDATE
2. queue.push({POSE, json.dump()})        ← QUEUE (non-blocking)
3. cv.notify_one()
4. WebSocket broadcast
5. Return HTTP 200
```

### POST /v1/bio/signal
```
1. state.latest_bio_json = body.dump()    ← CACHE (in-memory)
2. queue.push({BIO, body.dump()})         ← QUEUE (non-blocking)
2. cv.notify_one()
3. WebSocket broadcast
4. Return HTTP 200
```

---

## GET Flow

### GET /v1/mrd/latest
- Reads from `state.latest_mrd_json` (in-memory) ✅ Fast

### GET /v1/pose/current
- Reads from `state.poses` (in-memory) ✅ Fast

### GET /v1/bio/latest
- Reads from `state.latest_bio_json` (in-memory) ✅ Fast

---

## Known Gaps

### ~~1. Bio has no in-memory cache~~ **FIXED**
`latest_bio_json` cache added in marshal_state.hpp:101-102. Same pattern as MRD and Pose.


### 2. `/v1/mrd/ingest` still blocking
**Location:** `mrd_io.hpp:224` still calls `append_line()` directly
**Impact:** Legacy ingest endpoint still blocks on fsync
**Fix:** Route through queue or deprecate endpoint

---

## Files Modified

| File | Changes |
|------|---------|
| `src/marshal_state.hpp` | Added `WriteType`, `WriteRequest`, queue, paths |
| `src/marshal_main.cpp` | Added `json_writer_thread_func()`, path init |
| `src/marshal_http.hpp` | Replaced `append_line()` with queue push for bio/pose |
| `src/mrd_sink.cpp` | Updated to use typed `WriteRequest` |

---

## Testing

```bash
# Rebuild
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
docker build --no-cache -f /workspaces/cwru_data_marshal/docker/Dockerfile.mri \
  -t cwru/mri-marshal:latest .

# Test
docker compose --env-file .env.demo -f docker-compose.demo.yml up --profile viz

# Watch for timeouts (should be fixed)
docker logs -f cwru-pose-client
docker logs -f cwru-ecg-client
```

---

## Summary

| Endpoint | Blocking Before | Blocking After | Cache |
|----------|-----------------|----------------|-------|
| POST /v1/mrd/frame | JSON only | ✅ Non-blocking | ✅ `latest_mrd_json` |
| POST /v1/pose/update | ❌ fsync | ✅ Non-blocking | ✅ `PoseStore` |
| POST /v1/bio/signal | ❌ fsync | ✅ Non-blocking | ✅ `latest_bio_json` |
| GET /v1/mrd/latest | - | - | ✅ From memory |
| GET /v1/pose/current | - | - | ✅ From memory |
| GET /v1/bio/latest | - | - | ✅ From memory |

---

**Next Steps:**
1. Test the fix with `docker compose up --profile viz`
2. ~~Consider adding bio cache~~ **DONE** — `latest_bio_json` added
3. Consider fixing `/v1/mrd/ingest` if it's still used
