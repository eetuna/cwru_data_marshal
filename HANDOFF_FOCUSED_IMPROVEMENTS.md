# Handoff: Focused Production Improvements Plan

**Date:** 2026-01-06
**Branch:** `feature/viz-single-slice-navigator`
**Commit:** `ed8fadf` (PRODUCTION_IMPROVEMENTS_ANALYSIS.md added)
**Status:** Ready for implementation planning

---

## Context

A comprehensive production readiness analysis has been completed (see `PRODUCTION_IMPROVEMENTS_ANALYSIS.md`). The analysis identified 7 improvement opportunities across operational, safety, and performance dimensions.

**Key Finding:** The `feature/viz-single-slice-navigator` branch has **excellent data safety** (SWMR, atomic writes, thread safety all correct) but needs **operational improvements** for production use.

---

## User Profile

**Deployment Scenario:**
- Single-scan, multi-protocol MRI data collection
- Duration: Minutes to hours per session (NOT days-long)
- Typical streams: 5-20 different sequences per session
- Use case: Research data capture with quality control

**Constraints:**
- Performance-sensitive (50 fps baseline acceptable)
- Data integrity critical (research-grade reliability)
- Operational simplicity preferred (private network, no TLS needed)

---

## Focused Task

Based on the deployment profile, the user is NOT interested in fixing all 7 issues. Instead, they want a **focused plan for the 2 CRITICAL items most relevant to their use case:**

### Critical Item #1: Graceful Shutdown Handler
**Location:** `src/marshal_main.cpp:149`
**Risk:** Data loss on SIGTERM/SIGINT
**Why it matters:** Multi-hour sessions could be interrupted; graceful shutdown prevents HDF5 corruption
**Effort:** 2-4 hours
**Performance cost:** None

**Current problem:**
```cpp
ioc.run();  // Blocks forever until process killed
// No SIGTERM/SIGINT handlers
// In-flight requests abandoned
// HDF5 files not properly flushed on shutdown
```

**What needs to be done:**
1. Install signal handlers (SIGTERM, SIGINT)
2. Stop accepting new connections
3. Drain in-flight requests (with timeout)
4. Force flush all pending HDF5 data
5. Close all files cleanly
6. Exit gracefully

---

### Critical Item #2: Index-HDF5 Consistency Fix
**Location:** `src/mrd_sink.cpp:528-536`
**Risk:** Metadata shows frames not yet visible in HDF5
**Why it matters:** Ensures consistency guarantees for multi-protocol sessions
**Effort:** 1-2 hours
**Performance cost:** ZERO (reorders already-happening flush)

**Current problem:**
```cpp
// Current order:
auto result = stream_state->file->append_frame(data, bytes);  // HDF5 write
append_line(sink.index_root / "index.jsonl", entry.dump());    // Index write (fsynced)
write_atomic(sink.index_root / "latest.json", latest.data()...); // Latest write (fsynced)
// H5Dflush happens later per flush policy

// Race window: Index/latest written but HDF5 not yet flushed
// Client reads index, finds frame, opens HDF5, frame not visible yet
```

**What needs to be done:**
1. Ensure HDF5 flush completes before metadata writes
2. Options:
   - Force `H5Dflush()` before `append_line()` call
   - OR verify flush completed before proceeding
   - OR write metadata, then force HDF5 flush
3. Add test to verify ordering is correct

---

## Items to SKIP (Not Relevant to User)

The analysis identified 5 additional improvements, but they are **NOT needed for this user's deployment:**

| Item | Why Skip |
|------|----------|
| Stream map bounds | Not relevant: ~5-20 streams per session, won't hit unbounded limit |
| Timestamped logging | Nice to have, not critical for short sessions |
| Health endpoint enhancement | Operational monitoring, not needed for single-scan use |
| Fix silent failures | Good practice, but not blocking for research use |
| HDF5 type handle leak | Minor resource leak, negligible for short sessions |

---

## Deliverable

The next Claude Agent should create:

1. **Detailed implementation plan** for the 2 critical items
2. **Code change specifications** showing exactly what to modify
3. **Test verification steps** to confirm fixes work
4. **Effort breakdown** with subtasks
5. **Risk analysis** for each change

**Format:** Markdown document suitable for code review

**File location:** `HANDOFF_FOCUSED_IMPLEMENTATION_PLAN.md`

---

## Success Criteria

The implementation plan should be:
- ✅ **Specific:** Exact code locations and changes needed
- ✅ **Actionable:** Clear steps a developer can follow
- ✅ **Tested:** How to verify each fix works
- ✅ **Safe:** No performance regressions, no new issues introduced
- ✅ **Focused:** Only the 2 critical items, ignore the rest

---

## Reference Documents

- `PRODUCTION_IMPROVEMENTS_ANALYSIS.md` - Full analysis (467 lines)
  - Sections: What's Good, What Could Be Improved, Performance Impact
  - Detailed technical analysis of each issue
  - Effort estimates and trade-off analysis

- `PRODUCTION_READINESS_COMPARISON.md` - Branch comparison
  - Comparison of viz-single-slice-navigator vs performance-optimization
  - Safety analysis and risk assessment

---

## Next Steps

1. **Read** the provided analysis documents
2. **Understand** the 2 critical items in detail
3. **Design** the implementation approach
4. **Write** the focused implementation plan
5. **Create** the handoff document for execution

---

## Questions for Implementation

The implementation plan should address:

1. **Graceful Shutdown:**
   - Which signal handler library to use (std::signal vs Boost.Signals)?
   - How to safely stop io_context?
   - Timeout for draining in-flight requests?
   - How to ensure all files are flushed before exit?

2. **Index-HDF5 Fix:**
   - Where exactly to insert the flush check?
   - Should we force flush every frame or check flush status?
   - How to handle flush errors?
   - What test case verifies the fix?

---

## Files to Modify (Preliminary)

| File | Changes | Priority |
|------|---------|----------|
| `src/marshal_main.cpp` | Signal handlers, shutdown logic | P0 |
| `src/mrd_sink.cpp` | Index-HDF5 ordering fix | P0 |
| `tests/test_mrd_sink.cpp` | Add consistency verification test | P0 |

---

## Timeline

- **Investigation:** 2 hours
- **Design:** 1 hour
- **Implementation:** 3-6 hours
- **Testing:** 1-2 hours
- **Total:** ~1 day of focused work

---

**Ready for handoff to implementation planning agent.**
