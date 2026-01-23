# Handoff Report: Next Codebase Audit

**Date**: 2026-01-07
**Branch**: `feature/graceful-shutdown`
**Base Branch**: `fix/critical-bugs-from-audit`
**Purpose**: Request comprehensive codebase audit for bugs, performance issues, and optimization opportunities

---

## What Was Just Completed

### Bug Fixes (Branch: `fix/critical-bugs-from-audit`)
We fixed 4 critical bugs identified in the previous audit:

1. ✅ **Bug 1: Memory Leak - Unbounded Stream Map** (CRITICAL)
   - Added LRU eviction with `last_accessed` timestamp tracking
   - Implemented `cleanup_idle_streams()` method
   - Streams idle for >10 minutes are automatically evicted

2. ✅ **Bug 3: Double WebSocket Emit** (HIGH)
   - Removed duplicate `ws_emit()` call in `mrd_sink.cpp:540`
   - Eliminated duplicate message broadcasts

3. ✅ **Bug 4: WebSocket Broadcast Race Condition** (HIGH)
   - Implemented copy-and-release pattern in `broadcast()` and `broadcast_to()`
   - Prevents deadlock when holding multiple mutexes

4. ✅ **Bug 2: WebSocket Destructor Race** (CRITICAL)
   - Enhanced destructor safety leveraging Bug 4 fixes
   - Prevents use-after-free during client disconnection

### Graceful Shutdown Implementation (Branch: `feature/graceful-shutdown`)
Added production-ready async signal handling:

1. ✅ **Async Signal Handling**
   - Uses `boost::asio::signal_set` for thread-safe SIGINT/SIGTERM handling
   - Configurable timeout via `--shutdown-timeout-sec` (default: 30s)
   - Force exits if flush hangs (prevents infinite hang on broken filesystems)

2. ✅ **HDF5 Flush Methods**
   - Added `MrdFile::flush()` - force immediate flush
   - Added `MrdSink::flush_all()` - flush all active streams
   - Thread-safe implementation with copy-and-release pattern

3. ✅ **Demo Script Updates**
   - Updated `run_demo_simultaneous.sh` with graceful shutdown
   - Sends SIGTERM first, waits for graceful exit
   - Falls back to SIGKILL after timeout
   - Single `SHUTDOWN_TIMEOUT_SEC` variable controls all timing

4. ✅ **Performance Optimization**
   - Set `--flush-frames 4` (batch 4 frames before flush)
   - Set `--flush-ms 50` (safety net for max latency)
   - Reduced shutdown timeout to 15s for local storage
   - ~75% reduction in fsync overhead

---

## Current Branch Status

### Branch: `feature/graceful-shutdown`
```
f7d0773 refactor: Simplify shutdown config to single timeout variable
59488ff perf: Optimize flush settings for better performance
f17e221 fix: Update demo script to use --shutdown-timeout-sec parameter
9c28266 feat: Add async graceful shutdown with configurable timeout
b0c4012 fix: Restore reasonable frame interval in demo script
9193e2b fix: Address 4 critical WebSocket and memory bugs (audit findings)
```

### Files Modified
```
include/mrd_sink.hpp              - Added flush_all(), cleanup_idle_streams()
src/mrd_sink.cpp                  - Implemented flush methods, LRU tracking
src/marshal_ws.hpp                - Fixed broadcast race conditions
src/marshal_main.cpp              - Added async graceful shutdown
scripts/run_demo_simultaneous.sh  - Updated with graceful shutdown handling
```

### Build Status
✅ All files compile cleanly
✅ Binary size: 5.0MB (healthy)
✅ No warnings or errors

---

## What the Next Agent Should Do

### Primary Task: Comprehensive Codebase Audit

**Objective**: Identify remaining bugs, performance bottlenecks, code quality issues, and optimization opportunities.

**Scope**: Entire codebase (focus on branches: `feature/graceful-shutdown`, `fix/critical-bugs-from-audit`, `main`)

### Areas to Audit

