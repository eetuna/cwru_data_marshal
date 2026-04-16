# MRI Latest Image Executive Summary — 2026-04-16

## Context

This summary accompanies the detailed handoff in [MRI_LATEST_IMAGE_DIAGNOSIS_AND_OPTIONS_2026-04-16.md](MRI_LATEST_IMAGE_DIAGNOSIS_AND_OPTIONS_2026-04-16.md).

It is intended as the shortest accurate briefing for a follow-up reviewer.

## Current Status

The MRI marshal latest-image performance/stability issue is **not solved yet**.

Two real server-side experiments were implemented and validated:
- `f7bb797` on `perf/latest-file-reuse`
- `0c30408` on `perf/latest-slot-reuse`

Both passed focused correctness/integration checks.
Neither became a decisive performance breakthrough versus V0.

## Commits to Know

### V0 baseline
- `837a101`

### First committed experiment
- Branch: `perf/latest-file-reuse`
- Commit: `f7bb797`
- Message: `feat: reuse closed recon result files for latest publication`

### Second committed experiment / diagnosis checkpoint
- Branch: `perf/latest-slot-reuse`
- Commit: `0c30408`
- Message: `experiment: add slot-based latest path and cadence diagnostics`

## What Was Learned

### 1. Latest-file promotion is not the main bottleneck
Instrumentation showed:
- `latest_promote_ms` was almost always `0–1 ms`
- latest publish/promotion itself is basically free

### 2. Queue buildup is not the main issue
Instrumentation showed:
- recorder queue depth stayed small, usually around `1`
- max queue depth was only around `2–3`

This does not look like a backlog-drowning problem.

### 3. The strongest remaining signal is duplicated recon append churn
Aggregate cadence logs showed approximately:
- `70–93` live recon HDF5 appends/sec
- `70–93` latest-result HDF5 appends/sec
- `12–19` latest publishes/sec

Per-op cost is small, but the work is duplicated and repeated constantly.

## Best Current Diagnosis

The most credible remaining server-side issue is:

marshal performs duplicate recon-side HDF5 append work on the hot path
- once into live recon history
- once into the latest-result artifact

This cumulative repeated small-work churn appears more important than latest-file rename mechanics.

## What Should Not Be the Focus

The evidence argues against spending primary effort on:
- more latest-file rename/promotion tweaks
- more temp-file naming variants
- more queue/backpressure experiments as the main strategy
- client-side changes as a prerequisite for the fix

## Recommended Direction

The strongest remaining direction is:
- remove or restructure the duplicate recon append path
- avoid writing the same logical recon result twice into separate HDF5 destinations on the hot path

The detailed doc describes the main server-side options, with the strongest option being:
- make one closed recon result artifact the source of truth, then derive latest/archive from that instead of double-appending recon data

## Practical Repository State

- `perf/latest-file-reuse` is a clean committed checkpoint at `f7bb797`
- `perf/latest-slot-reuse` is a clean committed experiment checkpoint at `0c30408`
- no session changes should remain uncommitted after the requested cleanup/switch-back step is completed

## One-Line Takeaway

The work so far strongly suggests the problem is **not** latest-file promotion itself; it is the duplicated high-frequency recon HDF5 append churn inside marshal.
