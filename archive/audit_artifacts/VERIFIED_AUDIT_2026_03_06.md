# Verified Codebase Audit - 2026-03-06

Branch: `feature/marshal-image-return`
Method: Every claim below was traced to specific source lines. No claims carried from prior audit docs.

---

## Build & Test Status

- **Build**: 151/151 targets, 0 warnings (Ninja + C++20)
- **Tests**: 9/9 passing (43+ assertions)
- **Dependencies**: Boost 1.74, HDF5 1.10, ISMRMRD, OpenCV 4.5, Catch2 v3, nlohmann/json

## Source Code (6,707 lines across 29 files)

| Component | Files | Lines |
|-----------|-------|-------|
| Core server | marshal_main.cpp, marshal_http.hpp, marshal_ws.hpp, marshal_state.hpp | 2,264 |
| HDF5 engine | mrd_sink.cpp, mrd_sink.hpp | 1,066 |
| Headers/utils | mrd_io.hpp, mrd_type_detector.hpp, image_message_utils.hpp, atomic_write.hpp, mk_mrd_utils.hpp, pose.hpp | 811 |
| Clients | image_streamer, kspace_streamer, viz_client, fk_client, ws_producer | 1,375 |
| Tests | 9 test suites | 1,080 |
| Services | playback | 243 |

## Architecture (verified from source)

### Data Flow: Frame Ingestion

Traced from `marshal_http.hpp:527` -> `mrd_sink.cpp:413` -> `mrd_sink.cpp:321`:

1. HTTP handler receives `POST /v1/mrd/frame`
2. `detect_mrd_type()` classifies payload as ACQUISITION, IMAGE, or HDF5_FILE
3. ACQUISITION: forwarded async to `--recon-endpoint` (detached thread), returns 202 immediately
4. IMAGE: `MrdFile::append_frame()` writes to HDF5 dataset via `H5Dwrite`
5. `perform_flush()` called immediately (default: `max_pending_frames=1` at `marshal_state.hpp:63`)
6. `H5Dflush` executes synchronously before returning (`mrd_sink.cpp:397`)
7. Back in `MrdSink::append_frame()` (`mrd_sink.cpp:610-621`): updates in-memory cache, queues metadata to `json_write_queue`
8. Background thread in `marshal_main.cpp:100-153` dequeues and writes `index.jsonl` + `latest.json`

**Key correctness property**: HDF5 data is flushed *before* metadata is visible. No race condition exists with default flush policy.

### Threading Model

- **HTTP**: Single boost::asio::io_context, synchronous handlers
- **WebSocket**: Async per-connection queues with max 1000 messages (`marshal_ws.hpp`)
- **JSON writer**: Dedicated background thread consuming `json_write_queue` (`marshal_main.cpp:96-154`)
- **Reconstruction forwarding**: Detached threads per k-space frame (`marshal_http.hpp:588-663`)
- **Graceful shutdown**: SIGTERM/SIGINT handler with configurable timeout, flushes all streams

### HTTP API (verified from marshal_http.hpp)

| Method | Endpoint | Line | Behavior |
|--------|----------|------|----------|
| GET | /health | 146 | Returns `{"uptime_s": N}`, always 200 |
| GET | /v1/config | 341 | Returns data_dir, ws_port, max_entries |
| GET | /v1/pose/current | 153 | Reads in-memory cache. 204 if empty |
| POST | /v1/pose/update | 176 | Validates p[3]+R[9], updates cache, queues persistence |
| GET | /v1/bio/latest | 320 | Reads in-memory cache. 204 if empty |
| POST | /v1/bio/signal | 260 | Updates cache, queues persistence |
| POST | /v1/mrd/frame | 527 | Smart type detection + routing (ACQUISITION/IMAGE/HDF5) |
| POST | /v1/mrd/ingest | 758 | Smart type detection + atomic file write |
| POST | /v1/mrd/callback | 347 | Receives reconstructed images from recon service |
| GET | /v1/mrd/frame | 985 | SWMR-safe frame metadata read |
| GET | /v1/mrd/ingest | 1070 | File metadata read |
| GET | /v1/mrd/latest | 1133 | Reads in-memory cache. 204 if empty |
| GET | /v1/mrd/since | 1156 | Queries index.jsonl by timestamp |

### WebSocket (verified from marshal_ws.hpp)

