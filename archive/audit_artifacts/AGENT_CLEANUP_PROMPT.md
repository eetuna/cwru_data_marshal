# URGENT: Audit and Fix Previous Agent's Mess

## What Happened

A previous agent (2026-03-06) was asked to audit the codebase and fix failing tests. It made changes in the WRONG location. Here is exactly what went wrong and what needs to be done.

## Repository Structure

This repo has a confusing layout. Understand it FIRST before touching anything.

```
/workspaces/cwru_data_marshal/          ← MAIN GIT REPO (branch: feature/marshal-image-return)
├── .git/                               ← Real git directory
├── *.md (27+ files)                    ← Docs on this branch (MANY are stale, NONE were cleaned)
├── docs/ (7 files)                     ← Docs tracked on this branch
├── docker-compose.*.yml                ← Docker configs tracked on this branch
├── docker/                             ← Dockerfiles tracked on this branch
├── scripts/                            ← Build/demo scripts tracked on this branch
├── NO src/, include/, tests/           ← Source code is NOT at root on this branch
│
└── .worktrees/
    ├── mri_data_marshal/               ← ORPHANED WORKTREE (git metadata broken)
    │   ├── src/                        ← Source code lives HERE
    │   ├── include/                    ← Headers live HERE
    │   ├── tests/                      ← Tests live HERE
    │   ├── CMakeLists.txt              ← Build system HERE
    │   └── *.md                        ← Previous agent modified docs HERE (wrong place)
    │
    └── robot_data_marshal/             ← Separate robot marshal worktree
        └── (server.cpp, clients, etc.)
```

The worktree at `.worktrees/mri_data_marshal/` is ORPHANED - running `git` commands inside it fails with:
```
fatal: not a git repository: /workspaces/cwru_data_marshal/.git/worktrees/mri_data_marshal
```

The source code (src/, include/, tests/) exists ONLY in that orphaned worktree. It does NOT exist on the current branch (`feature/marshal-image-return`) at the repo root.

## What the Previous Agent Changed (ALL in the orphaned worktree)

### Code Changes (need to be rescued)
1. **`.worktrees/mri_data_marshal/include/mrd_type_detector.hpp`** - Removed a bug where `detect_mrd_type()` rejected valid ImageHeader payloads (198 bytes) because of an early-return guard checking `size < sizeof(AcquisitionHeader)` (340 bytes). This is a real bug fix.

2. **`.worktrees/mri_data_marshal/tests/test_http_handlers.cpp`** - Fixed 6 failing test assertions:
   - GET /v1/pose/current: expect 204 not 200 when cache empty
   - POST /v1/pose/update: check async queue instead of file existence
   - POST /v1/mrd/ingest: use HDF5 magic signature instead of "fake mrd content"
   - GET /v1/mrd/latest: populate in-memory cache instead of writing file
   - POST /v1/mrd/frame: set img_header.version=1 (was 0, failing type detection)
   - Added "Get Latest MRD (empty cache)" test section

### Doc Changes (in worktree - may or may not matter)
- Moved 17 root docs + 6 docs/ files to `archive/stale_docs/`
- Updated `HANDOVER_ASYNC_QUEUE.md` (fixed bio cache claims)
- Updated `README.md` (fixed dead doc links)
- Updated `docs/USAGE_AND_API.md` (fixed 404→204 status codes)
- Created `VERIFIED_AUDIT_2026_03_06.md`
- Created `DOCS_AUDIT_PROMPT.md`

### What was NOT touched (but should have been)
- The 27+ markdown files at `/workspaces/cwru_data_marshal/` repo root
- The 7 docs/ files at `/workspaces/cwru_data_marshal/docs/`
- 10 untracked .md files in the repo root (visible in `git status`)

## Your Tasks (in order)

### Task 1: Understand the Repo
- Run `git branch --show-current` in `/workspaces/cwru_data_marshal/`
- Run `git worktree list`
- Determine which branch has the source code (src/, include/, tests/)
- Determine whether `.worktrees/mri_data_marshal/` is actually tracking a branch or is truly orphaned
- Figure out the correct place to make changes

### Task 2: Rescue Code Fixes
The bug fix in `mrd_type_detector.hpp` and the test fixes in `test_http_handlers.cpp` are valid and tested (9/9 pass in the worktree build). They need to be applied to wherever the source code actually belongs. Options:
- If there's a branch that has src/include/tests, check it out and apply the changes
- If the worktree IS the correct working copy, figure out how to fix its git linkage
- Copy the changed files to the right location

### Task 3: Audit ALL Docs Everywhere
Check EVERY markdown file in:
1. `/workspaces/cwru_data_marshal/*.md` (27 files)
2. `/workspaces/cwru_data_marshal/docs/` (7 files)
3. `/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal/*.md`
4. `/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal/docs/`

For each file: is it current, stale, or needs update? Move stale ones to archive.

### Task 4: Report
Give a clear report of:
- The actual repo structure and what lives where
- What code changes were rescued and where they went
- What docs are current vs archived
- What's left to do

## Verified Technical Facts (from source code tracing)

Use these to validate any doc claims:

| Fact | Source |
|------|--------|
| Default flush: max_pending_frames=1, interval=0ms | marshal_state.hpp:63 |
| Bio has in-memory cache (latest_bio_json) | marshal_state.hpp:101-102 |
| Pose has in-memory cache (latest_pose_json) | marshal_state.hpp:104-105 |
| Empty cache GET returns 204 No Content | marshal_http.hpp:171, 337, 1149 |
| JSON writes are async via background thread | marshal_main.cpp:96-154 |
| Smart type detection on /v1/mrd/frame and /v1/mrd/ingest | mrd_type_detector.hpp |
| /v1/mrd/callback exists for async reconstruction | marshal_http.hpp:347 |
| CLI parsing has try/catch guards | marshal_main.cpp:42-68 |
| sizeof(ImageHeader)=198, sizeof(AcquisitionHeader)=340 | verified via compiler |
| H5Dflush is synchronous before metadata is queued | mrd_sink.cpp:357-358, 397 |
| No race condition with default flush policy | flush happens before metadata queue |
| 9 test suites, all passing (after fixes applied) | ctest output |

## False Claims From Old Docs (do NOT repeat these)

| False Claim | Reality |
|-------------|---------|
| "Index-HDF5 race condition" | No race: flush is synchronous, metadata queued after |
| "Blocking fsync in HTTP handler" | Metadata writes are async via json_write_queue |
| "CLI parsing can crash" | parse_size_arg/parse_int_arg have try/catch |
| "Bio has no in-memory cache" | latest_bio_json exists and is used |
| "Flush defaults to 4 frames / 50ms" | Default is 1 frame / 0ms |
