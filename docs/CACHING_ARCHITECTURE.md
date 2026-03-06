# In-Memory Caching Architecture

**Date:** 2026-01-26
**Branch:** `feature/bio-memory-cache`

---

## Overview

The MRI Data Marshal uses **write-behind caching** to achieve fast HTTP responses while ensuring data persistence. This document explains the patterns, trade-offs, and future considerations.

---

## The Problem We're Solving

Without caching:
```
Client POST → Write to disk → Return response
Client GET  → Read from disk → Return response
```

**Issues:**
1. **Slow writes**: Disk I/O blocks the HTTP response (causes client timeouts)
2. **Slow reads**: File scanning to find latest entry
3. **Stale reads**: Async queue may not have flushed yet, so GET returns old data

With write-behind caching:
```
Client POST → Update RAM cache → Queue async disk write → Return immediately
Client GET  → Read from RAM cache → Return instantly
```

---

## Current Implementation

### Three Data Types, Two Patterns

| Data Type | Cache Variable | Cache Type | Why This Pattern |
|-----------|---------------|------------|------------------|
| **MRD** | `state.latest_mrd_json` | `std::string` | Variable JSON structure |
| **Bio** | `state.latest_bio_json` | `std::string` | Variable JSON structure |
| **Pose** | `state.poses` | `PoseStore` (struct) | Fixed schema, used elsewhere in code |

---

## Pattern 1: String Cache (MRD, Bio)

### How It Works

```cpp
// marshal_state.hpp
std::mutex latest_bio_mutex;
std::string latest_bio_json;  // Stores serialized JSON string
```

**POST (write path):**
```cpp
// 1. Build JSON object
body["ts"] = mrd::iso8601_now_ms();

// 2. Serialize ONCE
const std::string bio_json = body.dump();

// 3. Update RAM cache
{
    std::lock_guard<std::mutex> lock(state.latest_bio_mutex);
    state.latest_bio_json = bio_json;  // Just a string copy
}

// 4. Queue for async disk write
{
    std::lock_guard<std::mutex> lock(state.json_queue_mutex);
    state.json_write_queue.push({MarshalState::WriteType::BIO, bio_json});
}
state.json_queue_cv.notify_one();

// 5. Return immediately (disk write happens in background)
return make_response(http::status::ok, {...});
```

**GET (read path):**
```cpp
// 1. Copy from cache (under lock)
std::string cached_json;
{
    std::lock_guard<std::mutex> lock(state.latest_bio_mutex);
    cached_json = state.latest_bio_json;
}

// 2. Parse and return
if (!cached_json.empty()) {
    return make_response(http::status::ok, json::parse(cached_json));
}
return make_response(http::status::no_content, {});
```

### Data Flow Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                         POST /v1/bio/signal                         │
└─────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
                    ┌──────────────────────────┐
                    │   Parse & Validate JSON   │
                    │   Add timestamp           │
                    │   bio_json = body.dump()  │
                    └──────────────────────────┘
                                   │
                    ┌──────────────┴──────────────┐
                    │                             │
                    ▼                             ▼
        ┌───────────────────┐         ┌───────────────────┐
        │   RAM Cache       │         │   Async Queue     │
        │   (instant)       │         │   (background)    │
        │                   │         │                   │
        │ latest_bio_json = │         │ json_write_queue  │
        │ bio_json          │         │ .push(bio_json)   │
        └───────────────────┘         └───────────────────┘
                    │                             │
                    │                             ▼
                    │              ┌───────────────────────────┐
                    │              │   Background Thread       │
                    │              │   json_writer_thread      │
                    │              │                           │
                    │              │   Wakes on cv.notify()    │
                    │              │   Writes to bio.jsonl     │
                    │              └───────────────────────────┘
                    │                             │
                    ▼                             ▼
        ┌───────────────────┐         ┌───────────────────┐
        │ GET /v1/bio/latest│         │   bio.jsonl       │
        │ reads from here   │         │   (on disk)       │
        └───────────────────┘         └───────────────────┘