#### 1. **Bug Detection** (Critical)
Look for:
- Race conditions and deadlocks
- Memory leaks (beyond the stream map we fixed)
- Use-after-free issues
- Buffer overflows or underflows
- Uninitialized variables
- Exception safety issues
- Resource leaks (file handles, mutexes, etc.)
- Off-by-one errors
- Integer overflow/underflow
- Null pointer dereferences

**Files to scrutinize:**
- `src/marshal_ws.hpp` - WebSocket session management
- `src/mrd_sink.cpp` - HDF5 file operations
- `src/marshal_http.hpp` - HTTP request handling
- `include/mrd_io.hpp` - File I/O operations

#### 2. **Performance Issues** (High Priority)
Look for:
- Unnecessary allocations in hot paths
- Inefficient algorithms (O(n²) where O(n log n) possible)
- Lock contention hotspots
- Blocking I/O in async contexts
- Redundant string copies
- Excessive logging in production paths
- Cache-unfriendly data structures
- Missing move semantics

**Hot paths to analyze:**
- `MrdSink::append_frame()` - called for every frame
- `broadcast()` / `broadcast_to()` - called on every WebSocket message
- `write_atomic()` / `append_line()` - called frequently

#### 3. **Code Quality Issues** (Medium Priority)
Look for:
- Inconsistent error handling patterns
- Missing `const` correctness
- Raw pointers where smart pointers should be used
- Magic numbers without named constants
- Missing documentation for complex logic
- Overly complex functions (>50 lines)
- Duplicate code that could be refactored
- Unused variables/functions
- Missing bounds checking
- Thread safety documentation gaps

#### 4. **Architecture & Design** (Medium Priority)
Look for:
- Tight coupling between modules
- Violations of single responsibility principle
- Missing abstractions
- Overly generic or overly specific interfaces
- Inconsistent naming conventions
- Global state (we use some in signal handler - is it necessary?)

#### 5. **Known Issues NOT Fixed Yet** (From Previous Audit)
These were deprioritized before but should be reconsidered:

- **No graceful shutdown** ✅ FIXED (we just added it!)
- **Blocking fsync in HTTP handler** - Still blocks, needs async queue
- **HDF5 Type Handle Leak** - Static `complex_type` never freed
- **Unchecked CLI parsing** - `stoull`/`stoi` can throw, no try/catch
- **Synchronous WebSocket writes** - Could use async writes
- **CURL timeout too short** - viz_client has 100ms timeout
- **HTTP body limit error version** - Hardcoded to HTTP/1.1
- **Redundant JSON dump calls** - `entry.dump()` called twice

---

## Audit Output Format

Please provide:

### 1. **Executive Summary**
- Total issues found (categorized by severity)
- Top 5 most critical issues
- Estimated effort to fix critical issues

### 2. **Critical Issues (P0)**
For each issue:
- **File & Line Number**: `src/file.cpp:123`
- **Severity**: CRITICAL / HIGH / MEDIUM / LOW
- **Category**: Bug / Performance / Code Quality / Design
- **Description**: What's wrong
- **Impact**: What could go wrong
- **Example**: Code snippet showing the issue
- **Fix**: Recommended solution

### 3. **Recommended Fixes (Prioritized)**
- Immediate fixes (do now - critical bugs)
- Short-term fixes (this week - high priority)
- Long-term improvements (next sprint - nice to have)

### 4. **Performance Analysis**
- Profiling bottlenecks (if any found)
- Optimization opportunities
- Expected performance gains

### 5. **Files Changed Summary**
Table showing which files have issues and issue counts

---

## Context for the Audit

### System Architecture
- **Purpose**: Real-time MRI data streaming server
- **Performance Target**: 50 fps sustained throughput
- **Data Flow**: HTTP POST → HDF5 file + index.jsonl + latest.json + WebSocket broadcast
- **Critical Path**: `append_frame()` → HDF5 write → flush → metadata write → WS broadcast

### Production Requirements
- ✅ **Data Safety**: HDF5 files must not corrupt
- ✅ **Real-time**: <50ms latency per frame (achieved with flush-frames=4, flush-ms=50)
- ✅ **Reliability**: Server must not crash on client disconnect
- ✅ **Graceful Shutdown**: Clean exit on SIGTERM (now implemented)
- ⚠️ **Monitoring**: Basic `/health` endpoint exists but limited
- ⚠️ **Logging**: Minimal stderr logging only

