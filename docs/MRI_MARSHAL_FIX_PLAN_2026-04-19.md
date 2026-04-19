# Fix plan — dump path + live history + verification (2026-04-19)

Scope: concrete fixes derived from the audits in
[MRI_MARSHAL_DUMP_PATH_AUDIT_2026-04-19.md](MRI_MARSHAL_DUMP_PATH_AUDIT_2026-04-19.md)
and the corrections in
[MRI_MARSHAL_PERF_STRESS_COMPARE_2026-04-19.md](MRI_MARSHAL_PERF_STRESS_COMPARE_2026-04-19.md)
§6 addendum + codex addendum.

No code written yet. This doc is for review / sign-off.

---

## 1. Dump becomes a mode switch (contract change)

**Intent:** `--dump` means archive-only. Live path is OFF. `/image/latest`
returns 404. No `LatestImageWriter`, no `LiveImageRecorder`, no
`latest_image.h5`.

### Changes

- [src/marshal_main.cpp](../.worktrees/mri_data_marshal/src/marshal_main.cpp):
  gate construction of `LatestImageWriter` and the two
  `LiveImageRecorder` instances (currently at lines ~340-343) behind
  `!state.dump_enabled`.
- [src/marshal_http.hpp:80-89](../.worktrees/mri_data_marshal/src/marshal_http.hpp#L80-L89)
  and the scanner/recon handlers in
  [src/mrd_tcp_listener.hpp:489-613](../.worktrees/mri_data_marshal/src/mrd_tcp_listener.hpp#L489-L613):
  skip `append_live_image()` when `state.dump_enabled`.
- `GET /image/latest` HTTP handler: return 404 with body
  `{"error": "dump mode; no live snapshot"}` when `state.dump_enabled`.
- [README.md:11, 72](../.worktrees/mri_data_marshal/README.md): update
  contract. Current text says "live always runs; dump is an additive
  mirror." New text: "`--dump` is an exclusive archival mode; live path
  is disabled when `--dump` is set."

### Open question (not blocking)

Do we want both behaviors available (keep current additive `--dump`, add
a new `--dump-only`)? The user said "make it a switch," which I'm reading
as: `--dump` itself becomes exclusive. Flag if wrong.

---

## 2. Dump: reduce per-acquisition overhead

**Intent:** remove the allocation + 2 extra memcpys + object-rebuild on
every acquisition. Keep ISMRMRD HDF5 schema (VLEN fields require the
library API).

### Current code

[src/dump_recorder.cpp:193-210](../.worktrees/mri_data_marshal/src/dump_recorder.cpp#L193-L210)
— worker lambda:
1. `ISMRMRD::Acquisition acq(...)` — allocates new traj + sample buffers
2. `acq.setHead(hdr)` — copies 340 B header
3. `memcpy(acq.getTrajPtr(), traj.data(), traj.size())` — 2nd copy of traj
4. `memcpy(acq.getDataPtr(), samples.data(), samples.size())` — 2nd copy of samples
5. `scanner_sink_->append_acquisition(acq)` → ISMRMRD HDF5 write

### Change

Keep the `ISMRMRD::Acquisition` + `appendAcquisition` call (required for
VLEN schema — confirmed by codex audit), but eliminate the captured
`std::vector<uint8_t>` + extra memcpy round-trip:

Option A — lambda captures already-constructed `ISMRMRD::Acquisition`:
- Build the `Acquisition` once in the caller
  ([src/mrd_tcp_listener.hpp:499-523](../.worktrees/mri_data_marshal/src/mrd_tcp_listener.hpp#L499-L523))
  from the bytes already on the stack.
- Move the `Acquisition` into the lambda.
- Worker lambda does just one thing: `scanner_sink_->append_acquisition(acq)`.
- Saves one allocation and two memcpys per acq.

Same treatment for `append_scanner_image`
([src/dump_recorder.cpp:212-224](../.worktrees/mri_data_marshal/src/dump_recorder.cpp#L212-L224))
and `append_scanner_waveform`
([src/dump_recorder.cpp:226-238](../.worktrees/mri_data_marshal/src/dump_recorder.cpp#L226-L238)).

### Not doing

- "Write wire bytes directly as HDF5 compound row." Codex audit showed
  ISMRMRD compound type uses VLEN fields with heap pointers — wire bytes
  are NOT byte-compatible. This fix direction was in session 3 of the
  dump audit; it is retracted.

---

## 3. Dump: drop-oldest on overflow (not clear-all)

**Intent:** match `LiveImageRecorder`'s overflow policy. Currently
dump nukes the whole queue on overflow, destroying up to 4096 pending
writes. Should drop only the single oldest job.

### Changes

[src/dump_recorder.cpp:43-52](../.worktrees/mri_data_marshal/src/dump_recorder.cpp#L43-L52)
— replace:

```cpp
if (queue_.size() >= kMaxQueuedJobs || queued_bytes_ + bytes > kMaxQueuedBytes) {
    dropped_records_.fetch_add(queue_.size() + 1);
    dropped_bytes_.fetch_add(queued_bytes_ + bytes);
    queue_.clear();
    queued_bytes_ = 0;
    ...
    return DumpEnqueueResult::Dropped;
}
```

with a version that drops the oldest single job (pop_front, subtract its
bytes from `queued_bytes_`, increment `dropped_records_` by 1, admit new
job). Mirror
[src/live_image_recorder.cpp:72-86](../.worktrees/mri_data_marshal/src/live_image_recorder.cpp#L72-L86)'s
policy with the added wrinkle of tracking `queued_bytes_`.

### Test coverage

Extend [tests/test_dump_overflow.cpp](../.worktrees/mri_data_marshal/tests/test_dump_overflow.cpp)
to assert that overflow causes exactly one job to be dropped per new
admission, not all 4096.

---

## 4. Dump: split queue per sink

**Intent:** scanner-acq head-of-line blocking should not stall
recon-image writes or vice versa. Currently all 5 kinds share one worker.

### Changes

In [include/dump_recorder.hpp](../.worktrees/mri_data_marshal/include/dump_recorder.hpp)
and [src/dump_recorder.cpp](../.worktrees/mri_data_marshal/src/dump_recorder.cpp):
- Replace single `std::thread worker_` + single `std::deque<Job> queue_`
  with a map keyed by sink (`scanner`, `recon`), each with its own
  thread and queue.
- `append_scanner_*` enqueue onto scanner queue.
- `append_recon_*` enqueue onto recon queue.
- `close_scan` barrier broadcasts to both queues and waits on both.

### Size caps

Keep `kMaxQueuedJobs = 4096` / `kMaxQueuedBytes = 256 MB` per queue, not
shared. So dump memory ceiling doubles to 512 MB under worst case — still
well under marshal's baseline RSS + 256 MB budget on the test host.

---

## 5. Verification: sink counters, not viz_client

**Intent:** viz_client FPS measures `LatestImageWriter` (a third path,
separate from dump and live history). It proves neither dump retention
nor live history retention. Drop it from correctness tests.

### What to measure

Each `MrdSink` already tracks `acq_count_`, `img_count_`, `wf_count_`
([src/mrd_sink.cpp:99-105, 107-209, 211-217](../.worktrees/mri_data_marshal/src/mrd_sink.cpp#L99-L105))
incremented on successful writes only. These are logged at sink close
(marshal.log line format: `Closed HDF5 sink: <path> (acq=N img=M wf=K)`).

### New HTTP endpoint

Add `GET /debug/sinks` returning JSON:

```json
{
  "dump_from_scanner": {"acq": 3027787, "img": 0, "wf": 23727, "dropped_records": 1613713, "dropped_bytes": ...},
  "dump_from_reconstruction": {"acq": 0, "img": 23742, "wf": 0, "dropped_records": ...},
  "live_from_scanner": {"acq": 0, "img": 36261, "wf": 0},
  "live_from_reconstruction": {"acq": 0, "img": 36261, "wf": 0}
}
```

Expose `DumpRecorder::dropped_record_count()` /
`dropped_byte_count()` already in
[include/dump_recorder.hpp:70-76](../.worktrees/mri_data_marshal/include/dump_recorder.hpp#L70-L76)
through the endpoint.

### New bench script

`scripts/bench_dump_retention.sh`:
- Runs kspace_streamer + mock_recon + marshal (with or without `--dump`).
- Polls `/debug/sinks` every second, records JSON.
- At end: compares kspace_streamer "volumes sent × 640" to
  `dump_from_scanner.acq`; compares mock_recon "images sent back" to
  `dump_from_reconstruction.img`.
- Reports retention % per sink. No viz_client involvement.

### Targets

- **Dump mode** (new switch): 100 % retention on both sinks. Any drop is
  a failure.
- **Live history** (no dump, --dump off): 100 % retention on `live/from_*`
  sinks, also measured via the same endpoint.

### Why not viz_client

- `latest_image.h5` is written by `LatestImageWriter`
  ([src/live_image_store.hpp:78-105](../.worktrees/mri_data_marshal/src/live_image_store.hpp#L78-L105)),
  which has its own queue, its own coalescing, its own drop policy
  (MEDIUM #14). It is independent of both dump and live history.
- viz_client FPS measures successful HTTP polls + H5 reads against that
  one file. It says nothing about whether `LiveImageRecorder` wrote any
  image into `scan_*.h5` or whether `DumpRecorder` persisted any acq.
- Keep viz_client only for its own concern: does the HTTP pull path +
  snapshot-swap handoff work? Not for archive correctness.

---

## 6. Live history: same in-place-view change

**Intent:** `LiveImageRecorder` → `MrdSink::append_image` uses the same
object-rebuild pattern as dump
([src/mrd_sink.cpp:107-209](../.worktrees/mri_data_marshal/src/mrd_sink.cpp#L107-L209)).
At image rate (not acq rate) it hasn't failed under load, but the
overhead is still real.

### Change

Same as fix #2 applied to `MrdSink::append_image`. The caller already
has the wire `body` buffer; capture it by move in the lambda and
construct the typed `ISMRMRD::Image<T>` using an in-place view.

Lower priority than #2. Do only if #2 ships cleanly first.

---

## Priority order

1. **Fix #1 (dump = switch)** — contract change, small code change, unblocks proper dump testing.
2. **Fix #5 (verification via sink counters)** — no point fixing dump perf if we keep measuring with viz_client.
3. **Fix #2 (per-acq overhead)** — the main throughput win.
4. **Fix #3 (drop-oldest)** — makes backpressure sane.
5. **Fix #4 (split queue)** — only if #2 + #3 don't close the gap to 100 %.
6. **Fix #6 (live history same treatment)** — cleanup pass, no known failure.

---

## Success criteria

A new 10-min run at `KSPACE_INTERVAL=0.020` (50 Hz) with `--dump`:
- `dump_from_scanner.acq == kspace_streamer volumes × 640`
- `dump_from_reconstruction.img == mock_recon images sent back`
- `DumpRecorder::dropped_record_count() == 0`
- No `[WARN] [dump_recorder] Dump queue full` in marshal.log
- `GET /image/latest` returns 404
- No `LatestImageWriter` or `LiveImageRecorder` instances alive (check
  via the same `/debug/sinks` endpoint, or grep marshal.log for
  `Opened HDF5 sink: .../live/...` — should not appear)

A complementary run without `--dump`:
- `live_from_scanner.img == scanner-sent image count`
- `live_from_reconstruction.img == recon-returned image count`
- viz_client can still read `latest_image.h5` (the one thing viz_client
  is actually testing).

---

## Out of scope

- Merging any branch.
- Performance of the ISMRMRD library itself (chunk size, compression).
- Disk I/O tuning.
- Any changes to `kspace_streamer` or `mock_recon`.
