# Documentation Audit Prompt

## Task

Audit EVERY markdown and text file in the entire CWRU Data Marshal repository. Classify each as CURRENT, STALE, or NEEDS_UPDATE. Move stale files to `archive/stale_docs/`. Fix inaccurate claims in files that stay.

## Scope

You must check ALL of these locations - not just one:

### 1. Main repo root: `/workspaces/cwru_data_marshal/`
There are 27+ markdown files here including untracked ones. Many are from previous agent sessions (HANDOFF_*, HANDOVER_*, AUDIT_*, etc.) and reference documentation (ARCHITECTURE.md, FLOWCHARTS.md, etc.). Check every single one.

### 2. MRI worktree root: `/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal/`
Root-level markdown files. Some were already cleaned on 2026-03-06 but verify.

### 3. MRI worktree docs/: `/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal/docs/`
10 remaining doc files. Some have known stale claims (e.g., 404 vs 204 status codes).

### 4. MRI worktree archive/: Check `archive/stale_docs/` is correctly organized.

### 5. Robot worktree: `/workspaces/cwru_data_marshal/.worktrees/robot_data_marshal/`
README.md and any other docs.

### 6. Existing archives: `archive/root_docs/`, `archive/docs_backup/`, etc.

## How to Verify

DO NOT trust claims in documentation files. For every technical claim, trace it to the actual source code:

- `src/marshal_http.hpp` - HTTP handlers and status codes
- `src/marshal_state.hpp` - State structure, defaults, flush policy
- `src/marshal_main.cpp` - CLI parsing, background threads, startup
- `src/marshal_ws.hpp` - WebSocket implementation
- `src/mrd_sink.cpp` - HDF5 SWMR engine
- `include/mrd_type_detector.hpp` - Smart type detection
- `include/mrd_io.hpp` - Utility functions
- `include/common/pose.hpp` - Pose structures

## Verified Facts (confirmed against source on 2026-03-06)

Use these as ground truth - each was traced to specific source lines:

| Fact | Source Location |
|------|----------------|
| Default flush: max_pending_frames=1, interval=0ms | marshal_state.hpp:63 |
| Bio has in-memory cache (latest_bio_json) | marshal_state.hpp:101-102 |
| Pose has in-memory cache (latest_pose_json) | marshal_state.hpp:104-105 |
| Empty cache GET returns 204 No Content | marshal_http.hpp:171, 337, 1149 |
| JSON writes are async via background thread | marshal_main.cpp:96-154 |
| Smart type detection on /v1/mrd/frame and /v1/mrd/ingest | mrd_type_detector.hpp |
| /v1/mrd/callback exists for async reconstruction | marshal_http.hpp:347 |
| CLI parsing has try/catch guards | marshal_main.cpp:42-68 |
| sizeof(ImageHeader)=198, sizeof(AcquisitionHeader)=340 | verified via compiler |
| 9 test suites, all passing | ctest output |
| No TODO/FIXME/HACK in source | grep confirmed |
| H5Dflush is synchronous before metadata is queued | mrd_sink.cpp:357-358, 397 |

## Previously Debunked Claims (from old audit docs)

These were stated in old docs but are NOT true:

| False Claim | Reality |
|-------------|---------|
| "Index-HDF5 race condition" | No race: flush is synchronous (max_pending_frames=1), metadata queued after |
| "Blocking fsync in HTTP handler" | Metadata writes are async via json_write_queue |
| "CLI parsing can crash on invalid input" | parse_size_arg/parse_int_arg have try/catch |
| "Bio has no in-memory cache" | latest_bio_json exists and is used |
| "Flush policy defaults to 4 frames / 50ms" | Default is 1 frame / 0ms |

## Output

For each file, state:
1. Full path
2. CURRENT / STALE / NEEDS_UPDATE
3. If stale: move to archive/stale_docs/ (preserve, don't delete)
4. If needs update: list specific lines with wrong claims and what they should say
5. If current: confirm it's accurate

After the audit, update the verified audit doc at:
`/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal/VERIFIED_AUDIT_2026_03_06.md`
