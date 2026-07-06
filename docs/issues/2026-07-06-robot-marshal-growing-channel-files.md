# Robot marshal: channel files grow forever and degrade the whole stack

**Date:** 2026-07-06 · **Reported by:** Eser · **Component:** robot data marshal (`server`, HTTP :8081)

## Summary

The robot marshal stores each channel (force sensing, catheter tracking, controller,
planning, front-end, surface tracking) as a single JSON file that is **appended to on
every write and re-processed in full on every read**. The files grow without bound, so
the CPU cost of every request grows with session length. This is **not a disk-speed
issue** — the files sit in the OS page cache, so reading their bytes is effectively
free; what grows is the work of parsing the entire accumulated content per request.
On the shared demo machine this degraded the MRI pipeline from ~20 fps to ~8–11 fps
within two hours.

## Measured evidence (2026-07-05/06, 8-CPU Docker VM)

1. **File growth:** `file_force_sensing.json` reached **78 MB after ~2 h** of normal
   client traffic. All channel files grow the same way; none is ever truncated.
2. **CPU tracks file size:** robot-marshal CPU was **35 %** with fresh files and
   **117 %** two hours later, with identical client load. Resetting the files to a
   single seed entry dropped CPU back to **35 % instantly**.
3. **It starves the recon:** freezing the robot containers mid-stream
   (`docker pause`, 2026-07-05) visibly raised MRI throughput on the spot
   (sender-side observation). With the robot stack fully stopped (`docker stop`,
   2026-07-06, 400 frames), the recon uses a whole core (~110 % CPU) and the
   pipeline sustains **13–18 recon images/s** — its measured ceiling on this
   machine (see `docs/reviews/2026-07-06-end-to-end-performance-audit.md`).
   The recon needs a full core with zero headroom, so the robot marshal's CPU
   growth slows it one-for-one.
4. **Unbounded console logging:** the server logs every request; Docker's default
   json-file log driver is unlimited, and the container had written a **26 GB** log
   file that filled the disk. (Mitigated stack-side with compose log caps
   `max-size: 50m, max-file: 2`, but the per-request logging itself remains.)
5. **Survives restarts by design:** the container entrypoint copies the previous
   session's JSON files into the new session directory, so a restart carries the
   bloat forward — the degradation never resets on its own.

## Why it happens

- Writes **append** one JSON object to the channel file (the file is a plain
  concatenation of JSON objects — not a JSON array — starting from a seed entry).
- Reads **process the whole file** to return the latest values, so every read costs
  O(file size) CPU. (Inferred from measured behavior — CPU rises in lockstep with
  file size and resets when the file is emptied — not from reading the server
  source; Ridaa can confirm the exact code path.) With six clients polling
  continuously, total CPU grows linearly with session duration.

## Proposed fix (~1 day in the robot server)

Separate "current state" from "history":

1. **Answer requests from in-memory state.** Keep the latest N entries per channel
   as already-parsed data structures inside the server process (N = whatever the
   clients actually need — likely small; Ridaa's call). A request then never
   re-parses the channel file. Note this is different from the file merely being
   cached in RAM — page cache makes the *bytes* cheap to fetch, but re-parsing the
   whole content per request is the cost that grows. This change makes request cost
   constant regardless of session length.
2. **Keep history as an append-only archive** that nothing re-reads during operation
   — no data is lost, it's just not on the request path.
3. **On restart, load only the tail** (last N entries) of the archive to re-seed
   memory, then start a fresh archive file — instead of copying whole files forward
   as the entrypoint does today.
4. **Gate per-request console logging** behind a debug flag; log errors and summaries
   by default.

## Interim workaround (already applied on the demo machine)

Overwrite any channel file >1 MB with a single seed entry
(`{"client_id": "seed", "sent_at": 1, "values": [0.0, 0.0, 0.0]}`) and restart
`cwru-robot-marshal`. This **discards the history accumulated in those files** for
the current session (older `run_*` session folders keep their earlier copies), and
the degradation returns as the files regrow — it restores speed, nothing more.
