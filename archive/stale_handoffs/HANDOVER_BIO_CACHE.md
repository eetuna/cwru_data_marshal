# HANDOVER: Add Bio In-Memory Cache (Write-Behind)

**Date:** 2026-01-26
**Branch:** `fix/async-json-writer`
**Priority:** Low (nice-to-have, not blocking)

---

## Task

Add a **write-behind cache** for bio signals, matching the pattern used by MRD and Pose.

---

## Current State

| Endpoint | Write-Behind Cache | GET Reads From |
|----------|-------------------|----------------|
| MRD | ✅ `state.latest_mrd_json` | Memory (fast) |
| Pose | ✅ `state.poses` (PoseStore) | Memory (fast) |
| **Bio** | ❌ **NONE** | File (slow, potentially stale) |

---

## Problem

`GET /v1/bio/latest` reads from `bio.jsonl` file:
- **Slow**: Scans entire file to find last line
- **Stale**: Async queue may not have flushed yet
- **Inconsistent**: MRD and Pose have caches, Bio doesn't

---

## Proposed Solution

Add `latest_bio_json` cache to match MRD pattern.

### Step 1: Add cache variable to `marshal_state.hpp`

```cpp
// Around line 97-98, after latest_mrd_json:
std::mutex latest_mrd_mutex;
std::string latest_mrd_json;

// ADD THESE:
std::mutex latest_bio_mutex;
std::string latest_bio_json;
```

### Step 2: Update POST /v1/bio/signal in `marshal_http.hpp`

**Current code (line 255-263):**
```cpp
// Server generates timestamp
body["ts"] = mrd::iso8601_now_ms();

// Queue bio for async persistence (NON-BLOCKING!)
{
    std::lock_guard<std::mutex> lock(state.json_queue_mutex);
    state.json_write_queue.push({MarshalState::WriteType::BIO, body.dump()});
}
state.json_queue_cv.notify_one();
```

**Change to:**
```cpp
// Server generates timestamp
body["ts"] = mrd::iso8601_now_ms();
const std::string bio_json = body.dump();

// Update in-memory cache (for GET /v1/bio/latest)
{
    std::lock_guard<std::mutex> lock(state.latest_bio_mutex);
    state.latest_bio_json = bio_json;
}

// Queue bio for async persistence (NON-BLOCKING!)
{
    std::lock_guard<std::mutex> lock(state.json_queue_mutex);
    state.json_write_queue.push({MarshalState::WriteType::BIO, bio_json});
}
state.json_queue_cv.notify_one();
```

### Step 3: Update GET /v1/bio/latest in `marshal_http.hpp`

**Current code (line 288-335):** Reads from file

**Replace with:**
```cpp
// GET /v1/bio/latest  (reads from in-memory cache)
if (req.method() == http::verb::get && req.target() == "/v1/bio/latest")
{
    std::string cached_json;
    {
        std::lock_guard<std::mutex> lock(state.latest_bio_mutex);
        cached_json = state.latest_bio_json;
    }

    if (!cached_json.empty())
    {
        try {
            return make_response(http::status::ok, json::parse(cached_json));
        } catch (...) {
            return make_response(http::status::internal_server_error,
                {{"error", "failed to parse bio cache"}});
        }
    }
    return make_response(http::status::no_content, json::object());
}
```

---

## Files to Modify

| File | Change |
|------|--------|
| `src/marshal_state.hpp` | Add `latest_bio_mutex` and `latest_bio_json` |
| `src/marshal_http.hpp` | Update POST to cache, GET to read from cache |

---

## Testing

```bash
# Rebuild
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
docker build --no-cache -f /workspaces/cwru_data_marshal/docker/Dockerfile.mri \
  -t cwru/mri-marshal:latest .

# Test POST then immediate GET
curl -X POST http://localhost:8080/v1/bio/signal \
  -H "Content-Type: application/json" \
  -d '{"source":"ecg","data":[0.1,0.2,0.3],"rate_hz":100}'

# Should return immediately with cached data (not stale)
curl http://localhost:8080/v1/bio/latest
```

---

## Reference: How MRD Does It

**POST side (mrd_sink.cpp:610-621):**
```cpp
// Update in-memory cache (for /v1/mrd/latest endpoint)
{
    std::lock_guard<std::mutex> lock(state_.latest_mrd_mutex);
    state_.latest_mrd_json = entry_dump;
}

// Enqueue for background write (NON-BLOCKING!)
{
    std::lock_guard<std::mutex> lock(state_.json_queue_mutex);
    state_.json_write_queue.push({MarshalState::WriteType::MRD, entry_dump});
}
state_.json_queue_cv.notify_one();
```

**GET side (marshal_http.hpp:569-586):**
```cpp
// GET /v1/mrd/latest  (reads from in-memory cache)
if (req.method() == http::verb::get && req.target() == "/v1/mrd/latest")
{
    std::string cached_json;
    {
        std::lock_guard<std::mutex> lock(state.latest_mrd_mutex);
        cached_json = state.latest_mrd_json;
    }

    if (!cached_json.empty())
    {
        try {
            return make_response(http::status::ok, json::parse(cached_json));
        } catch (...) {
            return make_response(http::status::internal_server_error,
                {{"error", "failed to parse latest JSON"}});
        }
    }
    return make_response(http::status::no_content, {});
}
```

---

## Benefits

1. **Fast reads**: GET returns instantly from memory
2. **Fresh data**: No stale reads from unflushed file
3. **Consistency**: All three data types (MRD, Pose, Bio) use same pattern
4. **Simpler code**: No file scanning in GET handler

---

## Estimated Effort

~15 minutes - straightforward copy of existing MRD pattern.
