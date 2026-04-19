# Handoff — MRI marshal dump/live/pushback refactor (2026-04-19)

**For:** the next Claude agent picking up this work.
**State at handoff:** round-9 audit complete (codex), no blocking findings.
**Reading time:** 10 minutes. Do not skip the "how I got it wrong" section.

---

## Where everything lives

**Inner worktree** (code): `/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal`
  - Branch: `fix/marshal-dump-mode-switch-2026-04-19`
  - Tip: `e6c099e` (round-8 send-mutex + fd-lifecycle fix, verified clean by codex round-9)
  - Baseline: `b1519b2` on `fix/marshal-source-2026-04-18` (the pre-work state)

**Umbrella** (docs): `/workspaces/cwru_data_marshal`
  - Branch: `docs/marshal-dump-mode-switch-2026-04-19`
  - Tip: `7c4d701` (contract aligned with the code refactor)
  - Baseline: `fix/marshal-bug-audit-2026-04-18`

```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
git log --oneline b1519b2..e6c099e     # 18 commits of code work
git log --oneline -- src/              # just code

cd /workspaces/cwru_data_marshal
git log --oneline fix/marshal-bug-audit-2026-04-18..docs/marshal-dump-mode-switch-2026-04-19
```

---

## The actual architecture (as of `e6c099e`)

Three pipelines, all coherent:

```
┌───────────────────────────────────────────────────────────────────┐
│ Protocol forwarding (scanner ↔ marshal ↔ recon)                   │
│   Scanner → recon:   forwarder_->post_frame (unchanged)           │
│   Recon  → scanner:  push_message_to_scanner INLINE ::send        │
│                      (no queue; TCP flow control = backpressure)  │
└───────────────────────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────────────────────┐
│ Archival (per-scan history)                                        │
│   Dump mode: raw MRD spool → HDF5 on close                        │
│   Live mode: raw MRD spool → HDF5 on close  (SAME model as dump)  │
│   Both use SpoolWriter + SpoolConverter. Lossless at any rate     │
│   the disk can sustain.                                            │
└───────────────────────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────────────────────┐
│ Mid-scan snapshot (GET /image/latest)                              │
│   LatestImageWriter: atomic-rename-of-closed-snapshots             │
│   live-mode only (404 in dump mode)                                │
│   Untouched by this whole branch; still correct.                   │
└───────────────────────────────────────────────────────────────────┘
```

Key point: **the per-scan `scan_<ts>.h5` files (both `live/` and `dump/`) are archival and only exist AFTER scan close.** Mid-scan readers use `latest_image.h5` via `GET /image/latest`. HDF5's default file locking would prevent mid-scan opens on the per-scan file anyway, so spool-then-convert loses nothing vs the old per-record append design.

---

## Contract ↔ code alignment

Contract doc: `/workspaces/cwru_data_marshal/docs/MRI_MARSHAL_PROTOCOL_CONTRACT_2026-04-19.md` (tip `7c4d701`).

| Contract clause | Code enforces it via |
|---|---|
| `--dump` exclusive mode | `src/marshal_main.cpp:335-343` gates `LatestImageWriter` + `LiveImageRecorder` on `!dump_enabled` |
| `/image/latest` 404 in dump | `src/marshal_http.hpp:125` |
| Scanner-IMAGE carve-out (archived, not forwarded to recon) | `src/mrd_tcp_listener.hpp:588` (else-branch writes to live/dump but no forwarder call) |
| Archival lossless (no drops) | Spool writes are disk-bandwidth bound. `DumpRecorder` / `LiveImageRecorder` never have a drop path except on hard disk failure. |
| Non-blocking archival | Archival runs on worker threads; reader threads enqueue and return. |
| TCP flow control as protocol-layer backpressure | `push_message_to_scanner` is inline `boost::asio::write` (no in-process queue). Blocks when scanner TCP buffer fills, which backpressures recon via its own TCP reader. |
| Byte-exact CONFIG/TEXT/METADATA_XML in spool | Listener passes raw wire body to DumpRecorder; spool stores verbatim. Converter reconstructs at HDF5-write time (with HDF5 VLEN-string truncation caveat documented). |

