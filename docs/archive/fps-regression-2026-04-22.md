# FPS regression 2026-04-22: investigation, fixes, and future ECG-with-image design

## Symptom

Docker 4-container demo (cwru/mri-marshal + mock-recon + kspace-streamer + viz-client):

- Baseline (branch `perf/latest-bulk-prealloc`): viz_client 15–20 fps, stable.
- Current tip (`16fc6fb` on `fix/marshal-dump-mode-switch-2026-04-19`): 8–10 fps, high variance.
- Native (no Docker): 42 fps.

Config: `IMAGE_SLICES=5`, `KSPACE_INTERVAL=0.04`, `--ecg` on.

## Investigation method

Added free-running counters exposed via `GET /debug/perf`:

- `recv.{scanner_images, recon_images, scanner_waveforms}`
- `publish_attempts.{scanner, recon}`
- `latest_writer.{enqueued, coalesced, dropped_oldest, completed, failed, max_queue_depth, last/max_write_us, last/max_drain_lag_us}`

Took paired t=5s / t=35s snapshots and computed deltas to derive rates and lost-frame counts.

## What we measured (Δ over 30s, Docker demo)

| Config | recon/s | attempts/s | completed/s | coalesced | dropped_oldest | max_q | max_write_us |
|---|---|---|---|---|---|---|---|
| Pre-fix, ECG on, logs on (original) | 50.5 | 10.1 | 9.3 | 21 | 0 | 1 | 737 ms |
| Fix A, ECG off, logs off | 78.4 | 15.7 | 15.7 | 0 | 0 | 2 | 118 ms |
| Fix A, ECG off, logs on | 106.4 | 21.3 | 21.3 | 0 | 0 | 7 | 338 ms |
| Fix A, ECG on, logs on | 70.6 | 14.1 | 13.5 | 0 | 18 | 64 | 1.23 s |

Key observations:

- The original regression was not a single root cause. `LatestImageWriter`'s silent same-dest coalescing, scanner-thread waveform spooling, and Docker stdout log driver backpressure all contributed.
- `scanner_waveforms` rate locks 1:1 with `recon_images` rate — waveform handling on the scanner TCP thread gates the whole pipeline.
- mock-recon discards waveforms (`mock_recon.py:207-210` logs "skipped"). Waveforms in the demo traverse the pipe for zero downstream use.

## Fixes applied

### Fix A — remove same-dest coalescing in `LatestImageWriter`

Pre-fix, `enqueue()` silently replaced the payload of any pending job for the same destination. Under writer stalls (up to 3.14 s observed), 34% of publish attempts were coalesced away — each lost snapshot was one missing mtime bump for viz.

Now the queue holds each publish as an independent job. Overload backstop remains: drop-oldest at 64 pending (logged, counted as `dropped_oldest`). Normal load: queue depth stays at 1–2.

`src/latest_image_writer.cpp::Impl::enqueue` — coalesce loop removed. Tripwire counter `perf_coalesced` stays in the schema; if ever non-zero, coalescing was reintroduced.

Result: `completed == enqueued` across all tested configs. Contract-clean.

### Fix B — remove scanner waveform live-spool call

The call at `src/mrd_tcp_listener.hpp:691` was added in commit `2288d2b` (2026-04-19) as part of the round-7 spool refactor. It archived every incoming ECG waveform into the per-scan live HDF5 file by calling `append_live_waveform` on the scanner TCP thread under `scan_mtx`.

This archival path:

- Contends with recon-image archival (both paths take `scan_mtx`) → scanner TCP throughput falls to recon reply rate (1:1 lock observed).
- Is unreadable mid-scan by any client (HDF5 file locks during writes).
- Has no current in-repo consumer. No documented external consumer.
- Post-scan archive use case is already served by `--dump`.

Fix B deletes the call. Marshal still forwards waveforms to recon via MRD TCP (real recon uses them for gating / motion correction). The forwarding path is untouched.

## Why these two and not the other candidates

Options considered and rejected:

- **Bigger `LatestImageWriter` queue.** Doesn't address the root cause (upstream throughput). Higher memory risk at 1 MB+ images.
- **Move `latest_image.h5` to a separate directory.** Predicted dentry/journal contention turned out not to be the actual bottleneck; `ext4` bind mount + direct rename is fine.
- **Gate live-waveform-spool behind a CLI flag** (`--live-archive-waveforms`). Considered as a hedge, rejected: the feature it preserves (mid-scan archive) is unusable anyway because of HDF5 locking. Flag complexity without a consumer.
- **`std::atomic<std::shared_ptr>` lifetime refactor** (C++20) to take scanner-thread waveform spooling off `scan_mtx`. Real fix but ~60 lines across multiple files. Preserves a feature with no consumer. Not worth the complexity for this project.
- **Split `scan_mtx` per lane.** Moderate refactor (~50 lines) with feature preservation. Same reasoning as above: not worth it for an unused feature.
- **try_lock with drop counter.** Small but introduces explicit drops for no real benefit; same feature-without-consumer problem.

## Mid-scan live query design (generalized)

The deleted live-H5 spool and the current `latest_image.h5` together leave a gap: **no mid-scan interface exists for queries beyond "the single latest image"**. Any future consumer that wants historical or correlated data during an in-progress scan needs something else.

Concrete future consumers and what they need:

- **"Give me the last N images"** (e.g. a time-scrolled viewer, a retrospective filter): needs mid-scan access to several recent images as an atomic set.
- **"Give me the latest image plus the ECG from its acquisition window"** (e.g. cardiac MPC for motion estimation): needs image + time-matched waveform samples, delivered together.

### Why existing paths don't serve these

