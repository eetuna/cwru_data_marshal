# MRI marshal review — findings, decisions, and deferred work (2026-07-02)

Reference record of the full-marshal review (correctness / robustness / performance,
protocol-loyal to python-ismrmrd-server), the decision options presented, what was
chosen, and what was deferred. Source of truth for "why did we pick X" questions later.

- Code reviewed: `.worktrees/mri_data_marshal` at `feat/latest-snapshot-reset` (`5c96ec4`).
- Protocol reference: `third_party/python-ismrmrd-server/{connection.py,constants.py}`.
- Repro probe: `session-data/probe_marshal.py` (strict MRD client; commands at the bottom).
- Fix stack (one branch per concern, based on `feat/latest-snapshot-reset`, **not merged**):
  `feat/close-handshake-guarantee` → `feat/preamble-scan-scope` →
  `feat/recon-connect-timeout` → `feat/scanner-socket-hardening`.

---

## 1. Findings (prioritized)

### (a) Correctness bugs

| # | Finding | Where | Status |
|---|---|---|---|
| A1 | Scanner never receives CLOSE when recon dies hard mid-scan (SIGKILL) or its post-CLOSE flush exceeds the hard-coded 2 s wait. Failure IMAGE (series 9999) arrives, CLOSE never does; a client.py-faithful scanner hangs forever. | `src/mrd_tcp_listener.hpp` CLOSE case (stale `session_active_`), `recon_forwarder.hpp` (`wait_for_close` result ignored; `end_session` suppresses failure callback) | **MEASURED**, fixed on `feat/close-handshake-guarantee` |
| A2 | `recon_preamble` never cleared: scan B on a persistent connection replays scan A's CONFIG/METADATA into recon (recon runs stale config); unbounded growth across image-only scans. | `src/mrd_tcp_listener.hpp` `send_or_buffer_recon` / `ensure_recon_session` | **MEASURED** (recon logs show CONFIG+METADATA twice in scan B), fixed on `feat/preamble-scan-scope` |
| A3 | Shutdown race: `register_session` returning 0 destroys a joinable `std::thread` → `std::terminate`; MRD accept path also calls a moved-from `ts.cancel()`. | `src/session_registry.hpp:44-50`, `src/mrd_tcp_listener.hpp` accept path, `src/marshal_main.cpp` HTTP accept | **DEFERRED** |
| A4 | `remote_endpoint()` can throw in the accept handler on a racing disconnect; escapes `ioc.run()` (no try/catch in `main`) → process death. | `src/mrd_tcp_listener.hpp` accept logging, `src/marshal_main.cpp` | **DEFERRED** |

### (b) Robustness gaps (real scanner/recon)

| # | Finding | Where | Status |
|---|---|---|---|
| B1 | `begin_session()` blocking resolve+connect, no timeout: blackholed recon host holds the scanner session hostage ~130 s (kernel SYN retries). | `src/recon_forwarder.hpp` | Fixed on `feat/recon-connect-timeout` (async resolve+connect, `--recon-connect-timeout-ms`, default 5000) |
| B2 | Wedged (accepting-but-not-reading) recon freezes the scanner mid-scan via blocking send. **MEASURED**: paused recon froze the streamer at frame 103 in ≤2 s for the full pause; on unpause all 1200 frames arrived, zero loss. | `src/recon_forwarder.hpp` `send_message`/`write_exact` | **DECISION: keep** — contract-sanctioned TCP backpressure (see §2.3) |
| B3 | No SO_KEEPALIVE on scanner socket: a vanished scanner (no RST) blocks the session forever; with the single-scanner policy, all future connections rejected until restart. | `src/mrd_tcp_listener.hpp` accept path | Fixed on `feat/scanner-socket-hardening` (keepalive 30/10/3) |
| B4 | No CLOSE toward recon when scanner dies abnormally: recon session leaks until next scan's `begin_session` or shutdown. | `src/mrd_tcp_listener.hpp` `done:` path | **DEFERRED** |
| B5 | Post-EOF finalization blocks new connections: `scanner_socket_.reset()` only after lane flush/convert. The ~35 s lockout after killing long streams; measured ms-scale for short scans, 10–30 s for 100+ MB spools (per fps-regression doc). | `src/mrd_tcp_listener.hpp` `done:` path + `live_image_store.hpp` `flush_live_lane` | **DEFERRED** |
| B6 | WS `ws.accept()` synchronous on the shared io thread: one slow WS client stalls HTTP + MRD accept loops (only with `--ws-port`). | `src/marshal_ws.hpp` | **DEFERRED** |

