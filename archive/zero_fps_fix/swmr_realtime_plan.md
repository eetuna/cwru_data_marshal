SWMR Realtime Plan (No Implementation)
=====================================

Context
-------
- Goal: SWMR must be the realtime source; clients read from HDF5 while it is being written.
- Constraint: Visibility to SWMR readers occurs only after the writer flushes.
- Requirement: No frame drops and no data loss; realtime visibility should track ingest cadence.

Plan
----
1) Confirm the realtime model.
   - Clients read the latest frame from the SWMR HDF5 file.
   - The writer must flush at the ingest cadence for visibility.

2) Reduce per-frame overhead to a single flush.
   - Keep `H5Dflush` per frame.
   - Drop per-frame `H5Fflush`.
   - Rationale: `H5Dflush` is sufficient to publish new dataset data to SWMR readers
     and is narrower than `H5Fflush`.

3) Remove extra per-frame fsyncs unrelated to SWMR.
   - Move `latest.json` and `index.jsonl` writes off the hot path (background or batch).
   - Serve `/v1/mrd/latest` from in-memory metadata so the endpoint stays accurate even
     if disk metadata lags.

4) Optional: reduce metadata churn further.
   - Extend the dataset in blocks rather than per frame.
   - Track a `valid_frames` scalar dataset so readers know how many frames are real
     inside pre-extended space.ra
   - This reduces `H5Dset_extent` overhead without knowing final dataset size.

5) Validate and benchmark.
   - Run FPS tests and confirm SWMR readers see each new frame after `H5Dflush`.
   - Measure flush latency distribution to confirm it stays under the ingest interval.

Which Flush to Keep (and Why)
-----------------------------
- Keep `H5Dflush` per frame.
  - Flushes the dataset's raw data and relevant metadata so SWMR readers see new frames.
  - Narrower scope; typically lower latency than `H5Fflush`.
- Drop per-frame `H5Fflush`.
  - Flushes file-wide metadata caches and is redundant when `H5Dflush` already publishes
    the dataset changes.
  - Adds latency on every frame with no SWMR visibility benefit.

When `H5Fflush` Should Be Used
------------------------------
Use `H5Fflush` for file-level metadata changes, not for per-frame data appends:
- After creating the file and datasets, before starting SWMR (required by HDF5 SWMR rules).
- After structural changes (new groups/datasets/attributes).
- On close, to ensure file-wide metadata is consistent on disk.

Why a Memory Ring Buffer Does Not Solve SWMR Realtime
-----------------------------------------------------
- SWMR readers only see new data after the writer flushes.
- A memory ring buffer helps only if clients read from memory instead of HDF5.
- If realtime clients must read from SWMR, the ring buffer does not change visibility;
  per-frame flush speed is still the gating factor.

Implications
------------
- Realtime visibility rate equals flush rate.
- If storage cannot flush faster than the ingest cadence, stalls are inevitable.
- Removing extra fsyncs and redundant flushes is the fastest low-risk improvement.

Validation Checklist
--------------------
- Confirm SWMR readers see each new frame after a single `H5Dflush`.
- Confirm no per-frame `H5Fflush` or filesystem `fsync` remains on the hot path.
- Measure flush latency (p50/p95/p99) against ingest interval.