```

### Pros & Cons

**Pros:**
- Simple: just store the serialized string
- Flexible: works with any JSON structure
- No struct definition needed

**Cons:**
- Parse overhead on every GET (`json::parse(cached_json)`)
- No typed access to fields (can't do `cache.source` without parsing)
- Redundant: stores string in cache AND in queue

---

## Pattern 2: Struct Cache (Pose)

### How It Works

```cpp
// common/pose.hpp
struct Pose {
    std::array<double, 3> p;   // Position [x, y, z]
    std::array<double, 9> R;   // Rotation matrix (row-major)
    std::string frame;          // "scanner", "robot", etc.
    std::string source;         // "fk", "api", "tracker"
    std::chrono::system_clock::time_point t;
};

class PoseStore {
    mutable std::mutex mtx_;
    Pose pose_;
public:
    void set(const Pose& p) {
        std::lock_guard<std::mutex> lock(mtx_);
        pose_ = p;
    }
    Pose get() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return pose_;
    }
};

// marshal_state.hpp
PoseStore poses;  // Thread-safe pose storage
```

**POST (write path):**
```cpp
// 1. Parse JSON into struct
Pose pose{};
pose.frame = body.value("frame", "scanner");
pose.source = body.value("source", "api");
for (int i = 0; i < 3; ++i) pose.p[i] = jp[i];
for (int i = 0; i < 9; ++i) pose.R[i] = jR[i];
pose.t = std::chrono::system_clock::now();

// 2. Update RAM cache (struct copy)
state.poses.set(pose);

// 3. Queue for async disk write (serialize for queue)
auto j_persist = pose_to_json(pose);
j_persist["ts"] = mrd::iso8601_now_ms();
state.json_write_queue.push({MarshalState::WriteType::POSE, j_persist.dump()});
```

**GET (read path):**
```cpp
// 1. Get struct from cache
auto p = state.poses.get();

// 2. Serialize and return
auto jpose = pose_to_json(p);
jpose["ts"] = iso8601_now();
return make_response(http::status::ok, {{"pose", jpose}});
```

### Pros & Cons

**Pros:**
- Type-safe: compiler catches errors
- Fast field access: `pose.p[0]` vs parsing JSON
- Can be used for calculations/transforms elsewhere in code
- Self-documenting: struct defines the schema

**Cons:**
- Fixed schema: adding fields requires code changes
- Serialize on every GET (`pose_to_json()`)
- More boilerplate code

---

## The Flexibility Problem You Raised

### Current Pose Limitations

The current `Pose` struct assumes a single tracked point (tool tip):

```cpp
struct Pose {
    std::array<double, 3> p;   // ONE position
    std::array<double, 9> R;   // ONE rotation
    std::string frame;
    std::string source;
};
```

### What If We Need Multiple Tracked Objects?

Real surgical scenarios might track:
- **Tool tip**: where the instrument is
- **Coil location**: MRI receive coil position
- **Patient markers**: fiducial positions
- **Robot joints**: full kinematic chain

### Option A: Extend the Struct (Rigid)

```cpp
struct Pose {
    // Tool tip (existing)
    std::array<double, 3> tip_p;
    std::array<double, 9> tip_R;

    // Coil (new)
    std::array<double, 3> coil_p;
    std::array<double, 9> coil_R;

    // Patient markers (new)
    std::vector<std::array<double, 3>> markers;

    std::string frame;
    std::string source;
};
```

**Problems:**
- Every new tracked object = code change
- Unused fields waste memory
- API changes break clients

### Option B: Named Pose Map (Semi-Flexible)

```cpp
struct TrackedObject {
    std::array<double, 3> p;
    std::array<double, 9> R;
    std::string frame;
};

