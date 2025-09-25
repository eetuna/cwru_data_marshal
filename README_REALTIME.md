# Realtime Add-Ons (≤50 ms) — Minimal Patch

This patch adds *optional* realtime features to the marshal **without changing existing HTTP contracts**:
- Ring buffer (non-blocking ingest)
- File writer with append + `index.jsonl` (instant discoverability)
- Last-Value Cache (LVC) for `/v1/realtime/last`
- Optional WS ingest stub (can be wired later)
- Optional named-pipe tee (if FIFO exists)

## Files added
- `include/realtime.hpp` — Frame types, `FrameQueue`, `LastValueCache`, and `SegmentWriter` (append + index).
- `src/realtime.cpp` — minimal TU.

## Wire-up (minimal)
In `src/marshal_main.cpp`:
- Create a global `FrameQueue` and start a `writer_thread` at startup (session dir under `./data/sessions/<SESSION>`).
- Add handler for `POST /v1/realtime/ingest` (HTTP): enqueues a frame (series, frame_idx, ts_ns; body is payload).
- Add handler for `GET /v1/realtime/last?series=...`: returns `{"series","frame","ts_ns","path","offset","bytes"}`.

If you prefer not to add endpoints, consumers can tail `./data/sessions/<SESSION>/index.jsonl` directly.

## Mode A (Live)
Producers:
```bash
curl -s -X POST \  -H "Content-Type: application/octet-stream" \  --data-binary @frame_000123.bin \  "http://localhost:8080/v1/realtime/ingest?series=T1rt&frame=123&ts_ns=$(date +%s%N)"
```

Consumers (option 1 — endpoint):
```bash
curl -s "http://localhost:8080/v1/realtime/last?series=T1rt"
# → { "series":"T1rt", "frame":123, "ts_ns":..., "path":"segments/00000001.mrd", "offset":..., "bytes":... }
```

Consumers (option 2 — file tail):
```bash
tail -n 1 -F ./data/sessions/<SESSION>/index.jsonl
# read "path,offset,bytes" and pread() the frame from the segment file.
```

## Mode B (Replay)
- Keep your existing `services/playback/playback_main.cpp` that re-POSTs MRDs to `/v1/mrd/ingest`.
- To test the realtime path, re-POST individual frames to `/v1/realtime/ingest`.
- Consumers can still use `/v1/realtime/last` or tail `index.jsonl` to follow along.

## Build
```bash
cmake -S . -B build
cmake --build build -j
```

## Notes
- No per-frame `fsync`; durability is on segment roll/shutdown (can be added later).
- The SegmentWriter writes one *flat* `.mrd` segment and a human-readable `index.jsonl` for discovery.
- Named-pipe tee can be added by writing `(header+payload)` to `/run/marshal/<series>.fifo` if present.
