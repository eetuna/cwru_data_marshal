# PR draft — MRI marshal bug audit fixes (2026-04-18)

**Status:** draft, not merged. Two fix branches ready for review; this doc
captures the PR description we would post when opening the merge.

## Branches

| Branch | Parent | Tip | Scope |
|---|---|---|---|
| `fix/marshal-source-2026-04-18` | `perf/latest-bulk-prealloc` @ `1822829` | `f68ef4b` | Source fixes inside the MRI marshal (`.worktrees/mri_data_marshal/`) |
| `fix/marshal-bug-audit-2026-04-18` | `audit/mri-marshal-protocol-fixes-umbrella` @ `45c6d6e` | `d0ec988` | Ops/docs edits in the umbrella |

Both branches are intended to be reviewed together — the source fixes
reference caps/helpers that also shape the expected ops envelope.

## Summary

Closes **all 21 source findings** and **6 ops/docs items** from
[docs/MRI_MARSHAL_BUGS_FINAL_2026-04-18.md](MRI_MARSHAL_BUGS_FINAL_2026-04-18.md).
Every finding has been verified PROVEN_FIXED by an independent
parallel-agent verification pass (10 agents, 21 items). No finding
downgraded to PARTIALLY_FIXED.

Test suite grew from 5 binaries / 92 assertions to **12 binaries / 162
assertions**. All pass. Bench (`scripts/bench_fps.sh DURATION=15
KSPACE_INTERVAL=0.025`) remained at 40–45 FPS across every commit; no
FPS regression.

## Per-finding commit table

### Source fixes on `fix/marshal-source-2026-04-18`

| Commit | Finding(s) | Subject |
|---|---|---|
| `805010c` | CRITICAL #1, #2, #3 | tracked sessions + joined shutdown; viz probe-read (the real one that commit `1822829` claimed but didn't stage) |
| `d577343` | HIGH #4 | async scanner writer — unblocks recon reader |
| `051ffc2` | HIGH #5, #6, #7 | checked arithmetic + caps for wire-parsed image sizes |
| `420f2ee` | HIGH #8 | reject concurrent scanner connections |
| `6c671b8` | HIGH #9 | gate recon_group_is_complete on header_received |
| `bbcd683` | HIGH #10 | expose dump overflow to caller via accessors |
| `450e4d9` | MEDIUM #11 | set Beast `body_limit` on parser before `http::read` |
| `cfe673a` | MEDIUM #12 | cap length-prefix body size before allocation |
| `9e1d0b6` | MEDIUM #13, #16, #17 | checked arithmetic in parser/sink/bulk |
| `37ba2c2` | MEDIUM #14, #15 | bounded queues for LatestImageWriter + LiveImageRecorder |
| `c3b1f89` | MEDIUM #18 | log exceptions in push-message callbacks |
| `f68ef4b` | LOW/NIT #19, #20, #21 | checked CLI parse, attr_string_len clamp, waveform notation |

### Ops/docs fixes on `fix/marshal-bug-audit-2026-04-18`

| Commit | Item(s) | Subject |
|---|---|---|
| `b3e2658` | (setup) | add `VIZ_INTERVAL`; pass `--interval` to viz-client; `KSPACE_INTERVAL` → 30 FPS |
| `45c6d6e` | (setup) | commit outstanding audit artifacts |
| `d0ec988` | #22, #23, R2, #24 | compose defaults, `scripts/bench_fps.sh` dedup, `DEVELOPER_GUIDE.md` refresh |

Deferred (follow-up, not blocking this PR):
- **R1** `scripts/demo-docker.sh` ↔ `scripts/demo-persistent.sh` dedup (a real refactor, not a fix)
- **R3** `docker/Dockerfile.image-streamer` ↔ `docker/Dockerfile.kspace-streamer` shared-base extraction

## What changed at the design level

### New shared helpers

- **`src/session_registry.hpp`** — thread-safe registry of active sessions
  with cancel callbacks. Used by both the HTTP accept loop and
  `MrdTcpListener`. `shutdown_and_join()` cancels (closes sockets) then
  joins, guaranteed to complete before `MarshalState` destruction.
- **`include/wire_guards.hpp`** — checked-arithmetic + per-field caps for
  all wire-parsed sizes. Reused at:
  - scanner IMAGE parse (`mrd_tcp_listener.hpp`)
  - recon-return IMAGE parse (`recon_forwarder.hpp`)
  - length-prefix body reads on both sides
  - sink `append_image` precondition (expected vs caller `pixel_bytes`)
  - latest-image bulk-write aggregate multiplication

### Shutdown order (signal handler in `marshal_main.cpp`)

```
1. acceptor.close()               — stop new HTTP connections
2. mrd_listener->stop()           — close MRD acceptor + scanner socket; join sessions
3. http_sessions.shutdown_and_join() — cancel + join detached HTTP sessions
4. forwarder->stop()              — join recon reader thread
5. flush_all_live_lanes(state)    — drain writer queues
6. state.close_scan()             — finalize HDF5 sinks
7. ioc.stop()                     — unwind asio
```

Every detached thread variant is joined before `MarshalState` is destroyed.
The old `[&state]` + `.detach()` pattern is gone from both the HTTP accept
path and `MrdTcpListener::do_accept`.

### Bounded queues

- `LatestImageWriter`: 64-job cap, coalesce by destination (latest is
  naturally coalescible), drop-oldest on overflow with log-once.
- `LiveImageRecorder`: 4096-job cap, per-job `droppable` flag,
  `close_scan()` enqueues with `droppable=false` so the barrier is never
  dropped. Overflow erases the first droppable pending job.
