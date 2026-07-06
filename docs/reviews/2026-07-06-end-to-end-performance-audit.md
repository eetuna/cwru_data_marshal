# End-to-end performance audit — where the fps actually goes

**Date:** 2026-07-06 · **Setup:** scanner (fire_stream) → mri-marshal → bundled test
recon → webgl, robot stack stopped, 8-CPU Docker/WSL2 VM, 128-matrix single-slice
k-space frames (1.09 MB/frame), images built 2026-07-06 04:55–05:04.

## Measurement method (important)

Sender-side fps (fire_stream's `streamed N frames` heartbeat) is NOT pipeline speed —
the marshal buffers ahead, so the sender can report 20 fps while the recon produces
far fewer images/s. All rates below are **recon output images/s**, measured from the
marshal's `/debug/perf` `recv.recon_images` counter sampled per interval, and
cross-checked against the scan archive (start time in the filename, end time in the
file mtime, image count in the HDF5). Use this method for all future checks.

## Stage-by-stage ceilings (each stage isolated)

| Stage | How measured | Ceiling |
|---|---|---|
| Test streamer (fire_stream) | unpaced into a pure byte sink (`mrd_bench.py sink`) | **35 fps** (~28 ms/frame of numpy work) |
| **Marshal alone** | byte-pump `blast` → marshal → byte-sink as recon | **74 fps / 81 MB/s** |
| **Test recon alone** | fire_stream unpaced direct to recon:9002, recon log timestamps | **10.6 images/s**, CPU pinned ~110 % (one core) |
| Full chain, unpaced | 400 frames, per-2 s counter sampling | 13.1 images/s avg (bursts 18–22/s, intermittent dips) |
| Full chain, paced 20 fps | 400 frames, user's exact command | **18.3 images/s** (400 images in 21.8 s), sender never throttled |

## Conclusions

1. **The bottleneck is the bundled test recon.** It is a single-core Python process:
   ~110 % CPU while working, 10–20 images/s depending on conditions. Every other
   stage has 2–7× headroom.
2. **The marshal is ruled out.** 74 fps / 81 MB/s at the exact same frame format —
   4–7× faster than anything feeding or draining it. Tripwires clean throughout
   (`coalesced=0`, `completed==enqueued`, `dropped_oldest=0`).
3. **Feeding the recon through the marshal is faster than feeding it directly**
   (18.3/s vs 10.6/s here; same direction seen on 2026-07-05: 11 vs 7.7). The
   marshal's batched writes make the recon's socket reads more efficient.
4. **Expected real-world rate: ~13–18 images/s on a quiet machine, less whenever
   anything else uses CPU.** The recon has zero headroom — it needs a full core to
   keep up, so any CPU theft slows it one-for-one. Day-to-day fps variation
   ("20 fps yesterday, 7 today") is machine load, not code changes.
5. **The 2026-07-06 ~6 images/s incident:** two runs at 10:33 produced a uniform
   4–7 images every second (recon log timestamps) — steadily ~3× slower than the
   identical containers/images/command measured 20 min later (18.3/s). VM load
   average over that window was ~1.0, i.e. nothing inside Docker/WSL was competing.
   The slowdown came from outside the Linux VM (Windows-side load or hypervisor CPU
   allocation). Not reproducible; not a stack defect.
6. **April implication:** the test recon's ceiling is irrelevant to the experiment —
   the real recon on its own hardware sets the rate. The number to obtain is the
   real recon's images/s at the experiment's matrix/slices; the marshal will not be
   the limiter.

## Untested lead

The test recon runs with `-v` (verbose) and prints a full XML metadata dump per
image to its log; dropping the flag may raise its ceiling. Not measured.

## Reference

Marshal-only harness and procedure: `scripts/mrd_bench.py`,
`docs/reviews/2026-07-03-marshal-performance-audit.md`. Robot-stack contention
findings: `docs/issues/2026-07-06-robot-marshal-growing-channel-files.md`.
