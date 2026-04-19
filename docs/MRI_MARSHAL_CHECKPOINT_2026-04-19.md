# Checkpoint — MRI marshal dump/live rework (2026-04-19)

Snapshot of working-tree state after this session. Nothing committed; this
doc is the paper trail.

## Branches

- Inner worktree: `/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal`
  - On `fix/marshal-source-2026-04-18` @ `b1519b2`, **10 files modified, 1 new file**, uncommitted.
- Umbrella: `/workspaces/cwru_data_marshal`
  - On `fix/marshal-bug-audit-2026-04-18` @ `5dbdeae`, 4 new doc files untracked.

## What changed, in one line each

1. **`--dump` is now an exclusive mode switch.** Live path (`LiveImageRecorder` + `LatestImageWriter` + `/image/latest`) is OFF in dump mode. Previously additive.
2. **Live mode now archives waveforms (ECG), not just images.** Scanner and recon WAVEFORM messages route to `LiveImageRecorder` when not in dump mode.
3. **Dump retention is measurable.** New `GET /debug/sinks` endpoint reports per-sink counters, last-closed counters, and dump drop counters. New `scripts/bench_dump_retention.sh` uses it (no viz_client).
4. **Dump hot path is faster.** `ISMRMRD::Acquisition`/`Image`/`Waveform` built at enqueue time and captured via `shared_ptr` so `std::function` doesn't re-copy vector buffers.
5. **Dump overflow drops oldest, not all.** Was `queue.clear()` wiping up to 4096 pending writes; now pops one oldest droppable job per admission. Barriers (`close_scan`) marked non-droppable.
6. **Dump queue is split per lane.** Scanner and recon each have their own thread + queue + sink; slow recon writes no longer starve scanner and vice versa.
7. **Filesystem leaks closed (codex round 1).** `reset_live_outputs_for_new_scan` and `on_failure`'s PNG write gated on `!dump_enabled` so dump mode doesn't create any `live/` dirs or files.
8. **Capacity + counter fixes (codex round 2).** `enqueue_on` now rejects oversized droppable jobs up front and re-checks cap after drop loop. Last-closed sink counters retained so `/debug/sinks` works post-close.
9. **New test.** `tests/test_http_handlers.cpp` `[dump][mode]` section: 404 from `/image/latest`, no live recorder construction, no `latest_image.h5` written.

## Retention result (60 s, 50 Hz, 5 slices)

| Sink | Retention |
|---|---|
| `dump/from_reconstruction.img` | 100 % |
| `dump/from_scanner.acq` | 76.5 % |
| `dump/from_scanner.wf` | 64.5 % |

Scanner-side gap is the single HDF5 worker ceiling. Requires batched or
non-HDF5 archival to close — out of scope for this session.

## Files touched

- **Modified (10):**
  - `README.md`
  - `include/dump_recorder.hpp`
  - `src/dump_recorder.cpp`
  - `src/live_image_recorder.cpp`
  - `src/live_image_recorder.hpp`
  - `src/live_image_store.hpp`
  - `src/marshal_http.hpp`
  - `src/marshal_main.cpp`
  - `src/mrd_tcp_listener.hpp`
  - `tests/test_http_handlers.cpp`
- **New (1):**
  - `scripts/bench_dump_retention.sh`
- **Diff:** +606 / -200

## Docs produced this session

All under `/workspaces/cwru_data_marshal/docs/`:

- `MRI_MARSHAL_PROTOCOL_CONTRACT_2026-04-19.md` — new contract (supersedes the original for the fix branch).
- `MRI_MARSHAL_FIX_PLAN_2026-04-19.md` — pre-implementation plan.
- `MRI_MARSHAL_DUMP_PATH_AUDIT_2026-04-19.md` — audit of why dump saturated.
- `MRI_MARSHAL_PERF_STRESS_COMPARE_2026-04-19.md` — bench numbers + correction about what viz_client actually measures.
- `MRI_MARSHAL_CHECKPOINT_2026-04-19.md` — this file.

## Tests

- `ctest --test-dir build`: **12/12 passed** after every P and after every codex round fix.

## Known gaps against the new contract

The new contract in `MRI_MARSHAL_PROTOCOL_CONTRACT_2026-04-19.md` says
"marshal must never drop records." Current code still drops oldest on
overflow when the worker can't keep up. The contract is stricter than
the implementation. Closing that gap requires either larger queues, a
batched writer, or a lower-overhead archival format — noted as next
session's work.

## Reproduce

```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
cmake --build build
ctest --test-dir build --output-on-failure
# retention bench in dump mode
DURATION=60 KSPACE_INTERVAL=0.020 SLICES=5 ./scripts/bench_dump_retention.sh
# retention bench in live mode
DUMP_ENABLE=0 DURATION=60 KSPACE_INTERVAL=0.025 SLICES=5 ./scripts/bench_dump_retention.sh
```