---

## Commit map (what each one did)

```
b1519b2  BASELINE (pre-refactor, 65% dump retention at 50Hz failure)

   Round 1-3 (c44d256..f0ff1e0): spool for dump + races + byte-exact
   1e824ad  dump=exclusive mode + live captures waveforms + /debug/sinks
   5733d48  CONFIG/TEXT pre-metadata buffer (buggy, superseded)
   7fcd53e  CONFIG/TEXT buffer ordering fix
   c2d1f86  raw-MRD spool for DUMP (lossless dump)
   850c349  codex round-1: 5 spool correctness fixes
   672bdcb  codex round-2: 5 more fixes + destructor race
   81229e1  codex round-3: barrier-atomic close_scan
   83d63a7  gitignore /Testing/
   9f2a0d6  codex round-4: live drop-oldest removed (unbounded queue, transitional)
   2e0bfbe  codex round-5: [live][lossless] deterministic test
   c44d256  codex round-5: dump telemetry race under lane.mtx
   d9d41e2  codex round-5: live + dump spool atomic counters (no sink_ reads from HTTP thread)
   90df0d5  codex round-5: pushback unbounded (transitional)
   c906045  codex round-5: byte-exact dump for TEXT/CONFIG
   f0ff1e0  codex round-6: byte-exact METADATA_XML

   Round 7 (2288d2b, b4a93dd): ARCHITECTURAL UNIFICATION
   2288d2b  LIVE HISTORY → SPOOL (mirror of dump). Kills the unbounded live queue.
   b4a93dd  PUSHBACK REVERT: no writer_thread, inline ::send, TCP does the backpressure

   Round 8 (e6c099e): pushback correctness fixes
   e6c099e  scanner_send_mtx_ + boost::asio::write (no cached fd) + shutdown-aware logging
```

---

## How I got it wrong (read this before touching anything)

I patched symptoms for ~90 turns before codex steered me into the root cause. The pattern to avoid:

1. **"Unbounded queue might OOM" is not a bug to patch at the queue level.** It's a symptom that the queue shouldn't exist. python-ismrmrd-server (the reference MRD implementation) has no in-process queue on the pushback path. TCP flow control is the standard backpressure. Every proxy in the ecosystem (nginx, Envoy, HAProxy, Gadgetron) uses producer backpressure.

2. **"Live mode needs per-record HDF5 for mid-scan reads" was false.** I claimed it 90+ times. HDF5's default file locking prevents mid-scan opens on an open writer. The real mid-scan reader interface is `latest_image.h5` (atomic closed-file snapshots via `LatestImageWriter`). The per-scan live HDF5 was NEVER mid-scan readable; moving it to spool-then-convert lost nothing.

3. **HIGH #4 was the original sin.** Commit on `fix/marshal-source-2026-04-18` that added a writer_thread + queue to `push_message_to_scanner`. The original problem it fixed (shutdown deadlock on stuck scanner) has a simpler fix: close the socket in `stop()`, which makes the blocked `::send` return EPIPE. The queue then forced every downstream decision to pick between drop (contract violation) and unbounded growth (OOM).

**Rule for next session:** when you're patching a symptom, ask whether the structure that produced the symptom is the right structure. If the same pattern keeps producing new symptoms (round-1, round-2, round-3...), the answer is usually no.

---

## Known residual (not blocking)

**Concurrent close() on `tcp::socket` during an in-flight `boost::asio::write`** is formally outside boost::asio's documented thread-safe operations, but kernel behavior is well-defined: concurrent `::close` on a blocked `::send` returns EPIPE/ECONNRESET via `error_code`, the push releases its mutex, done. This is the same guarantee python-ismrmrd-server / Gadgetron / nginx rely on. Documented in `push_message_to_scanner` and `stop()` comments in `src/mrd_tcp_listener.hpp`.

