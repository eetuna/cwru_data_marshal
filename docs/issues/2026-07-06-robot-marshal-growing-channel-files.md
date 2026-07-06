# Robot marshal: channel files grow forever and degrade the whole stack

**Date:** 2026-07-06 · **Reported by:** Eser · **Component:** robot data marshal (`server`, HTTP :8081)

## Summary

The robot marshal stores each channel (force sensing, catheter tracking, controller,
planning, front-end, surface tracking) as a single JSON file that is **appended to on
every write and re-read in full on every read**. The files grow without bound, so the
cost of every request grows with session length. On the shared demo machine this
degraded the MRI pipeline from ~20 fps to ~8–11 fps within two hours.

## Measured evidence (2026-07-05/06, 8-CPU Docker VM)

1. **File growth:** `file_force_sensing.json` reached **78 MB after ~2 h** of normal
   client traffic. All channel files grow the same way; none is ever truncated.
2. **CPU tracks file size:** robot-marshal CPU was **35 %** with fresh files and
   **117 %** two hours later, with identical client load. Resetting the files to a
   single seed entry dropped CPU back to **35 % instantly**.
3. **It starves the recon:** pausing the robot containers mid-stream raised MRI
   kspace throughput from **8–11 fps to 15–20 fps** on the spot. With the robot stack
   fully stopped, the recon gets a whole core (~110 % CPU) and the pipeline sustains
   its full **20 fps** (400-frame test, 2026-07-06).
4. **Unbounded console logging:** the server logs every request; Docker's default
   json-file log driver is unlimited, and the container had written a **26 GB** log
   file that filled the disk. (Mitigated stack-side with compose log caps
   `max-size: 50m, max-file: 2`, but the per-request logging itself remains.)
5. **Survives restarts by design:** the container entrypoint copies the previous
   session's JSON files into the new session directory, so a restart carries the
   bloat forward — the degradation never resets on its own.

## Why it happens

- Writes **append** a JSON entry to the channel file (the files are concatenated JSON
  objects, starting from a seed entry).
- Reads apparently **parse the whole file** to return the latest values, so every
  read is O(file size). With six clients polling continuously, total CPU grows
  linearly with session duration.

## Proposed fix (~1 day in the robot server)

Separate "current state" from "history":

1. **Serve reads from memory.** Keep the latest N entries per channel in RAM; reads
   never touch the disk file. This makes request cost constant regardless of session
   length.
2. **Keep history as an append-only archive** that nothing re-reads during operation
   — no data is lost, it's just not on the hot path.
3. **On restart, load only the tail** (last N entries) of the archive to re-seed
   memory, instead of copying whole files forward.
4. **Gate per-request console logging** behind a debug flag; log errors and summaries
   by default.

## Interim workaround (already applied on the demo machine)

Reset channel files >1 MB to a single seed entry
(`{"client_id": "seed", "sent_at": 1, "values": [0.0, 0.0, 0.0]}`) and restart
`cwru-robot-marshal`. Works, but the degradation returns as the files regrow.
