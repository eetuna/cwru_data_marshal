# HANDOVER TO NEXT AGENT - MRI Data Marshal Issues

## CRITICAL ISSUE: Marshal Implementation is Broken

The MRI marshal on the `mri-data-marhsal` branch has INCORRECT default flush parameters that were supposed to be removed from the implementation.

### Current Problem

**Location:** `src/marshal_main.cpp:100-101`

```cpp
std::size_t flush_max_frames = 4;  // ← WRONG VALUES
int flush_max_ms = 50;              // ← WRONG VALUES
```

**What's showing in logs:**
```
marshal listening http=0.0.0.0:8080 ws=0.0.0.0:8090 data=/session-data/run_20260125_001703
max_body=134217728 sink=mrd flush_frames=4 flush_ms=50 shutdown_timeout=30s
```

**The Issue:**
These flush parameters were supposed to be REMOVED from the implementation entirely according to previous work, but they're still there with wrong hardcoded defaults.

---

## Your Task

### Step 1: Review All Documentation

**CRITICAL:** Read ALL documentation files in the `mri-data-marhsal` branch to understand what was done previously:

```bash
git checkout mri-data-marhsal

# Read these files in order:
cat docs/HANDOVER_SWMR_IMPLEMENTATION.md
cat docs/CORRECTED_ARCHIVE_IMPLEMENTATION.md
cat docs/IMPLEMENTATION_STATUS.md
cat docs/QUICK_REFERENCE.md
cat docs/PROMPT_FOR_NEXT_AGENT.md
```

These docs explain:
- What the SWMR metadata-only architecture should be
- What changes were made (or should have been made)
- Why flush parameters are wrong
- What the correct implementation should look like

### Step 2: Understand the Flush Issue

Based on the documentation, determine:
1. **Should flush parameters exist at all?**
   - If yes, what should the values be?
   - If no, how should they be removed?

2. **What is the correct behavior?**
   - Check if flush was part of the old binary mode that was removed
   - Verify if SWMR mode needs different flush handling

### Step 3: Fix the Implementation

After reading the docs, you'll know:
- Whether to remove flush_max_frames and flush_max_ms entirely
- Or update them to correct values
- Or change the implementation approach

**Files to potentially modify:**
- `src/marshal_main.cpp` (lines 100-101, 181-182, 285-286)
- `src/mrd_sink.cpp` (flush_policy implementation)
- `src/marshal_http.hpp` (if related to metadata-only changes)

### Step 4: Verify Against Documentation

Cross-check your changes against:
- `CORRECTED_ARCHIVE_IMPLEMENTATION.md` - Has the REAL working code
- `HANDOVER_SWMR_IMPLEMENTATION.md` - Complete implementation guide
- `IMPLEMENTATION_STATUS.md` - What should be done vs what is done

### Step 5: Test

```bash
cd /workspaces/cwru_data_marshal
mkdir -p build && cd build
cmake .. -DBUILD_TESTING=ON
cmake --build . --parallel
ctest --output-on-failure
```

Expected: All tests pass ✓

### Step 6: Update Docker

After fixing the marshal, rebuild the Docker image:
```bash
git checkout main
./scripts/build-client-images.sh
```

Test the demo:
```bash
./scripts/demo-docker.sh
```

Verify the marshal logs show correct behavior (no wrong flush values).

---

## Context from Current Session

### What We Accomplished Today

1. ✅ **Fixed robot marshal** - Added command-line args to `server.cpp`
   - Branch: `robot_data_marshal_with_catheter_system_components`
   - Commit: `0958604` - Added `--http`, `--host`, `--port` arguments
   - Eliminates sed patch hack

2. ✅ **Updated main branch** - Removed sed patches
   - Branch: `main`
   - Commits: `c2c1180`, `0a6a375`
   - Updated Dockerfile.robot and docker-compose.demo.yml
   - Fixed .gitignore

3. ✅ **Pushed all branches** - To origin and upstream

### What's Broken

4. ❌ **MRI marshal has wrong flush defaults** - Needs investigation
   - Branch: `mri-data-marhsal`
   - Issue: Hardcoded flush_frames=4, flush_ms=50
   - Previous agent made changes that were "lost when worktree was deleted"
   - Full documentation exists but implementation doesn't match

---

## What Happened - Git History Analysis

**User's report:** "they deleted the latest marshal and brought back the older one"

**Key commits to investigate:**

1. **057fa6b** - "fixed the marshal and viz client" (Jan 24 23:31)
   - Added archive/http_binary_mode/marshal_http_archive.hpp
   - Added 5 new docs (CORRECTED_ARCHIVE_IMPLEMENTATION.md, etc.)
   - Modified src/marshal_http.hpp
   - This is where the SWMR metadata-only changes SHOULD have been applied

2. **4de30c4** - "Add GET endpoints for /v1/mrd/frame and /v1/mrd/ingest"
   - This might have added back old endpoints or overwritten the new ones

**What to check:**

```bash
# Compare the current marshal_http.hpp with what it should be
git show 057fa6b:src/marshal_http.hpp > /tmp/marshal_http_057fa6b.hpp
diff /tmp/marshal_http_057fa6b.hpp src/marshal_http.hpp

# Check if the archive file exists (it should)
ls -la archive/http_binary_mode/marshal_http_archive.hpp

# Check what 4de30c4 actually changed
git show 4de30c4 --stat
git show 4de30c4 src/marshal_http.hpp
```

**Hypothesis:** Commit 4de30c4 may have overwritten the SWMR metadata-only changes from 057fa6b, bringing back old binary-mode code with flush parameters.

**How to fix:**
1. Check the diff between 057fa6b and current HEAD
2. See if marshal_http.hpp was reverted to old implementation
3. Re-apply the correct SWMR metadata-only changes from docs
4. Remove flush parameters entirely (they belong to old binary mode)

## Important Notes

- **Don't guess** - Read the documentation files first AND check git history
- The docs were written by previous agents who understood the architecture
- `CORRECTED_ARCHIVE_IMPLEMENTATION.md` has REAL C++ code, not just docs
- The issue might be that SWMR changes were never fully applied
- Or a later commit reverted the changes back to old binary mode
- Or the flush implementation conflicts with metadata-only mode
- **User says: someone deleted the latest marshal and brought back an older one** - investigate commits 057fa6b vs 4de30c4

---

## Current State

**Branches:**
- `main` - Clean, ready for demo builds
- `robot_data_marshal_with_catheter_system_components` - Fixed, pushed to upstream
- `mri-data-marhsal` - **HAS ISSUES**, needs investigation based on docs

**Working Directory:** `/workspaces/cwru_data_marshal`

**Current Branch:** `mri-data-marhsal`

Good luck! Start by reading those docs - they have all the answers.