- Endpoint: `ws://host:port/ws`
- Topics: `_system_` (default), `mrd`, `mrd.ingest`, `mrd.frame`, `pose`
- Commands: `{"subscribe":"topic"}`, `{"unsubscribe":"topic"}`
- Binary messages forwarded to `ingest_payload`
- Per-session async queue, max 1000 messages before dropping

### Type Detection (verified from mrd_type_detector.hpp)

Detection order:
1. HDF5 magic signature (`\x89HDF\r\n\x1a\n`) - 8 bytes
2. `is_acquisition_header()` - checks version (1-10), samples (1-16384), channels (1-128), trajectory (0-3)
3. `is_image_header()` - checks version (1-10), matrix (1-4096), channels (1-128), data_type (1-10)
4. UNKNOWN if none match

**Bug fixed this session**: Previous code had early return `if (size < sizeof(AcquisitionHeader))` at line 193 which rejected valid ImageHeader payloads (198 bytes < 340 bytes) before checking them. Removed.

## Fixes Applied This Session

### 1. mrd_type_detector.hpp - Production bug fix
Removed premature size guard that rejected IMAGE payloads smaller than AcquisitionHeader (340 bytes).

### 2. test_http_handlers.cpp - 6 assertion fixes

| Test | Old (broken) | New (correct) | Why |
|------|-------------|---------------|-----|
| Pose current (empty) | Expected 200 OK | Expects 204 No Content | Cache empty -> no_content |
| Pose update persistence | Checked `fs::exists(poses.jsonl)` | Checks `json_write_queue` not empty | Async writer not running in tests |
| MRD ingest | Sent `"fake mrd content"` | Sends HDF5 magic signature + payload | detect_mrd_type() rejects unrecognized data |
| MRD latest | Wrote file to disk | Populates `state.latest_mrd_json` cache | Handler reads cache, not file |
| MRD latest (empty) | N/A (new test) | Expects 204 No Content | Documents empty-cache behavior |
| Frame append | `img_header.version = 0` | `img_header.version = 1` | is_image_header() requires version >= 1 |
| Invalid payload error | Expected `"payload size does not match header"` | Now works correctly | With version=1, truncated body passes detection and reaches size check |

## What's Actually Remaining (verified, not carried from old docs)

### Genuine gaps (confirmed by reading code)

1. **Logging is stderr-only** - Every log line is `std::cerr <<` or `std::cout <<` with no timestamps, no levels, no structured format. Confirmed across all source files.

2. **Health endpoint is superficial** - `marshal_http.hpp:146-149` returns `{"uptime_s": N}` unconditionally. No filesystem, disk space, or HDF5 health checks. Always returns 200.

3. **CLI parsing can crash** - `marshal_main.cpp:166-213` uses `std::stoull`/`std::stoi` with `parse_size_arg`/`parse_int_arg` helpers that do have try/catch. Actually verified: the parse helpers DO catch exceptions. **This is NOT a real issue** - the CLI parsing is properly guarded.

4. **Detached threads for recon forwarding** - `marshal_http.hpp:588-663` uses `.detach()` for recon HTTP calls. These are fire-and-forget with their own error handling, which is acceptable for the use case but means failures are only logged to stderr.

5. **No TODO/FIXME/HACK in source** - Confirmed zero instances via grep.

### Previously claimed issues that are NOT real

| Claimed Issue | Reality |
|---------------|---------|
| Index-HDF5 race condition | No race: H5Dflush is synchronous (default max_pending_frames=1), metadata queued after |
| Blocking fsync in HTTP handler | Metadata writes are async (json_write_queue), not blocking |
| CLI parsing can crash | parse_size_arg/parse_int_arg have try/catch |

## Documentation Status (after cleanup)

15 stale docs moved to `archive/stale_docs/`. Remaining docs are current:

**Root**: README.md, ARCHIVE_SUMMARY.md, HANDOFF_AGENT_SUMMARY.md, HANDOFF_STATUS.md, HANDOVER_ASYNC_QUEUE.md
**docs/**: README.md, CLIENT_API_REFERENCE.md, USAGE_AND_API.md, DEMO_GUIDE.md, CACHING_ARCHITECTURE.md, HDF5_LOCKING_NOTES.md, IMPROVEMENTS_AND_OPTIMIZATION.md, MRI_DATA_MARSHAL_PRESENTATION.md, SWMR_AND_ROBOT_MARSHAL_OVERVIEW.md, SWMR_CONTINUOUS_BENCH_ANALYSIS.md