### (c) Performance

| # | Finding | Status |
|---|---|---|
| C1 | Publish path healthy. **MEASURED**: image mode 30 fps sustained (300/300), k-space 20 fps end-to-end (1200/1200); tripwires clean (`coalesced=0`, `completed==enqueued`, `dropped_oldest=0`, queue depth ≤2); latest-H5 write 2.3–2.7 ms typical, outliers to 931 ms absorbed. | No action needed |
| C2 | Full-volume deep copies under `scan_mtx` in `append_live_image` (`live_image_store.hpp`, partial-flush + complete-publish paths) — could be `std::move`d. | DEFERRED (micro-opt) |
| C3 | No TCP_NODELAY on scanner socket; pushback wrote tag+body as two syscalls. | Fixed on `feat/scanner-socket-hardening` (NODELAY + single gather write) |
| C4 | Scan-close spool→HDF5 conversion runs on the protocol thread under `scan_mtx` (driver of B5's window). | DEFERRED (with B5) |

### (d) Confirmed fine / non-issues

- Wire framing byte-compatible with `constants.py`: tags, header sizes 340/198/40
  (connection.py's "240 bytes" waveform comment is an upstream comment bug), length-prefixed
  bodies preserved verbatim, image body layout matches `read_image`.
- Lossless backpressure: zero frame loss through a 20 s recon stall.
- `wire_guards.hpp` overflow-checked size math on both parse paths; `mrd_sink` re-validates.
- Latest-snapshot per-volume reset (`349caae`) behaves; series-index-constant recons handled.
- `header_received` gate, snapshot generation guard, DumpRecorder stem/barrier protocol,
  SessionRegistry UAF fixes (modulo A3) all check out.

---

## 2. Decisions presented and chosen (2026-07-02, this session)

### 2.1 Scope: which fixes to implement now

| Option | Trade-off |
|---|---|
| All 6 branches (A1, A2, B1, B3+C3, A3+A4, B5) | Everything at once; includes the two riskier/rarer items |
| **Top 4 only — CHOSEN** | A1 close-handshake, A2 preamble scope, B1 recon connect timeout, B3+C3 socket hardening. Defer shutdown races (A3/A4) and non-blocking finalize (B5) to a later pass |
| Just A1 + A2 | Only the two measured correctness bugs |

Deferred and still open: **A3/A4** (shutdown races), **B5/C4** (non-blocking finalize /
socket-release-before-convert — the ~35 s lockout on large spools), **B6** (async WS
handshake), **C2** (move-instead-of-copy under `scan_mtx`), **B4** (CLOSE toward recon
on abnormal scanner EOF).

**Phase 2 (2026-07-03, user-requested follow-up):** A3, A4, B4 and B5 were implemented
on two additional branches stacked on the phase-1 tip:
- `feat/shutdown-hardening` — A3 (`register_session` now cancels+joins on the shutdown
  race instead of dropping a joinable thread; moved-from `ts.cancel()` call sites removed),
  A4 (`peer_string()` uses the `remote_endpoint(ec)` overload; `main()` runs the
  io_context in a resilient catch-and-resume loop), B4 (abnormal scanner EOF now sends
  CLOSE to recon with a 2 s bounded wait, ends the session, finalizes the recon lane —
  **verified live**: recon received CLOSE ~100 ms after `docker rm -f` of the streamer).
- `feat/nonblocking-finalize` — B5, live mode only: the scanner slot is released BEFORE
  lane finalization; overlap with a new session is made safe by `state.scan_epoch`
  (bumped by METADATA_XML under `scan_mtx`) and epoch-guarded finalize helpers
  (`flush_live_lane_at_epoch` / `mark_lane_finalized_after_eof_at_epoch`) that stand down
  when a new scan has taken ownership. Dump mode keeps the old ordering
  (`DumpRecorder::close_lane` has no epoch protection). **Verified live**: after killing
  a 512²-image stream (1111-image spool, 3.6 s conversion), a new connection was
  accepted ~30 ms after the EOF, mid-conversion — previously rejected for the whole
  window. Verification also caught a real overlap bug: the finalizing session's trailing
  `scanner_socket_.reset()` clobbered the NEW session's slot (its CLOSE was silently
  dropped); fixed by compare-and-reset (only reset your own socket).

Still open after phase 2: **B6**, **C2**, and the **B2** watchdog question for Andrew.

### 2.2 Base branch for the fix stack

| Option | Trade-off |
|---|---|
| **Stack on `feat/latest-snapshot-reset` — CHOSEN** | Contains the latest-snapshot fix (`349caae`); sequential stack tests as one Docker build |
| Independent branches from `mri-data-marshal` | Cleaner per-concern review, but no combined integration test without a merge branch; may conflict with unmerged latest-snapshot work |

### 2.3 B2 policy: wedged recon vs scanner stall (mid-scan)

| Option | Trade-off |
|---|---|
| **Keep TCP backpressure as-is — CHOSEN** | Matches the protocol contract and python-ismrmrd-server; zero data loss (measured). Only connect-time gets a timeout (B1). |
| Add send-progress watchdog | If a single blocking send to recon stalls > T, fail the recon session (failure image + CLOSE via A1) and continue archiving. Scanner never stalls, but a slow-recon scan loses its recon output. |

**Open item:** raise the watchdog question with Andrew before the April 2026 experiment —
GRAPPA tail latency on Azure interacts with the CLOSE-flush timeout below.

### 2.4 Recon CLOSE-flush timeout (replaces the hard-coded 2 s wait)

| Option | Trade-off |
|---|---|
| **30 s default + `--recon-close-timeout-ms` — CHOSEN** | Covers GRAPPA/Azure tail latency; on expiry marshal sends its own CLOSE so the scanner never hangs |
| 10 s default + flag | Scanner waits less after recon trouble, but long recon tails get cut and their images lost |
| Keep 2 s, add own-CLOSE | Minimal change; slow-recon tail images still lost |

New CLI flags introduced by the stack: `--recon-close-timeout-ms` (default 30000),
`--recon-connect-timeout-ms` (default 5000).

### 2.5 Amendments discovered during live verification of the stack

- **B1 EAGAIN regression (caught by the k-space regression sweep):** `async_connect`
  leaves the socket in non-blocking mode (synchronous `net::connect` did not); the
  forwarder's raw-fd reader/writer treat EAGAIN as fatal, so the recon reader died
  ~1 ms after every connect. Fixed in the same branch with
  `native_non_blocking(false)` after connect (verified by strace + native harness).
- **B3 keepalive is not enough (caught by the vanish test):** with unACKed pushback
  bytes in flight, Linux suppresses keepalive probes and the ~15 min retransmission
  timer governs — a vanished mid-scan scanner stayed wedged past 9 min on keepalive
  alone. Added `TCP_USER_TIMEOUT=60s`, which bounds both the idle and in-flight
  cases; measured unwind: 67 s from vanish to session end, new connection accepted
  immediately after.

---

## 3. Repro / verification probes

All probes run in the `fire-python` image on `cwru-demo-net`
(`HOSTREPO` = host path of this repo, e.g. `/home/eet12/research/catheter/cwru_data_marshal`):

```bash
# A1 — recon hard-death: expect "RESULT: CLOSE received" post-fix
docker run -d --name probe-close --network cwru-demo-net \
  -v "$HOSTREPO/scripts:/scripts" -v "$HOSTREPO/session-data:/probe" fire-python:latest \
  python3 /probe/probe_marshal.py --test close_on_recon_death --pause 15
# ...during the PAUSING window:
docker kill cwru-recon        # SIGKILL: no graceful CLOSE (docker stop sends one!)
docker logs -f probe-close
docker start cwru-recon       # restore

# A2 — stale preamble: expect exactly ONE CONFIG/METADATA in scan B's recon session
docker run --rm --network cwru-demo-net \
  -v "$HOSTREPO/scripts:/scripts" -v "$HOSTREPO/session-data:/probe" fire-python:latest \
  python3 /probe/probe_marshal.py --test preamble
docker logs cwru-recon --since 3m | grep -E "CONFIG_FILE|METADATA|Accepting"

# B2 wedge demo (behavioral, by design): docker pause cwru-recon mid k-space stream
# freezes the streamer within ~2 s; docker unpause -> full recovery, zero loss.
```

Measurement baseline (pre-fix, 2026-07-02): image mode 30 fps, k-space 20 fps
(128×128, 8 coils, invertcontrast), tripwires clean throughout.
