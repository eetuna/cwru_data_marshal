# MRI marshal performance audit — 2026-07-03

Scope per user direction: **marshal only**. The fire streamer and the bundled
python-ismrmrd-server recon are stand-ins that will be replaced by the real
scanner and the real (GRAPPA) recon, so their ceilings are excluded — the
question is what the marshal itself adds. All numbers measured on the running
Docker stack (WSL2), marshal built from `mri-data-marshal` @ `c08cb24`.

Method: byte-pump endpoints (`session-data/mrd_bench.py`) that speak minimal
MRD framing with `MSG_WAITALL`/`sendall` on pre-serialized frames, so endpoint
python cost is negligible; direct endpoint↔endpoint baselines isolate the
marshal's contribution. Test frame = the user's config: 96×96, 8 coils,
5 slices → 480 acquisitions × 6,486 B = 3.11 MB per frame; return image =
5-slice float volume, 184 KB.

## Results

| # | Path | Result |
|---|------|--------|
| R1 | sender → sink, **direct** (no marshal) | **310 MB/s, 47,700 acq/s** (~100 fps of this frame) |
| R2 | sender → **marshal** → sink (forward path, unpaced) | **75 MB/s, 11,485 acq/s = 24 fps**, marshal at 107 % CPU (one core saturated) |
| R3a | echo round-trip **direct**, paced 15 fps | RTT p50 **6.2 ms** (p99 10.8) |
| R3b | echo round-trip **via marshal**, paced 15 fps | RTT p50 **38.6 ms** (p99 48.8) → **marshal adds ≈32 ms per 5-slice frame** |
| R4 | recon-side burst: 2,000 × 184 KB images → marshal → scanner | **all 2,000 relayed** (sender offered 1,656 img/s); publish worker completed ~430 snapshot writes, drop-oldest backstop shed the rest — protocol relay unaffected |
| Lag | 15 fps k-space stream, sent-frame vs frame inside `latest_image.h5` | published repetition **tracks the sent frame within ~1 s of sampling noise** — the viewer always gets the *newest* volume; there is no stale-frame backlog |
| Viewer | webgl `/api/read` (2D and 3D) during live stream | 16–94 ms per read (warm h5py worker); frontend fetches back-to-back in its render loop |

Tripwires stayed clean throughout the protocol-path tests (`coalesced=0`,
`completed==enqueued` except the deliberate R4 overload where `dropped_oldest`
is the designed latest-only backstop).

## Interpretation

1. **The marshal is not the cause of the lag seen at 15 fps × 5 slices.**
   Its forward ceiling (11,485 acq/s) is ~60 % above that demand (7,200 acq/s),
   it delivers the newest frame end-to-end (no stale backlog), and its publish
   worker keeps up at every realistic rate. The perceived lag is a *frame-rate*
   drop imposed by the test recon (~25–38 slice-recons/s ceiling, measured in
   the prior session), which TCP backpressure correctly propagates to the
   sender.

2. **What the marshal does add:** ~65–90 µs of one core per acquisition
   (read + reassemble + copy + forward), which shows up as:
   - ≈32 ms added pipeline latency per 480-acquisition frame (R3),
   - a single-thread forward ceiling of ~24 fps for this frame shape / 75 MB/s
     (R2) — comfortable for the April configuration, but the headroom is 1.6×,
     not 10×. Bigger matrices/more coils/more slices eat it linearly.

3. **Return path and snapshot publishing have large margins**: 1,600+ img/s
   relay, ~430 snapshot-rewrites/s, viewer reads in tens of ms.

## Recommendations (standard fixes only, in order)

1. **Nothing marshal-side is required for the April config.** 15 fps × 5
   slices × 96² × 8 coils runs within measured headroom once the real recon
   replaces the python test server. Re-run `mrd_bench.py` (R2/R3) against the
   real recon's frame shape as acceptance.
2. **For demos with the bundled recon, size the demand to it**: ≤5 volumes/s
   at 5 slices (or 1 slice at 15–20 fps). That is a test-tool limit, not a
   pipeline defect.
3. **If future configs exceed ~10k acq/s** (larger matrix, more coils), the
   conventional next step is reducing per-acquisition work in
   `mrd_tcp_listener.hpp` (single body buffer instead of
   header/traj/samples staging copies, reuse of the per-message vectors).
   Ordinary buffer hygiene, no protocol or architecture change. Not needed now.

## Environment note

During this audit the compose stack had been recreated from the devcontainer,
which bound a *different* host `session-data` directory than the user's WSL
shell sees (Docker Desktop path-mapping after the WSL restart). The marshal
and viewer share one tree, the repo folder shows another. Standard fix:
bring the stack down and `docker compose --profile test-recon up -d` **from
the WSL terminal in the repo folder** (the normal QUICK_START flow); that
re-binds `session-data` to the repo folder.