### Technology Stack
- C++20
- Boost.Asio (async I/O)
- Boost.Beast (HTTP/WebSocket)
- HDF5 1.10.7
- ISMRMRD (MRI data format)
- nlohmann/json

### Recent Changes (Be Aware)
1. We just added async graceful shutdown - audit this new code carefully!
2. We fixed WebSocket race conditions - verify the fix is correct
3. We added LRU stream cleanup - check for memory leaks in this logic
4. We changed flush settings from 1→4 frames - verify this is safe

---

## Testing Instructions

If you want to test the current code:

```bash
# Build
cd /workspaces/cwru_data_marshal/build
ninja marshal

# Run server
./build/marshal --http 127.0.0.1:8080 --data /tmp/test_data

# Stream test data (in another terminal)
./build/image_streamer --http http://127.0.0.1:8080 --frames 100 --dt-ms 50

# Test graceful shutdown
# Press Ctrl+C and observe shutdown messages

# Run demo (tests everything together)
./scripts/run_demo_simultaneous.sh
```

---

## Questions to Answer

1. **Are there any remaining race conditions** in WebSocket handling?
2. **Is the graceful shutdown implementation correct?** (we just added it)
3. **Are there memory leaks** beyond the stream map we fixed?
4. **Can performance be improved** without sacrificing data safety?
5. **Is the flush policy safe?** (flush-frames=4, flush-ms=50)
6. **Are there any undefined behaviors** lurking in the code?
7. **Is error handling consistent** across the codebase?
8. **Should we use async I/O** for HDF5 writes? (currently blocking)

---

## Files Requiring Extra Scrutiny

### High Priority (Critical Path)
1. `src/mrd_sink.cpp` - Core data ingestion logic (just modified)
2. `src/marshal_ws.hpp` - WebSocket handling (just fixed races)
3. `src/marshal_main.cpp` - Signal handling (just added async shutdown)
4. `include/mrd_io.hpp` - Atomic file writes

### Medium Priority (Important but Less Critical)
5. `src/marshal_http.hpp` - HTTP request handling
6. `src/marshal_state.hpp` - Shared state management

### Lower Priority (Clients, Scripts)
7. `clients/viz_client/viz_client_main.cpp`
8. `clients/image_streamer/image_streamer_main.cpp`
9. `scripts/*.sh` - Demo scripts

---

## Don't Audit (Out of Scope)

- `archive/` - Old documentation backups
- `tests/` - Test code (unless you find bugs in tests)
- `docs/` - Documentation
- `.github/` - CI configuration
- Third-party dependencies (Boost, HDF5)

---

## Success Criteria

A successful audit will:
1. ✅ Identify at least 10 specific issues with file:line references
2. ✅ Categorize issues by severity (P0/P1/P2/P3)
3. ✅ Provide concrete fix recommendations
4. ✅ Estimate effort for each fix
5. ✅ Highlight any critical safety issues that must be fixed immediately
6. ✅ Suggest performance optimizations that are production-safe

---

## How to Use This Handoff

**For the next Claude agent:**

1. Read this entire document first
2. Read `CODEBASE_AUDIT_REPORT.md` (previous audit from commit `cca845b`)
3. Focus on areas NOT covered in the previous audit
4. Pay special attention to code we just modified (graceful shutdown, race fixes)
5. Use the Grep, Glob, and Read tools to explore the codebase
6. Write findings to `CODEBASE_AUDIT_REPORT_2.md`
7. Prioritize findings by severity and effort
8. Recommend which fixes to do immediately

---

## Contact

This handoff was prepared by Claude Opus 4.5 on 2026-01-07.

For questions about the recent changes:
- Bug fixes: See commit `9193e2b`
- Graceful shutdown: See commits `9c28266`, `f17e221`, `f7d0773`, `59488ff`
- Previous audit: See `CODEBASE_AUDIT_REPORT.md`
