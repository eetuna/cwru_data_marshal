# Marshal purpose-driven audit — 2026-07-03

Scope: what the marshal is *for*, how every client interacts with it in every
mode, and where it falls short of that purpose. Suggestions are deliberately
conventional — standard patterns only, no niche tricks. Grounded in
ARCHITECTURE.md, EXTERNAL_CLIENT_GUIDE.md, API_REFERENCE.md,
SLICE_REPOSITION_AND_POSITION_PLAN.md, the code, and this week's measurements.

---

## 1. What the marshal is trying to achieve

For the April 2026 MRI-guided catheter experiment, the marshal is the hub that:

1. **Proxies the scan** — real scanner ↔ recon over MRD TCP, transparently
   (recon images also return to the scanner console).
2. **Feeds the live view** — WebGL UI (and any external client) sees the newest
   reconstructed image mid-scan.
3. **Records the session** — full k-space archive (dump mode) or images+ECG
   (live mode) for post-hoc analysis.
4. **Carries control/telemetry side-channels** — slice-translation commands
   (UI → scanner sequence), pose, slice-transform delta, and (planned) slice
   geometry extracted from image headers.

The robot side (robot-marshal) is a separate service; the UI merges both.

## 2. Client interaction map

| Client | Channel | Live mode | Dump mode |
|---|---|---|---|
| Scanner | MRD TCP :9100 | full protocol; images+CLOSE pushed back | same |
| Recon | MRD TCP out | per-scan session, k-space in / images out | never contacted (archive only) |
| WebGL viewer | HTTP + **shared volume** | poll `/image/latest` → open `latest_image.h5` (warm h5py worker) | `/image/latest` = 404, **UI blank** |
| External viewers (LAN or container) | same pattern | must mount the same volume at the same path | blank |
| UI slice nudge | `POST /write/file_slice_translation` | cached in memory; **nothing consumes it** (loop open — see plan doc) | same |
| Control clients | `GET/PUT /transform` (consume-on-read), `POST/GET /pose` | in-memory caches | same |
| Analysts | `scan_<ts>.h5` archives | images+waveforms | full stream incl. k-space |
| Operators | `/health`, `/debug/perf`, `/debug/sinks` | counters | counters |

## 3. Where it falls short of the purpose (ranked), with conventional fixes

### P1 — The slice-translation control loop is open (mission-critical)
The UI can command ±1 slice, the marshal caches it, and **nothing delivers it
to the scanner**. SLICE_REPOSITION_AND_POSITION_PLAN.md documents this and
already contains the right, conventional design: (a) extract
`position/read_dir/phase_dir/slice_dir` from image headers into a
slice-geometry cache + GET endpoint; (b) deliver the command to the scanner as
a standard MRD `TEXT(5)` message on the **existing** return socket (the scanner
side speaks MRD, not HTTP). Nothing exotic — TEXT is a first-class protocol
message. Without this, the April slice-reposition feature does not exist
end-to-end. **Effort: M (the plan doc scopes it).**

### P2 — Dump mode blinds the operator (either/or is the wrong shape)
The experiment needs **recording AND live view at the same time**; today
`--dump` gives a complete archive but a blank UI. The snapshot publisher is
measured cheap (~2.5 ms/write; ~430/s ceiling vs ≤40/s realistic demand), so
the mutual exclusion is not a resource necessity. **Fix: allow the live
snapshot pipeline in dump mode** (`--dump` keeps full archival; snapshot +
`/image/latest` stay on). One flag / mode-matrix change, big operational win.
**Effort: S–M.**

### P3 — Image delivery couples every client to a shared filesystem
`GET /image/latest` returns a *path*; a reader must mount the same volume at
the same mount point, link HDF5, and disable file locking. Consequences:
- LAN clients (which the client guide explicitly supports for HTTP) **cannot
  read images at all** — they have no volume.
- Same-host misconfiguration silently reads stale files (exactly the
  devcontainer/host `session-data` split that bit us this week).
**Fix: serve the bytes over HTTP too** — `GET /image/latest.h5` streams the
snapshot file; optionally `GET /image/latest/raw` returns JSON header + binary
pixels so simple clients need no HDF5 at all. Standard REST file serving; keep
the existing pointer endpoint for back-compat. Kills an entire failure class
and makes the client guide's LAN story true. **Effort: S.**

### P4 — Blind 20 Hz polling; no change signal
Every client re-opens and re-parses the snapshot even when nothing changed
(10–60 ms per read on the viewer path). The marshal already keeps an atomic
`latest_image_generation`. **Fix (standard HTTP):** include `generation` in
the `/image/latest` JSON and honor `ETag`/`If-None-Match` (304 when
unchanged). Optionally broadcast "image updated" on the already-existing WS
port so clients fetch exactly once per volume. **Effort: S.**

### P5 — Inconsistent command-channel semantics
`/transform` is consume-on-read (second reader sees zeros; two readers race);
`file_slice_translation` is read-without-clear (re-apply risk; dedupe left to
each consumer). Two similar channels, two different contracts. **Fix: one
convention — every command/latest-value endpoint carries a monotonic `seq` (and
server timestamp); consumers apply only `seq > last_seen`. GETs become
idempotent and multi-reader-safe.** Apply to `/pose` responses too (staleness
visibility for robot-MRI registration). **Effort: S.**

### P6 — No single operational status view
`/health` says "ok"; the real questions during an experiment are: which mode,
scanner connected?, recon session state, current scan id, images published,
seconds since last publish, disk free. All already known in-process. **Fix: a
read-only `GET /status` aggregating them.** **Effort: S.**

## 4. Confirmed fit-for-purpose (no action)
- Protocol proxying: correct and robust after the 8-commit fix stack (CLOSE
  guarantees, bounded connect, socket hardening, non-blocking finalize) — all
  verified live.
- Performance: marshal adds ~32 ms/frame and has ~60 % headroom over April
  demand; publish path and viewer reads are not limiters (2026-07-03 perf
  audit). Multislice slowness is the bundled test recon, which the real recon
  replaces.
- Archival model (spool → convert-on-close, atomic-rename snapshot): sound,
  standard patterns.
- Security posture: documented as isolated-network; appropriate for the lab.

## 5. Suggested order
1. **P1** slice-translation delivery + slice geometry (April feature, already planned)
2. **P2** live snapshot in dump mode (operator visibility while recording)
3. **P3** HTTP image bytes (decouple from shared volume)
4. **P4** generation/ETag (cheap, cleaner clients)
5. **P5** seq/timestamps on command channels
6. **P6** `/status`

P3+P4 together also simplify Ridaa's viewer (fetch-on-change over HTTP, no
h5py worker, no volume mount) — coordinate before changing anything it reads.