If you ever need to make this formally safe (e.g. a Windows port, or a future boost version changes semantics), the options are:
- **socket.cancel()** from stop() before close. I tried this in round-9 and it deadlocked — the cancel unblocks the current write but the hammer thread re-enters the send path and holds `scanner_send_mtx_` forever. Would need a "don't re-enter when stopping" flag inside `push_message_to_scanner`.
- **Non-blocking sends** so stop() can acquire `scanner_send_mtx_` before close. Defeats the whole TCP-flow-control mechanism.

Neither is worth doing until there's a concrete failure.

---

## Perf & stress test plan (NOT YET RUN at round-9 handoff)

Codex round-9 verified correctness; it did not demand a fresh perf/stress
sweep. The operator wants one before declaring the refactor complete.
Three pipelines, each tested in isolation AT 50 Hz, plus an end-to-end
integration run.

All runs at 50 Hz scanner target: `KSPACE_INTERVAL=0.020`, `SLICES=5`.
10-min stress = `DURATION=600`. Record:
- Sender counts (kspace_streamer volumes_sent × 640, recon images_returned)
- Recorded counts (final HDF5 on disk, post-close)
- Retention % per sink
- Peak RSS (`while pgrep marshal; do ps -o rss=; sleep 30; done`)
- `/debug/sinks` `dropped_records` (must be 0)
- Any `[WARN]` / `[ERROR]` in marshal.log

### Test 1 — DUMP mode @ 50 Hz, 10 min

```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
rm -rf /tmp/bench_retention.* /tmp/bench_retention_logs.*
DURATION=600 KSPACE_INTERVAL=0.020 SLICES=5 \
    ./scripts/bench_dump_retention.sh 2>&1 | tee /tmp/bench_dump_50hz_10min.log
```

**Expected:** 100% retention on `dump/from_scanner.acq` (volumes×640),
`dump/from_scanner.wf` (volumes×5), `dump/from_reconstruction.img`
(recon-returned count). `dropped_records=0`. Disk usage ~9 GB during
run (drops to ~2 GB after HDF5 convert deletes none — spool retained
by default). Report in `docs/MRI_MARSHAL_PERF_2026-04-19_DUMP.md`.

### Test 2 — LIVE mode @ 50 Hz, 10 min

```bash
rm -rf /tmp/bench_retention.* /tmp/bench_retention_logs.*
DUMP_ENABLE=0 DURATION=600 KSPACE_INTERVAL=0.020 SLICES=5 \
    ./scripts/bench_dump_retention.sh 2>&1 | tee /tmp/bench_live_50hz_10min.log
```

**Expected:** 100% retention on `live/from_scanner.wf` (vol×5) and
`live/from_reconstruction.img` (recon-returned). `acq=0` on both
(live mode does not archive raw acqs by design, per contract). Live
rate is much lower than dump (no ACQ records, only IMG+WF); retention
is easy but this confirms the spool path handles both archival
contents correctly. Report in `docs/MRI_MARSHAL_PERF_2026-04-19_LIVE.md`.

### Test 3 — LatestImageWriter mid-scan snapshot @ 50 Hz, 10 min

```bash
# Live mode (LatestImageWriter only exists there).
# viz_client runs as the consumer; bench_fps.sh measures the
# end-to-end snapshot pipeline.
rm -rf /tmp/bench_fps.* /tmp/bench_fps_logs.*
DURATION=600 KSPACE_INTERVAL=0.020 VIZ_INTERVAL=0.020 \
    ./scripts/bench_fps.sh 2>&1 | tee /tmp/bench_fps_50hz_10min.log
```

**Expected:** mean FPS ~40-48 (capped by scanner volume rate).
stdev < 5. Min FPS > 10. Zero `latest_image.h5` read errors in
viz_client.log. No unbounded memory growth over 10 min. Report in
`docs/MRI_MARSHAL_PERF_2026-04-19_LATEST.md`.

**Caveat on bench_fps.sh:** it measures `LatestImageWriter` throughput
via viz_client polling `GET /image/latest`, NOT dump or live-history
retention. Documented in the earlier perf doc. Don't conflate with
tests 1 and 2.

### Test 4 — END-TO-END (all four binaries, 10 min)

All four processes run together, matching a real scan:

