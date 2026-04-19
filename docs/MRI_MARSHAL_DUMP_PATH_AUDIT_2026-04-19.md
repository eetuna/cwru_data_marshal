# Dump-path audit — why dump saturated while live didn't (2026-04-19)

## Original issue

During the 10-min stress run on `fix/marshal-source-2026-04-18` @ `b1519b2`
with `KSPACE_INTERVAL=0.020` (50 Hz scanner) and marshal `--dump` enabled,
`DumpRecorder`'s 256 MB / 4096-job queue filled in **~23 s** and then
dropped for the remaining 9.5 min. `LiveImageRecorder` did not. The user
pushed back: dump should be straightforward (scanner/recon send ISMRMRD
→ write ISMRMRD), so it should NOT be slower than the live path.

Three parallel Explore-agent audits were run to find the real cause.
First two ran; third was interrupted and re-run after the user asked for
thoroughness.

---

## Session 1 — "Dump path deep audit"

Scope: worker threads, queue structure, message volume, MrdSink costs,
backpressure semantics.

### Worker thread and queue structure

- **DumpRecorder**: single `std::thread` + single `std::deque<Job>`
  ([include/dump_recorder.hpp](../.worktrees/mri_data_marshal/include/dump_recorder.hpp),
  [src/dump_recorder.cpp:22](../.worktrees/mri_data_marshal/src/dump_recorder.cpp#L22)).
  All 5 message kinds (scanner_acq, scanner_img, scanner_wf, recon_img,
  recon_wf) and both sinks (`scanner_sink_`, `recon_sink_`) share one
  worker thread and one queue.
- **LiveImageRecorder**: one `std::thread` + one queue **per lane**
  ([src/live_image_recorder.cpp:47-50](../.worktrees/mri_data_marshal/src/live_image_recorder.cpp#L47-L50));
  two instances are constructed upfront in
  [src/marshal_main.cpp:340-343](../.worktrees/mri_data_marshal/src/marshal_main.cpp#L340-L343)
  (one for scanner lane, one for recon lane).

### Message volume at 50 Hz × 5 slices × 128 lines

- Per acquisition ≈ 256 KB (samples only).
- Per 23 s: ~1,150 acquisitions ≈ 290 MB — exceeds the 256 MB queue budget
  alone.

### MrdSink append costs

- Both `append_acquisition` and `append_image`
  ([src/mrd_sink.cpp:99-105, 107-209](../.worktrees/mri_data_marshal/src/mrd_sink.cpp#L99-L105))
  hold a per-sink `std::mutex` and delegate to `ISMRMRD::Dataset::appendX()`.
- `append_image` is more expensive (bounds checks + 9-way data-type
  dispatch) but runs once per image.
- `append_acquisition` is the hot path: 128 per volume × ~50 volumes/s on
  the current test harness.

### Backpressure semantics — the killer difference

- **DumpRecorder on overflow**
  ([src/dump_recorder.cpp:43-52](../.worktrees/mri_data_marshal/src/dump_recorder.cpp#L43-L52)):
  `queue_.clear()` — nukes the entire pending queue (up to 4096 jobs /
  256 MB of work), then drops the current job, logs once.
- **LiveImageRecorder on overflow**
  ([src/live_image_recorder.cpp:72-86](../.worktrees/mri_data_marshal/src/live_image_recorder.cpp#L72-L86)):
  drops only the **oldest droppable** job, admits the new one, logs once.
  Queue stays at ~kMaxQueuedJobs.

### "Warning once, then silence" pattern

`drop_logged_` is reset only in `start_scan()`
([src/dump_recorder.cpp:96](../.worktrees/mri_data_marshal/src/dump_recorder.cpp#L96)).
Once `drop_logged_.exchange(true)` fires, subsequent overflows skip the
warn. Backpressure keeps firing silently.

### Verdict from session 1

Single worker + `queue.clear()` on overflow means once dump is saturated
it oscillates (clear → refill → overflow → clear) without recovery, and
the log goes silent after the first warn.

---

## Session 2 — "Live vs dump path divergence audit"

Scope: callers, routing, instance counts, per-volume counts.

### Caller sites

- **DumpRecorder** called from [src/mrd_tcp_listener.hpp](../.worktrees/mri_data_marshal/src/mrd_tcp_listener.hpp):
  - `append_scanner_text` at line 491 (rare)
  - `append_scanner_acquisition` at line 521 (line-rate)
  - `append_scanner_image` at line 585 (once per volume)
  - `append_scanner_waveform` at line 611 (rare)
- **LiveImageRecorder** called via `append_live_image` at line 592 of the
  same file, and from [src/marshal_http.hpp:87-88](../.worktrees/mri_data_marshal/src/marshal_http.hpp#L87-L88)
  for recon-returned images.

### Per-volume call counts (1 slice × 128 lines)

| Recorder | Calls / volume |
|---|---|
| DumpRecorder | ~129 (128 acqs + 1 image) |
| LiveImageRecorder | 1 (image only) |

**~129× asymmetry.** Raw acquisitions flood DumpRecorder only.

### Message routing

| MRD type | DumpRecorder | LiveImageRecorder |
|---|---|---|
| ACQUISITION (1008) | ✓ | ✗ |
| IMAGE (1022) | ✓ | ✓ |
| WAVEFORM (1026) | ✓ | ✗ |
| TEXT (5) | ✓ | ✗ |

### Multiplicity

- **DumpRecorder**: 1 per process ([src/marshal_main.cpp:336](../.worktrees/mri_data_marshal/src/marshal_main.cpp#L336)).
- **LiveImageRecorder**: 2 per process (one per lane;
  [src/marshal_main.cpp:340-343](../.worktrees/mri_data_marshal/src/marshal_main.cpp#L340-L343)).

### Verdict from session 2

Dump receives ~129× the call volume live does, on a single shared worker.
Live's oldest-drop policy stays in steady state; dump's clear-all policy
never heals.

---

## Session 3 — "HDF5 cost + wire format audit" (re-run after interruption)

Scope: why dump is slower than "just write the bytes" — what work is
DumpRecorder doing that a minimal dump wouldn't?

### Wire format vs HDF5 layout

ISMRMRD on the wire and ISMRMRD in an HDF5 file are the **same format**.
- Wire send in
  [clients/kspace_streamer/kspace_streamer_main.cpp:140-152](../.worktrees/mri_data_marshal/clients/kspace_streamer/kspace_streamer_main.cpp#L140-L152):
  tag (2 B) + `AcquisitionHeader` (340 B) + trajectory bytes + sample bytes,
  memcpy-ed onto the socket.
- Wire recv in
  [src/mrd_tcp_listener.hpp:499-517](../.worktrees/mri_data_marshal/src/mrd_tcp_listener.hpp#L499-L517):
  `read_exact` into `ahdr`, then `traj`, then `samples`, reassembled into a
  `body` vector `[header | traj | samples]`.
- HDF5 compound-type dataset row has the same layout.

### What DumpRecorder actually does per acquisition

[src/dump_recorder.cpp:193-210](../.worktrees/mri_data_marshal/src/dump_recorder.cpp#L193-L210)
— in the worker lambda for every acquisition:

1. Construct a fresh `ISMRMRD::Acquisition` object (allocates trajectory +
   sample buffers).
2. `acq.setHead(hdr)` — copies the 340-byte header into the Acquisition's
   owned buffer.
3. `std::memcpy(acq.getTrajPtr(), traj.data(), traj.size())` — second copy
   of the trajectory.
4. `std::memcpy(acq.getDataPtr(), samples.data(), samples.size())` — second
   copy of the samples.
5. `scanner_sink_->append_acquisition(acq)` →
   `ISMRMRD::Dataset::appendAcquisition(acq)` — the library reads the
   Acquisition's fields back out and hands them to HDF5 field-by-field.

### What the same path does for recon forwarding

[src/mrd_tcp_listener.hpp:511-517](../.worktrees/mri_data_marshal/src/mrd_tcp_listener.hpp#L511-L517)
already builds the assembled `body` buffer (header | traj | samples) in
the correct wire format, and [line 528](../.worktrees/mri_data_marshal/src/mrd_tcp_listener.hpp#L528)
forwards that `body` to recon via `post_frame()` — direct, no reparse.

### The round-trip

```
wire bytes  →  parsed hdr + traj/samples vectors
            →  copied into a fresh ISMRMRD::Acquisition (extra allocation + 2 memcpys)
            →  ISMRMRD::Dataset::appendAcquisition() reads fields back out
            →  HDF5 writes field-by-field
```

vs what it should be:

```
wire bytes  →  HDF5 compound-type row (one direct write)
```

### Why this matters at 50 Hz

Every acquisition: new allocation + 2 extra memcpys + object-field
round-trip through the ISMRMRD API. 128 of these per volume, 50 volumes/s
= ~6,400 round-trips/s going through a single mutex-held HDF5 append call.

### Verdict from session 3

Dump is slow because the current `DumpRecorder::append_scanner_acquisition`
does a **parse → rebuild `ISMRMRD::Acquisition` → re-serialize via HDF5
API** round-trip per message, even though the wire bytes already match
the HDF5 compound layout. The already-assembled `body` buffer is
available in the TCP listener but unused by dump.

---

## Why the third session was re-run

The third agent got interrupted on its first launch (rejected before it
executed). The two that completed (sessions 1 and 2) gave correct but
**structural** answers — thread counts, queue policies, call ratios.
Those explained why dump saturates FASTER than live, but not why dump is
SLOWER per message than a minimal dump. The user flagged that: dump
should be "just write what scanner/recon sent," since ISMRMRD on the wire
is already ISMRMRD HDF5. Session 3 was re-run specifically to answer
that, and it found the round-trip in
[src/dump_recorder.cpp:193-210](../.worktrees/mri_data_marshal/src/dump_recorder.cpp#L193-L210).

---

## Combined root-cause statement

Two orthogonal problems compound:

1. **Per-message cost is too high** (session 3): dump re-parses and
   re-serializes every acquisition through `ISMRMRD::Acquisition` +
   `Dataset::appendAcquisition`, instead of writing the already-assembled
   wire bytes directly to an HDF5 compound-type row. Allocation + 2 extra
   memcpys + object-field round-trip per acq.
2. **Architecture can't absorb the resulting latency** (sessions 1 + 2):
   one worker + one queue for all 5 message kinds means 128 acqs per
   volume serialize head-of-line behind each other, and on overflow the
   queue is nuked wholesale rather than sheddding oldest-first, so dump
   never recovers within a scan.

## Fix direction

- **Primary (session 3):** change `DumpRecorder::append_scanner_acquisition`
  (and the image/waveform siblings) to accept the wire `body` buffer and
  write it to HDF5 as a compound-type row directly. Pass `body` from
  [src/mrd_tcp_listener.hpp:511](../.worktrees/mri_data_marshal/src/mrd_tcp_listener.hpp#L511)
  through unchanged. Expected gain: eliminate 1 alloc + 2 memcpys + the
  ISMRMRD API round-trip per message.
- **Secondary (sessions 1 + 2):** split the dump queue per sink/kind so
  acquisitions don't head-of-line-block images; change overflow from
  `queue.clear()` to oldest-drop to match `LiveImageRecorder`.

---

## Session 4 — "what did the 50 Hz + dump bench actually measure?"

Scope: verify the earlier addendum's "42.33 mean FPS" claim against
what the bench harness and the marshal sinks actually produced.

### Bench harness FPS source

`scripts/bench_fps.sh` parses `[FPS DEBUG] ... FPS: <n>` lines from
`viz_client` stderr
([clients/viz_client/viz_client_main.cpp:437-438](../.worktrees/mri_data_marshal/clients/viz_client/viz_client_main.cpp#L437-L438)).
Those come from viz_client polling `GET /image/latest`
([clients/viz_client/viz_client_main.cpp:327](../.worktrees/mri_data_marshal/clients/viz_client/viz_client_main.cpp#L327))
and counting successful mtime-change detections +H5 reads. That endpoint
resolves via `state.latest_image_path` which is written by
`LatestImageWriter` on the **live** path
([src/live_image_store.hpp:78-105](../.worktrees/mri_data_marshal/src/live_image_store.hpp#L78-L105)).

### Live and dump are parallel, not serial

In [src/marshal_http.hpp:80-89](../.worktrees/mri_data_marshal/src/marshal_http.hpp#L80-L89)
and [src/mrd_tcp_listener.hpp:489-613](../.worktrees/mri_data_marshal/src/mrd_tcp_listener.hpp#L489-L613),
each incoming message is fanned out to dump (if `--dump`) AND to the
live path unconditionally. Dump returning `DumpEnqueueResult::Dropped`
does not block or signal the live path. So viz_client's FPS is
insensitive to dump state.

### Actual dump throughput for the 50 Hz + dump run

From the sink close-log lines at the end of
[marshal.log](../session-data/bench_archive/b1519b2-stress-600s-50hz-dump/marshal.log):

| Sink | Acqs | Imgs | Wfs |
|---|---|---|---|
| `dump/from_scanner` | **3,027,787** | 0 | 23,727 |
| `dump/from_reconstruction` | 0 | 23,742 | 0 |
| `live/from_reconstruction` | 0 | 36,261 | 0 |

From [mock_recon.log](../session-data/bench_archive/b1519b2-stress-600s-50hz-dump/mock_recon.log):
scanner sent **4,641,500** acquisitions total.

- Dump retained **65.2 %** of scanner acqs (3.03 M / 4.64 M).
- Dump dropped **~34.8 %** (~1.61 M acqs) silently after the single
  warn at t+23 s.
- Dump retained **~65.5 %** of recon images (23,742 / 36,261).

### What the addendum got wrong

The earlier addendum's §6 table listed `Mean FPS = 42.33` for
"fix-50 Hz-dump" and compared it to the no-dump 40 Hz run. Those FPS
numbers come from the live path in both cases and say nothing about
dump retention. The claim "throughput is actually higher under 50 Hz +
dump than 40 Hz + no-dump" was true for scanner ingestion but false
for dump persistence — the addendum conflated the two.

### What the run correctly shows

The live path sustains 40–48 FPS while dump is dropping a third of
the scanner stream. That's the HIGH #10 resilience claim: dump drops,
live keeps going. That finding is real. The numeric framing around it
was wrong.

### What a real dump throughput test looks like

Instrument the three `MrdSink` counters (`acq_count_`, `img_count_`,
`wf_count_` in [src/mrd_sink.cpp](../.worktrees/mri_data_marshal/src/mrd_sink.cpp))
to log per-second deltas, or periodically read them via a debug HTTP
endpoint. Compare those rates to kspace_streamer's send rate and
mock_recon's image-return rate to get retention % and drop % over time.
viz_client FPS is not a substitute.

---

---

## Documented contract for `--dump`

From [README.md:11, 72](../.worktrees/mri_data_marshal/README.md):

> When `--dump` is enabled, the marshal **also** mirrors canonical
> ISMRMRD HDF5 history into `dump/from_scanner/` and
> `dump/from_reconstruction/`.
>
> `live/` is **always populated** for active scans, while `dump/` is
> the **optional archival mirror**.

So `--dump` is documented as **additive**, not a mode switch. "Live
always runs" is the current contract. The code in
[src/marshal_http.hpp:80-89](../.worktrees/mri_data_marshal/src/marshal_http.hpp#L80-L89)
and the scanner/recon handlers matches that contract: every message is
fanned out to dump (if `--dump`) AND to `append_live_image()`
unconditionally.

This separates two distinct issues:

### Issue A — performance bug within the current contract

Even as an additive mirror, dump should not be 129× more expensive per
volume than live (session 2) and should not re-parse + re-serialize
every acquisition through the ISMRMRD object API when the wire bytes
already match the HDF5 compound layout (session 3).

Fix direction (contract-preserving):
- Change
  [src/dump_recorder.cpp:193-210](../.worktrees/mri_data_marshal/src/dump_recorder.cpp#L193-L210)
  to accept the assembled wire `body` buffer from
  [src/mrd_tcp_listener.hpp:511-517](../.worktrees/mri_data_marshal/src/mrd_tcp_listener.hpp#L511-L517)
  and write it directly as an HDF5 compound-type row.
- Split the dump queue per sink/kind so scanner_acq head-of-line
  blocking doesn't stall scanner_img, recon_img, etc.
- Change overflow from `queue.clear()` to oldest-drop to match
  `LiveImageRecorder`.

### Issue B — product question: should `--dump` become a mode switch?

If the operator wants a pure archival run and doesn't care about
`GET /image/latest` or live history, running both pipelines in
parallel is wasted work. Making `--dump` exclusive (dump = no live,
no LatestImageWriter, no LiveImageRecorder, `/image/latest` returns
404) would:

- Remove the per-image live HDF5 reconstruction cost.
- Remove the LatestImageWriter queue + snapshot-write overhead.
- Make dump the only consumer of the incoming MRD session thread's
  fan-out, so it can't be stalled by live-path latency.

But this is a contract change — README's "live is always populated"
guarantee would have to be revised, and any client relying on
`/image/latest` being available during a dumping scan would break.
This is a product decision, not a bug.

A middle ground: keep `--dump` additive (current contract), add a
new `--dump-only` or `--archive-mode` flag that is mutually exclusive
with the live path. Leaves the documented contract intact and gives
operators an opt-in fast-archive mode.

---

## Artifacts used in this audit

- [session-data/bench_archive/b1519b2-stress-600s-50hz-dump/](../session-data/bench_archive/b1519b2-stress-600s-50hz-dump/) — marshal/recon/kspace/viz logs from the run that triggered this audit.
- Source files cited inline.

---

## Codex addendum — corrected dump/live interpretation

This addendum does not replace the investigation above. It tightens the
interpretation after re-checking the source and logs.

### Dump purpose clarification

Dump is supposed to write **everything**: scanner config/text/XML,
scanner acquisitions, scanner images, scanner waveforms, recon text,
recon images, and recon waveforms. Therefore, the fact that dump receives
many more messages than live is **not itself a bug**. That is the point of
dump.

The real dump issue is narrower:

> Dump is required to persist the high-rate full stream, but the current
> implementation is too expensive and lossy for that archival workload at
> the tested rate.

### Dump issues that remain valid

1. **Dump could not keep up in the 50 Hz + dump stress run.**

   The artifact shows a real archival failure:

   - `mock_recon.log` reports about **4,641,500** acquisitions received.
   - `marshal.log` closes `dump/from_scanner` with **3,027,787**
     acquisitions.
   - Retention is about **65.2%**; roughly **34.8%** of scanner
     acquisitions were not persisted to dump.

2. **Dump drops records under load.**

   The log shows both the recorder overflow and the enqueue-time
   backpressure signal:

   - `Dump queue full; marking dump incomplete...`
   - `DUMP drop at enqueue time (scanner_acquisition); caller-visible
     backpressure signal`

   `DumpEnqueueResult` makes loss visible to callers, which is an
   improvement, but it does not make dump lossless. For an archival dump
   mode, dropping a third of the stream is still a failure unless the mode
   is explicitly documented as best-effort.

3. **Dump persistence path is too expensive for acquisition-rate traffic.**

   For scanner acquisitions, the listener already assembles:

   ```text
   AcquisitionHeader | trajectory bytes | sample bytes
   ```

   in `src/mrd_tcp_listener.hpp` before forwarding to recon. Dump does not
   persist that assembled body directly. Instead,
   `DumpRecorder::append_scanner_acquisition` constructs a fresh
   `ISMRMRD::Acquisition`, copies trajectory and sample data into it, and
   then `MrdSink::append_acquisition` calls
   `ISMRMRD::Dataset::appendAcquisition`.

   This adds allocation, extra copies, and per-message ISMRMRD/HDF5 object
   API overhead on the hottest path.

4. **Viz FPS is not a dump-health metric.**

   `scripts/bench_fps.sh` measures `viz_client` reads from
   `/image/latest`, which is the live/latest path. The live path can stay
   at 40+ FPS while dump is losing records. Dump health must be measured
   by persisted record counts, dump completeness, drop counters, and sink
   close counts, not by viz FPS.

5. **Current `--dump` is additive.**

   When `--dump` is enabled, the process still constructs
   `LatestImageWriter` and `LiveImageRecorder` instances and still calls
   `append_live_image` for scanner/recon images. If the intended mode is
   archive-only, current `--dump` is doing unnecessary live/latest work
   and distorting dump throughput tests.

   If additive behavior remains useful, split the semantics explicitly:

   - `--dump` or `--dump-live`: dump plus live/latest, current behavior.
   - `--dump-only`: archive-only, no live history, no latest image, no
     `/image/latest` success path.

### Live-history findings

Live history has related design/performance concerns, but it is not the
same failure class as dump.

1. **Live history also rebuilds image objects.**

   `append_live_image` copies the wire image body, `LiveImageRecorder`
   parses it, and `MrdSink::append_image` constructs a typed
   `ISMRMRD::Image<T>`, copies pixels, and calls `Dataset::appendImage`.
   This is the same object-copy pattern as dump images.

2. **Live history can drop old images under backlog.**

   `LiveImageRecorder` uses a bounded queue and drops the oldest
   droppable pending image on overflow. That is acceptable for best-effort
   live history, but it means live history is not a complete archive.

3. **Live history is not a substitute for dump.**

   Live history records images only. It does not preserve raw
   acquisitions, waveforms, text/config messages, or the full scanner/recon
   stream.

4. **No live-history failure was proven in the cited run.**

   The cited stress run showed live/recon history closing with 36,261
   images and the live/latest FPS path staying healthy. The confirmed
   archival failure is in dump, not live history.

### Corrections to numeric framing above

The high-level diagnosis remains useful, but some numbers above need
correction:

- The cited log shows first dump overflow at about **1.36 s** after MRD
  session start, not `~23 s`.
- The `~129 calls / volume` comparison is a one-slice example. The cited
  stress run used 5 slices, and `kspace_streamer.log` reports
  **640 acquisitions per volume**, so actual dump pressure was roughly
  640 acquisition writes plus image/waveform/recon work per volume.
- The proposed "write wire bytes directly as an HDF5 compound row" fix
  direction needs schema/API proof before implementation. The safe
  statement is: reduce or eliminate the per-acquisition object rebuild
  and extra copies where the ISMRMRD HDF5 schema and library API allow;
  otherwise use batching or a lower-overhead append path.

### Practical fix priority

1. Decide semantics: additive `--dump` vs exclusive `--dump-only`.
2. For archive-only mode, disable `LatestImageWriter`,
   `LiveImageRecorder`, and `/image/latest` success responses.
3. Measure dump by retained record counts and drop counters, not viz FPS.
4. Reduce dump acquisition hot-path overhead: direct/batched/lower-copy
   append path if safe.
5. Revisit dump queue policy after hot-path optimization. A dump archive
   should either keep up, apply upstream backpressure, or fail loudly as
   incomplete; silent or sustained loss is not acceptable for archival
   semantics.