| Path | Mid-scan readable? | Multi-image? | Waveforms? |
|---|---|---|---|
| Per-scan live H5 (spool + close-convert) | No (finalized at close; HDF5 lock during) | Yes (after close) | No (as of this fix) |
| `latest_image.h5` | Yes (atomic rename) | No (single latest) | No |
| Dump mode H5 | No mid-scan; post-close only | Yes | Yes |
| HDF5 SWMR | Would work technically | Yes | Yes | 

SWMR is explicitly out of scope (user constraint; deployment gotchas around reader cache coherency make it fragile in practice). Per-image H5 files and similar disk-churn alternatives are worse on overlay/bind-mount FS.

### Recommended pattern: bounded in-memory ring + HTTP

```
MarshalState:
  std::mutex recent_mtx;
  std::deque<ImageSnapshot> recon_recent_images;   // bounded to N (e.g. 10)
  // ImageSnapshot = { header, attributes, pixels, acquisition_time,
  //                   optional matched waveform slice }

publish_latest_snapshot():
  under recent_mtx: push_back snapshot; pop_front if > N

marshal_http.hpp:
  GET /image/history?n=K  → snapshot the deque under mutex, return K most recent
                            as a single H5 blob or multipart response
  GET /image/latest        → unchanged (still served from latest_image.h5)
```

Properties:

- **Mid-scan read-safe:** deque is copied under a short-held mutex; response is a point-in-time snapshot. No HDF5 file-lock interaction.
- **Atomic multi-image delivery:** client gets N images that were all present at the time of the request.
- **ECG extension:** when a waveform ring buffer is added (scanner TCP thread push, own mutex), `publish_latest_snapshot` extracts the time-matched slice and stores it in the `ImageSnapshot`. Same endpoint delivers image + ECG together.
- **Bounded memory:** N × image_size (~1–5 MB × 10 ≈ 10–50 MB). Trivial.
- **No scan_mtx churn:** the recent-images deque is its own mutex, not shared with the image-parse path.

### Durability story

The ring is mid-scan only. Long-term persistence still lives on disk:

- **Per-scan live H5** (written by `LiveImageRecorder` via spool + close-convert): finalized at `close_scan`. Complete record of the scan, readable after close. Covers "I want everything from scan X" post-scan.
- **Dump mode H5** (when `--dump` is set): same story, separate archival lane.
- **Ring buffer:** lost on marshal restart. Acceptable for a live interface — if marshal restarts mid-scan, the live view catches up from the next published snapshot; the durable record is the per-scan H5 once the scan closes cleanly.

Ring and disk are complementary:

| Access pattern | Served by |
|---|---|
| Current latest image | `latest_image.h5` |
| Last N images mid-scan | ring buffer (proposed) |
| Image + matched ECG mid-scan | ring buffer (proposed, with waveform slice) |
| All images of scan X, post-close | per-scan live H5 |
| Full session with waveforms, post-close | `--dump` mode H5 |

### Scope

Ring buffer + `/image/history` endpoint: ~60 lines. Add waveform ring + matched-slice extraction: another ~40 lines. Self-contained; no modification to the existing spool, writer, or `latest_image.h5` paths.

When a concrete consumer lands, build the ring. Do not reinstate the scanner-thread waveform spool — wrong access pattern for any real mid-scan use case.

## Verification

- 12/12 ctest pass after each fix.
- `/debug/perf` tripwire: `coalesced` should always be 0 in future runs. Non-zero means Fix A was reverted.
- Counters remain exposed for future regression detection.

## Files touched

- `src/marshal_state.hpp` — perf counters
- `src/latest_image_writer.{hpp,cpp}` — coalesce removed, perf counters, `drain_lag_us` tracking, per-event TIMING log removed
- `src/live_image_store.hpp` — counter bumps in `append_live_image` / `append_live_waveform` / `publish_latest_snapshot`, per-event TIMING log removed
- `src/live_image_recorder.cpp` — spool removal criterion relaxed (see follow-ups)
- `src/mrd_tcp_listener.hpp` — live-waveform-spool call removed
- `src/marshal_http.hpp` — `GET /debug/perf` endpoint
- `docker-compose.demo.yml` — `${MARSHAL_LOG_DRIVER}`, `${RECON_LOG_DRIVER}`, `${KSPACE_ECG}` toggles, `stop_grace_period: 60s` on marshal services

## Follow-ups (same day)

### Spool cleanup criterion

Original `convert_spool_to_hdf5` only removed the spool when `stats.ok()` (no error AND no truncation). On Docker container stop, the userspace 1 MiB buffer often holds a partial trailing record when SIGTERM fires; the converter sets `truncated=true` and the spool was kept indefinitely.

Now the spool is removed when `stats.error.empty()`, regardless of truncation. Tail truncation at shutdown is the normal case — the H5 captured every complete record, the partial trailing record would have been lost either way. Only a true replay error (exception during record N) keeps the spool for forensics.

### Docker stop grace period

Marshal's SIGTERM handler runs `flush_all_live_lanes` synchronously, which drives `convert_spool_to_hdf5` for each lane. For a 100+ MB spool on a bind-mounted ext4 in Docker, conversion takes 10–30 s. Docker's default 10 s grace SIGKILL'd marshal mid-convert, orphaning the spool with no matching `.h5`.

`docker-compose.demo.yml` now sets `stop_grace_period: 60s` on `mri-marshal` and `mri-marshal-dump`.

### Per-event TIMING logs removed

Two `LOG_INFO("TIMING ...")` lines (per `append_live_image` and per `LatestImageWriter` write) were diagnostic, added during this investigation. Same data is exposed by `/debug/perf` as free-running counters. The per-event lines added stdout noise that contributed to Docker log-driver pressure. Removed from `live_image_store.hpp` and `latest_image_writer.cpp`.