struct PoseState {
    std::map<std::string, TrackedObject> objects;
    // "tip" -> {p, R, frame}
    // "coil" -> {p, R, frame}
    // "marker_1" -> {p, R, frame}
    std::string source;
    std::chrono::system_clock::time_point t;
};
```

**Pros:**
- Add new objects without code changes
- Type-safe for individual objects
- Can query specific objects: `state.objects["coil"]`

**Cons:**
- Still limited to position+rotation per object
- More complex API

### Option C: Switch to String Cache (Fully Flexible)

```cpp
// marshal_state.hpp
std::mutex latest_pose_mutex;
std::string latest_pose_json;  // Like MRD/Bio pattern
```

**Pros:**
- Any JSON structure works
- Add fields without code changes
- Consistent with MRD/Bio pattern

**Cons:**
- Lose type safety
- Lose direct field access for calculations
- Parse overhead on every read

### Option D: Hybrid (Recommended)

Keep struct for internal use, add string cache for API:

```cpp
// Internal typed access (for calculations, transforms)
PoseStore poses;  // Keep existing

// API cache (for flexible GET response)
std::mutex latest_pose_mutex;
std::string latest_pose_json;

// POST handler:
// 1. Parse into struct for internal use
state.poses.set(pose);

// 2. Also cache raw JSON for GET
state.latest_pose_json = body.dump();
```

This gives:
- Type safety for internal code
- Flexibility for API responses
- Consistent caching pattern

---

## Performance Comparison

### Write Path (POST)

| Pattern | Operations | Approximate Cost |
|---------|-----------|------------------|
| String | `json.dump()` once | ~1-5 μs |
| Struct | Parse fields + `json.dump()` for queue | ~2-10 μs |

### Read Path (GET)

| Pattern | Operations | Approximate Cost |
|---------|-----------|------------------|
| String | `json::parse()` | ~1-5 μs |
| Struct | `pose_to_json()` | ~1-3 μs |

**Bottom line:** Both are sub-millisecond. The difference is negligible compared to network latency (~1-10 ms).

---

## Thread Safety

### Mutex Scope

Each cache has its own mutex to minimize contention:

```cpp
std::mutex latest_mrd_mutex;   // Only for latest_mrd_json
std::mutex latest_bio_mutex;   // Only for latest_bio_json
std::mutex json_queue_mutex;   // Only for json_write_queue
```

### Lock Duration

Locks are held for minimal time (just the copy):

```cpp
// GOOD: Lock only for copy
std::string cached_json;
{
    std::lock_guard<std::mutex> lock(state.latest_bio_mutex);
    cached_json = state.latest_bio_json;  // ~nanoseconds
}
// Parse OUTSIDE lock
json::parse(cached_json);  // ~microseconds

// BAD: Don't do this
{
    std::lock_guard<std::mutex> lock(state.latest_bio_mutex);
    return json::parse(state.latest_bio_json);  // Holds lock during parse!
}
```

---

## Memory Considerations

### String Cache Memory

Each string cache holds ONE entry (the latest):

```cpp
std::string latest_bio_json;  // ~100-1000 bytes typical
std::string latest_mrd_json;  // ~200-2000 bytes typical
```

**Total RAM for caches:** < 10 KB (negligible)

### Queue Memory

The async queue can grow if disk is slow:

```cpp
std::queue<WriteRequest> json_write_queue;
```

In normal operation: 0-10 entries (~10 KB)
Under disk pressure: could grow unbounded

**Future improvement:** Add queue size limit with backpressure.

---

## Summary: When to Use Which Pattern

| Scenario | Recommended Pattern |
|----------|---------------------|
| Variable JSON structure | String cache |
| Fixed schema, used internally | Struct + PoseStore |
| Need flexibility + type safety | Hybrid (both) |
| Performance critical (millions/sec) | Struct (avoid parse) |
| Rapid prototyping | String cache |

---

## Files Reference

| File | Contains |
|------|----------|
| `src/marshal_state.hpp` | Cache variables, mutex declarations |
| `src/marshal_http.hpp` | GET/POST handlers that use caches |
| `src/mrd_sink.cpp` | MRD cache update (in append_frame) |
| `src/common/pose.hpp` | Pose struct and PoseStore class |

---

## Future Considerations

1. **Unified pattern**: Consider switching Pose to string cache for consistency
2. **Queue limits**: Add backpressure when queue grows too large
3. **Metrics**: Track cache hit rates, queue depth
4. **Multiple poses**: If coil/markers needed, consider Option B or D above
