# MRI Data Marshal: Current Capacity, Limitations, and Improvements

This document summarizes the current performance envelope, bottlenecks, and
practical improvement paths for the MRI Data Marshal demo pipeline.

## Current Architecture (Data Path)

1. `image_streamer` sends frames via HTTP POST at a target interval (`--dt-ms`).
2. MRI Marshal appends each frame to HDF5 in SWMR mode (flush per frame).
3. `viz_client` polls `GET /v1/mrd/latest` and reads the newest frame from HDF5.

This is a storage-first design: live display depends on HDF5 write throughput.

## Capacity Snapshot (Observed)

- Sustained SWMR append: ~19 fps at 50ms interval, ~40 MB/s.
- 10ms target (100 fps): ~0.4 fps in WSL2/devcontainer.
- `GET /v1/mrd/latest`: ~124 RPS, ~8ms latency (reader side).
- Frame sizes tested: 192x192x10–15 (approx 1.5–2.2 MB per frame).
- WSL2 adds ~5–10ms syscall overhead; not the primary bottleneck.

## Why 50 fps (20ms) Drops to 35/29/20 fps

HDF5 SWMR metadata synchronization dominates the per-frame cost. Each append
requires serialized file-locking, metadata updates, and fsync. The per-frame
cost is often ~35–45ms, which exceeds a 20ms budget. Faster hardware helps
slightly, but the algorithmic overhead remains the limiting factor.

## Current Limits (Practical)

- **Real-time SWMR demo**: 50ms intervals (~20 fps) are realistic and stable.
- **20ms intervals**: possible bursts, but sustained rate decays.
- **10ms intervals**: not viable with per-frame SWMR flushes.

## Environmental Factors

- **WSL2 / devcontainer**: adds overhead, but HDF5 metadata cost dominates.
- **Storage location**: Windows-mounted paths worsen latency; Linux FS is better.
- **Frame size**: smaller frames (64x64x10) improve headroom but still hit the
  per-frame metadata wall at sub-50ms intervals.

## Improvement Options

### 1) Decouple Live Display from HDF5 (Recommended)

Goal: keep 50 fps for display while writing HDF5 asynchronously.

Approach:
- Send frames over a fast path (TCP/UDP/ZeroMQ/WebSocket/shared memory).
- Buffer frames in memory.
- Write HDF5 in batches on a background worker.

Pros:
- Live UI remains smooth at higher fps.
- HDF5 still stores all data (with latency).

Tradeoffs:
- Requires queue/backpressure logic.
- Adds memory usage and complexity.

### 2) Batch HDF5 Writes

Goal: reduce metadata syncs per second.

Approach:
- Buffer N frames in RAM.
- Write and flush once per batch (e.g., every 10 frames).

Pros:
- 5–10x fewer metadata syncs.
- Higher throughput on the same hardware.

Tradeoffs:
- Adds latency (e.g., 100ms for 10-frame batches).
- Not strictly real-time per-frame visibility.

### 3) Smaller Frames or Lower Slice Count

Goal: reduce per-frame write cost.

Approach:
- Lower `--size` and/or `--nslices`.

Pros:
- Simple, no code changes.
- Improves stability at 50ms intervals.

Tradeoffs:
- Reduced spatial resolution.

### 4) Alternative Storage Format (Zarr or Binary Stream)

Goal: avoid HDF5 SWMR metadata overhead.

Approach:
- Replace HDF5 SWMR with a format optimized for concurrent appends (e.g., Zarr).
- Or store to a raw binary stream with periodic indexing.

Pros:
- Potentially supports sub-20ms intervals.

Tradeoffs:
- Larger refactor and toolchain changes.

## Recommended Operating Point

For the current demo architecture:
- Use `--dt-ms 50` (20 fps target).
- Keep frames at `128x128x10` or smaller for stable display.
- Run on native Linux FS where possible.

If you need 50 fps sustained, plan for option (1) or (2) above.