```bash
# In one terminal:
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal

# Clean disk
rm -rf /tmp/e2e_test_* && mkdir -p /tmp/e2e_test_data

# Start marshal (live mode)
./build/marshal --http 0.0.0.0:28080 --mrd-port 29100 \
    --dump-dir /tmp/e2e_test_data \
    --recon-host localhost --recon-port 29002 \
    > /tmp/e2e_test_marshal.log 2>&1 &
MARSHAL_PID=$!

# Start mock_recon
python3 docker/mock-recon/mock_recon.py --port 29002 \
    > /tmp/e2e_test_recon.log 2>&1 &
RECON_PID=$!
sleep 2

# Start kspace_streamer (50 Hz, 10 min)
./build/kspace_streamer --host localhost --port 29100 --ecg \
    --interval 0.020 --slices 5 \
    > /tmp/e2e_test_kspace.log 2>&1 &
KSPACE_PID=$!

# Start viz_client
./build/viz_client --http http://localhost:28080 --interval 0.020 \
    > /tmp/e2e_test_viz.log 2>&1 &
VIZ_PID=$!

# Let it run 10 min, then shut down cleanly
sleep 600
kill -TERM $KSPACE_PID $VIZ_PID $RECON_PID
sleep 2
kill -TERM $MARSHAL_PID
wait
```

**Expected:**
- marshal.log: exactly one `MRD session started`, exactly one `MRD session ended`. No `[ERROR]`. No `[WARN]` from the DumpRecorder / LiveImageRecorder drop paths.
- recon.log: mock_recon received close to (volumes × 640) acquisitions.
- kspace_streamer.log: "received N reconstructed image(s) back" where N ≈ volumes.
- viz_client.log: FPS ~40-48 sustained, no H5 read errors.
- Final HDF5: `live/from_scanner/scan_<ts>.h5` has `img=0 wf=<volumes×5>`; `live/from_reconstruction/scan_<ts>.h5` has `img=<recon_images> wf=0`; `latest_image.h5` readable.
- Peak marshal RSS < 500 MB.

Report in `docs/MRI_MARSHAL_PERF_2026-04-19_E2E.md`.

### What to report

One combined doc would also work. Minimum content per test:
- Branch tip SHA
- Host info (`uname -a`, `free -h`, `nproc`)
- Exact command run
- Result table (sent / captured / retention / drops / peak RSS)
- Any anomalies (warns in log, unexpected rates, memory growth slope)
- Verdict: contract satisfied y/n per clause (lossless, non-blocking, mid-scan readable)

If any test fails, file a regression — do NOT patch-around. Round-7's
whole point was to stop symptom-patching.

---

## Docker images

Four images to build (referenced by `docker-compose.demo.yml` in the
umbrella repo):

| Image | Dockerfile | Build context |
|---|---|---|
| `cwru/mri-marshal:latest` | `docker/Dockerfile.mri` | inner worktree |
| `cwru/mock-recon:latest` | `docker/Dockerfile.mock-recon` | inner worktree |
| `cwru/kspace-streamer:latest` | `docker/Dockerfile.kspace-streamer` | inner worktree |
| `cwru/viz-client:latest` | `docker/Dockerfile.viz-client` | inner worktree |

Commands:

```bash
INNER=/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
DOCKER_DIR=/workspaces/cwru_data_marshal/docker

docker build -t cwru/mri-marshal:latest     -f $DOCKER_DIR/Dockerfile.mri             $INNER
docker build -t cwru/mock-recon:latest      -f $DOCKER_DIR/Dockerfile.mock-recon      $INNER
docker build -t cwru/kspace-streamer:latest -f $DOCKER_DIR/Dockerfile.kspace-streamer $INNER
docker build -t cwru/viz-client:latest      -f $DOCKER_DIR/Dockerfile.viz-client      $INNER
```

Each takes ~5-10 min (apt-get + ISMRMRD from source + project build).
~2-5 GB disk per image layer; with 17 GB free at start of round-9
there's comfortable headroom.

Verify:
```bash
docker images | grep cwru
# cwru/mri-marshal       latest    ...   ~2 GB
# cwru/mock-recon        latest    ...   ~1 GB
# cwru/kspace-streamer   latest    ...   ~2 GB
# cwru/viz-client        latest    ...   ~2 GB
```