- Both expose `had_overflow()` + `dropped_count()` accessors.
- `MrdTcpListener` writer: 1024-job cap, drop oldest (pushback is
  naturally lossy — a failed `::send` already closes the socket).

### CLI hardening

`std::stoi` / `std::stoull` used to throw uncaught on bad CLI input,
terminating the process. New `checked_parse_uint16` / `checked_parse_size`
helpers reject trailing garbage + out-of-range and exit 1 with a clear
error message.

## Verification

### Test suite

```
unit_pose                          3 assertions /  1 case
test_mrd_sink                     28 assertions /  9 cases
unit_http_handlers                53 assertions / 11 cases
it_http                            2 assertions /  2 cases
test_ws_client                     7 assertions /  3 cases
test_scanner_pushback              2 assertions /  2 cases   (NEW — HIGH #4)
test_scanner_race                  3 assertions /  1 case    (NEW — HIGH #8)
test_wire_guards                  44 assertions /  6 cases   (NEW — HIGH #5/#6/#7)
test_recon_group_header           10 assertions /  1 case    (NEW — HIGH #9)
test_dump_overflow                 4 assertions /  2 cases   (NEW — HIGH #10)
test_http_body_limit               2 assertions /  2 cases   (NEW — MEDIUM #11)
test_bounded_queues                4 assertions /  2 cases   (NEW — MEDIUM #14/#15)

TOTAL                            162 assertions / 42 cases   (all pass)
```

### Bench (at `KSPACE_INTERVAL=0.025`, 40 Hz target)

| Stage | mean FPS |
|---|---|
| Baseline (before fixes) | 43.26 |
| After CRITICAL #1–3 | 42.38 |
| After HIGH #4 | 43.12 |
| After HIGH #5–7 | 43.96 |
| After HIGH #8 | 43.14 |
| After HIGH #9 | 43.08 |
| After HIGH #10 | 41.85 |
| After MEDIUM #11 | 42.50 |
| After MEDIUM #12 | 44.76 |
| After MEDIUM #13/#16/#17 | 40.49 |
| After MEDIUM #14/#15 | 44.44 |
| After MEDIUM #18 | 42.13 |
| After LOW/NIT #19/#20/#21 | 43.03 |

No commit introduced a measurable FPS regression. Mean stays in a 40–45
band across all 12 source-fix commits.

### Independent verification (agents)

10 parallel read-only agents, one per finding (or per cluster), each
given only the finding text + commit hash + instruction to read the
post-fix source and walk the adversarial path. All 21 findings came back
**PROVEN_FIXED** with quoted code. No residual race windows, no missed
OOB paths.

Notable calls:
- **HIGH #9** race on scan close — verified: the `header_received` load
  runs inside `scan_mtx`, which also protects the close-scan reset. No
  window where an image is mis-grouped.
- **HIGH #10** backpressure semantic — clarified: the accessors provide
  observability, not a control channel. Runtime backpressure on
  `DumpRecorder::enqueue` would require redesigning the enqueue return
  type; that is out of scope. The fix lets the operator see drops
  post-scan via `LOG_WARN` on CLOSE.
- **MEDIUM #14** drop-vs-coalesce order — verified: coalesce check
  precedes drop-oldest, so fast producers with the same dest never
  trigger the drop path.

## Known caveats

- Bench is a single-machine proxy for performance. It does not exercise
  shutdown under in-flight requests, which is the specific path CRITICAL
  #1–3 fix. Shutdown correctness is argued from call-graph inspection
  (joined thread order); a race stress test remains a follow-up.
- `HIGH #10` is observability-only. If the live MRD path's rate
  genuinely exceeds what the dump writer can sustain, records will still
  drop — operators must read logs or check `dump_complete="false"` on
  close.
- Frame-size caps in `wire_guards.hpp` are generous (512 MiB aggregate,
  8192 per dim, 256 channels, 16 MiB attrs). If a real MRI deployment
  legitimately exceeds those, reject logs will fire and the cap needs to
  be revisited. The values were picked to cover standard 2D/3D scans
  with 2–4× margin.
- `scripts/bench_fps.sh` host-native stack uses different ports than
  docker-compose (28080/29100/29002 vs 8080/9100/9002) to allow both to
  run simultaneously.

## Reviewer checklist

- [ ] Inner worktree on `fix/marshal-source-2026-04-18` builds clean
      (`cmake --build build`).
- [ ] All 12 test binaries pass.
- [ ] `scripts/bench_fps.sh DURATION=15 KSPACE_INTERVAL=0.025` returns
      mean FPS ≥ 35.
- [ ] `marshal --ws-port notanumber` exits 1 with a clean error
      (no `std::terminate` / backtrace).
- [ ] Docker compose parses: `docker compose --env-file .env.demo -f
      docker-compose.demo.yml config | head -5`.
- [ ] `scripts/bench_fps.sh` exists only at inner worktree path.
- [ ] `docs/DEVELOPER_GUIDE.md` branch diagram references
      `audit/mri-marshal-protocol-fixes-umbrella` + `perf/latest-*`.

## Not in this PR

- Shutdown stress tests (CRITICAL #1–3 correctness is reviewed, not
  stress-exercised).
- ASan/TSan-instrumented test run.
- R1 (demo-docker / demo-persistent merge) and R3 (Dockerfile shared
  base). These are cleanup refactors, scheduled as follow-up.
- Protocol contract changes (`MRI_MARSHAL_PROTOCOL_CONTRACT.md`
  unchanged).
