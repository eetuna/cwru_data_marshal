# Plan: make the marshal reroute slice-translation AND know slice geometry

## Context / problem

The WebGL UI can POST a `±1` slice-translation command, and (after the just-merged
port) the MRI marshal **receives and caches** it at `POST /write/file_slice_translation`.
But two things are missing for it to actually *do* anything:

1. **No consumer / delivery.** Nothing reads `GET /read/file_slice_translation`. The
   scanner-side streamer (`kspace_streamer_main.cpp`) is explicitly "No HTTP" and never
   polls. So the command lands in a cache and stops — the reroute loop is open.
2. **No geometry.** The command is a bare `±1` with no position. The marshal never reads
   `position[3]` / `read_dir` / `phase_dir` / `slice_dir` from the ISMRMRD image headers,
   so it cannot associate the nudge with a real slice location.

Reference for the geometry piece: `eetuna/cwru_data_server` — a single MRD receiver
(like python-ismrmrd-server) that extracted position+orientation into a `FrameInfo`
struct (`mrd_server.cpp::extractFrameInfo`). That logic is what the marshal is missing.

All work is in the MRI marshal code (the `mri_data_marshal` worktree).

---

## Piece 1 — Extract slice position/orientation from image headers

Port the `FrameInfo` idea from `cwru_data_server` into the marshal's image path.

- **Where:** the IMAGE branch in `src/mrd_tcp_listener.hpp` (~line 603, where `ihdr` is
  already a parsed `ISMRMRD::ImageHeader*`) and/or `live_image_store.hpp` where
  `parsed.header` is available.
- **What to read** (fields confirmed present, currently unused):
  `position[3]`, `read_dir[3]`, `phase_dir[3]`, `slice_dir[3]`, plus existing `slice`.
- **Store:** add to `MarshalState` a small struct guarded by a mutex, e.g.
  ```cpp
  struct SliceGeometry { int slice_index; float position[3];
                         float read_dir[3], phase_dir[3], slice_dir[3]; bool valid; };
  std::mutex slice_geom_mtx;
  std::map<int, SliceGeometry> slice_geom;   // keyed by slice index
  ```
  Populate it as each IMAGE arrives (the marshal already forwards the bytes; this only
  *reads* a few header fields — no change to the forwarded stream).
- **Expose:** `GET /read/slice_geometry` (or `/slice_geometry/{index}`) returning the
  cached geometry as JSON, mirroring the existing in-memory `/transform` / `/pose` style.
- **No Eigen dependency required** — store raw float[3]; only add Eigen if vector math is
  actually needed marshal-side (the old server used Eigen for visualization, which the
  marshal doesn't do).

## Piece 2 — Deliver the slice-translation command to the slice controller

Decide the delivery mechanism (the open half of the loop). Two viable options:

- **Option A — pull (poll over HTTP):** the acquisition/sequence controller polls
  `GET /read/file_slice_translation`. Marshal change = none beyond the existing endpoint
  (optionally add consume-on-read if double-application is a concern). Simplest; only
  works if the controlling client can speak HTTP.
- **Option B — push over the existing MRD-TCP back-channel (recommended if the scanner
  can't poll HTTP):** the marshal already has a return path to the scanner
  (`recon_forwarder.hpp` pushes IMAGE/TEXT/CLOSE back). Send the slice command back as an
  `MRD_MESSAGE_TEXT` (id 5) frame on that connection. This bridges the HTTP→MRD-TCP
  protocol gap so the WebGL command reaches the scanner over the channel it already uses.
  - Hook: when `handle_post_slice_translation` accepts a command, enqueue a TEXT frame
    via the forwarder's existing `post_frame(MRD_MESSAGE_TEXT, body)` path.

**Recommendation:** Option B, because the scanner side speaks MRD TCP and explicitly does
no HTTP. Combine with Piece 1 so the TEXT command can carry the *current* slice geometry
+ the ±1 delta (a real reposition instruction), not just an abstract step.

---

## Files to touch (rewrite branch)
- `src/marshal_state.hpp` — `SliceGeometry` struct + map + mutex.
- `src/mrd_tcp_listener.hpp` — read header position/orientation in the IMAGE branch;
  populate `slice_geom`. (Option B) enqueue TEXT command back via forwarder.
- `src/marshal_http.hpp` — `GET /read/slice_geometry`; (Option B) trigger push on POST.
- `src/recon_forwarder.hpp` — confirm/extend `post_frame(MRD_MESSAGE_TEXT, …)` back-path.
- `tests/test_http_handlers.cpp` — geometry endpoint + command-delivery tests.

## Verification
- Unit: extend `unit_http_handlers` — feed a wire IMAGE with known `position`/`*_dir`,
  assert `GET /read/slice_geometry` returns them; assert POST slice-translation triggers a
  TEXT frame (Option B) or is readable (Option A).
- Integration: run marshal + a mock scanner that (B) reads back a TEXT frame after a WebGL
  POST, or (A) polls the read endpoint. Build via the existing
  `cmake --build build --target unit_http_handlers && ctest -R unit_http_handlers`.

## Open decisions for the user
1. Delivery: **Option A (poll)** vs **Option B (push over MRD-TCP back-channel)**.
2. Does the command need real geometry (Piece 1 wired into the delivered message), or is a
   bare ±1 enough for now?
3. Consume-on-read vs keep-cached for the command (currently keep-cached).
