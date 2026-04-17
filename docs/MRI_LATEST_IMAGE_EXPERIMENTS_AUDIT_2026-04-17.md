# MRI Latest-Image Speed Experiments — Audit

**Date:** 2026-04-17
**Base commit:** `837a101` ("checkpoint: snapshot MRI marshal latest-image audit state", 2026-04-15 17:58Z)
**Worktree:** `.worktrees/mri_data_marshal`

All experiments below branch off the same base `837a101` (the "v0" snapshot of the
latest-image audit state). None have been merged. Commits are preserved; this
document captures the order in which they were tried and what each attempted.

## Linear order (chronological)

| # | Date (UTC)         | Branch                               | Tip commit | Parent   | Files touched                                                                 |
|---|--------------------|--------------------------------------|------------|----------|-------------------------------------------------------------------------------|
| 1 | 2026-04-15 18:32   | `perf/latest-image-shared-buffers`   | `9d8661a`  | 837a101  | `src/latest_image_writer.cpp` (+24 / -4)                                      |
| 2 | 2026-04-16 00:04   | `perf/latest-file-reuse`             | `f7bb797`  | 837a101  | `latest_image_writer.{cpp,hpp}`, `live_image_recorder.{cpp,hpp}`, `live_image_store.hpp`, `marshal_state.hpp`, `tests/test_http_handlers.cpp` |
| 3 | 2026-04-16 00:46   | `perf/latest-slot-reuse`             | `0c30408`  | f7bb797 (#2) | `latest_image_writer.cpp`, `live_image_recorder.{cpp,hpp}`, `live_image_store.hpp`, `marshal_state.hpp` |
| 4 | 2026-04-17 15:48   | `perf/latest-bulk-prealloc`          | `ba80258`  | 837a101  | `src/latest_image_writer.cpp`, `clients/viz_client/viz_client_main.cpp`       |

## What each experiment tried

### 1. `perf/latest-image-shared-buffers` — bounded backpressure
- Adds `max_in_flight_jobs = 1` cap on the writer's work queue.
- Introduces `space_cv` condition variable so producers block in `enqueue`
  instead of growing the deque unboundedly.
- Adds `active_jobs` counter so "in-flight" includes the job currently being
  written (not just queued).
- Shutdown uses `notify_all` on both CVs and early-returns blocked producers.
- **Goal:** stop unbounded queue growth when the writer can't keep up.

### 2. `perf/latest-file-reuse` — reuse closed recon result files
- Teaches the marshal to republish already-closed recon result files as
  "latest" instead of writing new H5 files from memory.
- Adds `live_image_recorder` alongside the existing store; tests updated.
- **Goal:** skip the H5 write entirely for the latest-image path by pointing
  at a file the recon already finalized.

### 3. `perf/latest-slot-reuse` — slot-based latest path + cadence diagnostics
- Builds on #2 (its parent is `f7bb797`, not the base).
- Adds slot-based pathing so "latest" has a fixed set of rotating slot files
  rather than one growing path.
- Adds cadence diagnostics — instrumentation to measure how often latest
  gets republished.
- **Goal:** bound the number of files on disk and measure real cadence.

### 4. `perf/latest-bulk-prealloc` — bulk writer + preallocated datasets
- Largest change (+477 / -75).
- Rewrites `latest_image_writer.cpp` around preallocated datasets and bulk
  writes, rather than per-image open/close cycles.
- Updates `viz_client` polling loop to match the new layout.
- **Goal:** amortize H5 file open + dataset creation costs across many
  frames.

## Revert bookmark (not an experiment)

`checkpoint/latest-writer-queue-unbounded` (`34569ff`, 2026-04-17 15:54) was
**deleted during this audit**.

It branched off experiment #1 and its sole commit reverted every change that
experiment #1 introduced, producing a tree identical to `837a101`. It was a
safety checkpoint ("what does code look like without the backpressure
experiment?"), not a new speed attempt. Deleted because it had no unique
content — the equivalent state is always one `git revert 9d8661a` away.

## Worktree cleanup performed

Prior to this audit three MRI worktrees existed:

- `.worktrees/mri_data_marshal` — the real one (kept)
- `.worktrees/mri_latest_file_reuse` — scratch worktree, removed
- `.worktrees/mri_v0_benchmark` — scratch worktree, removed

Both removed worktrees pointed at the base `837a101` in detached HEAD and
held no unique refs. The four experiment branches above are untouched by
this cleanup and remain available via `git checkout` inside
`.worktrees/mri_data_marshal`.

## How to inspect an experiment

```bash
cd .worktrees/mri_data_marshal
git checkout perf/latest-image-shared-buffers    # or any of the four
git diff 837a101..HEAD -- src/latest_image_writer.cpp
git checkout 837a101                              # back to base
```
