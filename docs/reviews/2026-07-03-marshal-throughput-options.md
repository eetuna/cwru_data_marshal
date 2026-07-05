# Marshal throughput/latency improvement options — full analysis (2026-07-03)

Decision document: every performance-relevant path in the marshal, what it
costs today (measured where possible), and the conventional improvement for
each. Nothing here is implemented; each item is a yes/no for the user.

## Measured baseline (2026-07-03 audit, 96×96×8-coil×5-slice frames)

| Path | Today |
|---|---|
| Direct wire (no marshal) | 310 MB/s, 47.7k acq/s |
| Through marshal (forward) | 75 MB/s, **11.5k acq/s = 24 fps**, one core pegged |
| Marshal-added latency | **+32 ms per 480-acq frame** |
| Snapshot publish (latest_image.h5) | 2.5 ms typical, **0.5–3 s stalls** on the bind mount; ~430/s ceiling; drop-oldest at 64 |
| Viewer read (HTTP pointer + h5py) | 16–94 ms per poll, 20 polls/s regardless of change |
| Return relay (recon→scanner) | ≥1,600 img/s — not a limiter |

---

## A. Ingest/forward path (scanner → marshal → recon)

**A1 — Buffered socket reads.** Today each acquisition costs ~3 `recv` syscalls
(tag, header, samples). A userspace read buffer (read 64–256 KB per syscall,
parse messages out of it) amortizes to <1 syscall per acquisition. Textbook
buffered I/O.
**A2 — Buffer reuse + assemble-once.** Today each acquisition allocates 3
vectors and memcpys the parts into a 4th; the forwarder then copies the whole
frame *again* into a 5th (tag+body). Persistent scratch buffers + reading
directly into the final layout + `writev` (tag,body as 2 iovecs) removes ~4
allocations and ~13 KB of copies per acquisition.
**Expected (A1+A2):** ~2–3× on the acq/s ceiling (est. 24→~50–70 fps for
5-slice frames); marshal latency ~32→~12–15 ms/frame. Verified before/after
with the existing `mrd_bench.py` R2/R3.
**Effort M. Risk: touches the protocol parser — needs the full regression
suite + bench + live k-space test. No client/recon coordination (wire bytes
unchanged).**

**A3 — Two-thread pipeline (parse thread | forward thread).** Only worth it if
a future config needs more than A1+A2 deliver. Architecture change, ordering
care. **Not recommended now; revisit only if the real recon outruns the
post-A1/A2 marshal.**

## B. Return path (recon → marshal → scanner + archive)

**B1 — Buffer reuse on recon reads.** Fresh vector per return message today
(184 KB–3 MB images). Reuse one growable buffer. **Effort S.**
**B2 — Move, don't copy, into archival.** `append_live_image` deep-copies the
image body; the group-publish path copies the whole accumulated volume again
(under `scan_mtx`). Pass by move / `std::move` where the source dies anyway.
Removes 1–2 large copies per image. **Effort S. No coordination.**

## C. Live-view path (publish + read)

**C1 — Snapshot on tmpfs (RAM-backed file). The low-coordination cache.**
Keep the exact same API (`/image/latest` → path → h5py open) but put
`latest_image.h5` on a tmpfs mount: writes and reads become memory-speed, the
0.5–3 s bind-mount stalls disappear, and the per-scan archive on disk is
untouched (data still saved for later — the user's requirement). Change =
a `--latest-dir` flag + a tmpfs line in docker-compose. Viewer unchanged.
**Effort S. Risk minimal.**

**C2 — Full in-memory cache + serve over HTTP + sequence number.** Marshal
keeps the newest volume in RAM with a `seq`; `/image/latest` reports `seq`;
new endpoint serves the bytes; viewer fetches only when `seq` changes (kills
the 20 Hz blind re-read too). Disk archival unchanged. Supersedes C1.
**Effort M. Needs Ridaa (viewer reads a new endpoint).**

Recommendation: C1 now (free win, zero coordination), C2 when a viewer change
can be scheduled. C1→C2 is not wasted work (C1 is one flag).

## D. Archival path

**D1 — Background spool→HDF5 conversion on NORMAL scan close.** Today the
convert runs synchronously in the CLOSE handler: with a long scan, the next
scan on the *same scanner connection* (real-scanner behavior) waits seconds.
Move conversion to a worker (the abnormal-EOF path already got this shape in
the fix stack, with the epoch guard). **Effort M. Risk: stem-reuse care —
pattern already exists.**
**D2 — `--no-convert` option for max-rate dump sessions** (convert offline
after the experiment; spool is already the byte-exact record). **Effort S.**

## E. Not worth doing (checked, rejected)

- Async HTTP server: thread-per-connection is fine at 1–3 clients.
- Batching acquisitions before forwarding: adds latency for throughput nobody
  needs after A1/A2.
- Replacing HDF5 archive format: archives are the analysis interface; keep.
- Custom TCP image protocol: HTTP is TCP; no gain, new client code.

---

## Suggested picks by goal

| Goal | Pick |
|---|---|
| "Marshal should never be the bottleneck vs the real recon" | **A1+A2** |
| "Live view fast + stall-proof, zero client changes" | **C1** |
| "Cheapest wins" | B2, C1, D2 |
| "Back-to-back scans without a gap" | D1 |
| Bigger redesigns | none recommended |

All items keep wire-protocol bytes identical and archives complete.
