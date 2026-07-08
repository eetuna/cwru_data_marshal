# Slice control — what's done, what Andrew and Ridaa each need to do

**Status (2026-07-07):** the marshal side is implemented, tested (14/14 suite,
e2e against stock python-ismrmrd-server code), merged, and **deployed** on the
demo machine. Full API detail: [API_REFERENCE.md](API_REFERENCE.md). Hands-on
test: QUICK_START step 5f / [TESTING.md](TESTING.md) "Slice command channel".

## How it works (one paragraph)

The marshal reads each image's ISMRMRD header as it passes through and caches
the slice geometry (`position` + `read_dir`/`phase_dir`/`slice_dir`). When the
UI posts a slice command over HTTP, the marshal relays it to the scanner as an
`MRD_MESSAGE_TEXT (5)` JSON message **on the scan's existing MRD TCP
connection** — the same socket the scanner already reads recon images from.
python-ismrmrd-server's receive loop parses TEXT natively, so the scanner gets
a ready-to-use JSON string.

---

## Ridaa — UI side

**Already working (nothing to change):** the ± slice-translation buttons.
They POST to `/write/file_slice_translation`; the marshal now forwards each
press to the scanner as
`{"type":"slice_translation","direction":±1,"slice_geometry":{...current
position/orientation...}}`.

**To build: the absolute control.** A UI selection (e.g. a draggable/rotatable
plane in the 3D view) that produces a slice center + plane axes and posts:

```
POST http://mri-marshal:8080/write/slice_target
{"position": [12.5, -3.0, 40.0],
 "read_dir": [1,0,0], "phase_dir": [0,1,0], "slice_dir": [0,0,1]}
```

- `position` = slice center in mm. Plane widget mapping: normal → `slice_dir`,
  the two in-plane edge directions → `read_dir` / `phase_dir`.
- Vectors must be unit length and mutually perpendicular; otherwise the
  marshal answers `400` naming the offending vector and sends nothing.
- The response's `"delivered": true|false` says whether a scanner was
  connected to receive it (`false` = cached only) — surface this in the UI.
- Initialize the widget from the live slice location:
  `GET http://mri-marshal:8080/read/slice_geometry`.
- Testing without a scanner: `GET /read/slice_target` echoes the last
  prescription; or run `scripts/slice_command_mock_scanner.py` as a fake
  scanner and watch it print the received command (QUICK_START 5f).

**Relative controls (added 2026-07-08, deployed):** if your buttons make
incremental moves (translate a bit / rotate a bit), post them as-is:

```
POST http://mri-marshal:8080/write/slice_delta
{"translation_mm": [dx, dy, dz], "rotation_rad": [rx, ry, rz]}
```

Either field alone is fine (the other defaults to zero). The scanner receives
the deltas together with the current slice geometry as the base of the move.
Recommended UI shape: **relative buttons for normal use + an absolute "reset"
using `slice_target`** (seeded from `GET /read/slice_geometry`) to snap the
slice back to a known state if anything gets out of sync.

**Planned:** once `slice_target` is integrated in the UI, the ±1
`file_slice_translation` endpoint becomes redundant and will be removed —
`slice_delta` and `slice_target` are the long-term pair.

---

## Andrew — scanner side

**Receiving (trivial, ~20 lines):** your MRD receive loop (connection.py
style) already parses TEXT messages into strings. Handle them:

```python
for msg in connection:
    if isinstance(msg, str):
        cmd = json.loads(msg)
        if cmd.get("type") == "slice_target":
            # cmd["position"], cmd["read_dir"], cmd["phase_dir"], cmd["slice_dir"]
            apply_prescription(cmd)
        elif cmd.get("type") == "slice_translation":
            # cmd["direction"] = +1/-1; cmd["slice_geometry"] = current geometry (or null)
            step_slice(cmd)
        continue
    ...existing image/close handling...
```

A complete working receiver is `scripts/slice_command_mock_scanner.py` — run
it against the deployed marshal to see the exact messages.

**The real work:** making the sequence act on it — re-prescribing the slice at
`position` with axes `read_dir`/`phase_dir`/`slice_dir` (all in the same
patient/scanner coordinates your image headers already use, so it round-trips
with what you emit), and defining what one `slice_translation` step means
(suggest: one slice thickness+gap along `slice_dir`).

**Two decisions to confirm back:**
1. Units/coordinate convention OK as stated (mm, image-header convention)?
2. When are commands honored — mid-scan (real-time reposition) or between
   scans? The channel delivers them mid-scan; what the sequence does with the
   timing is yours.