Smoke test: `docker compose -f /workspaces/cwru_data_marshal/docker-compose.demo.yml up`. See the demo guide for the expected services (mri-marshal + robot-marshal + mock-recon + kspace-streamer + viz-client + helpers).

---

## What's next (if anything)

The round-9 audit said "no blocking findings." The branch is ready for merge review.

**Suggested follow-ups, not blocking:**

1. **Disk-full behavior on spool.** The contract acknowledges hard disk failure (ENOSPC, EIO) as the only acceptable drop path. There's no explicit test simulating disk full. Could add one using a tmpfs mount with a small size.

2. **SWMR mid-scan read.** If a future consumer legitimately needs to read `live/from_*/scan_<ts>.h5` DURING the scan, the contract documents two options: enable HDF5 SWMR mode, or publish periodic closed snapshots. Both are out of scope for round-9 but documented as design paths.

3. **Docker images.** Operator started asking for them (`cwru/mri-marshal`, `cwru/mock-recon`, `cwru/kspace-streamer`, `cwru/viz-client`) before pivoting to this refactor. Dockerfiles are at `/workspaces/cwru_data_marshal/docker/Dockerfile.*`. Build context = inner worktree. Each takes ~5-10 min (apt + ISMRMRD from source + project build). See `docker-compose.demo.yml` for service definitions.

4. **Round-8's "missing test #2" (shutdown-caused failure not logged as error).** Codex asked for explicit log-capture validation. Not implemented; would need a test-only log sink. Current shutdown test proves unblocking but not log-level discipline. Low priority unless someone relies on log-level filtering in production.

5. **http_tracker.py consumer.** Still polls `/image/latest`; breaks cleanly (404 response) in dump mode. If you want it to survive dump mode, either remove it from dump-mode test suites or teach it to handle 404 as "no snapshot this mode."

---

## Commands that work

```bash
# In inner worktree
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal

# Build
cmake --build build

# All tests (12/12 pass, takes ~25s)
ctest --test-dir build --output-on-failure

# Focused tests (fast)
./build/test_dump_overflow              # 13+ test cases covering dump
./build/test_bounded_queues             # live-lossless regression
./build/test_scanner_pushback           # 4 pushback tests inc. [ordering]

# Retention bench (dump mode, 60s @ 50Hz × 5 slices)
DURATION=60 KSPACE_INTERVAL=0.020 SLICES=5 ./scripts/bench_dump_retention.sh

# Retention bench (live mode)
DUMP_ENABLE=0 DURATION=60 KSPACE_INTERVAL=0.025 SLICES=5 ./scripts/bench_dump_retention.sh
```

Both benches should show 100% retention (scanner counts vs final HDF5 counts). Dump is harder (higher rate), live is always easy.

---

## Files you should read before making changes

In order of importance:

1. `docs/MRI_MARSHAL_PROTOCOL_CONTRACT_2026-04-19.md` (umbrella) — authoritative contract
2. `src/mrd_tcp_listener.hpp` — protocol forwarding + inline pushback
3. `src/live_image_recorder.{hpp,cpp}` — live spool worker
4. `include/dump_recorder.hpp` + `src/dump_recorder.cpp` — dump spool worker
5. `include/spool_writer.hpp` + `include/spool_converter.hpp` — shared spool machinery
6. `src/latest_image_writer.{cpp,hpp}` — mid-scan snapshot (untouched, still correct)
7. `docs/MRI_MARSHAL_DUMP_PATH_AUDIT_2026-04-19.md` — root-cause audit from round-7
8. This handoff doc

---

## One-line summary

> Dump, live, and pushback were three separate attempts to solve "lossless + non-blocking + bounded memory." Each had a queue, each had the same failure modes. Round-7 collapsed them into: dump = spool, live = spool (same code), pushback = TCP (no queue). Round-8 fixed the last real pushback bugs (frame-interleave, fd-reuse). Contract updated to explicitly distinguish "archival non-blocking" from "TCP flow control on the protocol path." 12/12 tests pass. No blocking findings from codex round-9.
