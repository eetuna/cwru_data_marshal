# Slice control — how the channel works, what each side needs to do

**Status (2026-08-25):** the marshal now drives Andrew's scanner-side
`slice_agent` directly (branch `feat/slice-agent-client`). Unit tests: 15/15
targets; local e2e verified against the real `slice_agent` + `shm_peek` from
`dynamic-slice-position-main/agent`. Full API detail:
[API_REFERENCE.md](API_REFERENCE.md). Hands-on test: [TESTING.md](TESTING.md)
"Slice command channel".

## The chain (one paragraph)

```
WebGL buttons ──HTTP──▶ write-server ──HTTP──▶ marshal :8080 ──TCP 9270──▶ slice_agent --listen (MARS)
                                                                                      │ shared memory /slice_xfm
                                                                                      ▼
                                                                    RadialCardiac2D sequence (reads once per frame)
```

The UI posts a slice command (`slice_delta` for relative moves, `slice_target`
for an absolute prescription). The marshal turns it into an **absolute slice
geometry** (center in mm + read/phase/normal directions, scanner PCS), converts
that to the agent's wire form (three Euler angles in degrees + center) and sends
a 56-byte `SliceCommand` packet to `slice_agent --listen` on the MARS. The agent
publishes it to shared memory; the sequence applies it at the start of its next
image frame. Nothing is sent on the scanner's MRD socket any more (the earlier
JSON-text relay had no receiver and was removed).

## Turning it on (compose)

```
SLICE_AGENT_HOST=<MARS ip> docker compose up -d
```

`SLICE_AGENT_HOST` unset/empty = channel off (endpoints still cache, answer
`"enabled": false`). `SLICE_AGENT_PORT` defaults to 9270. `SLICE_AGENT_EXTRA`
carries the axis switches settled by the scanner check below, e.g.
`SLICE_AGENT_EXTRA="--slice-transpose"`.

Scan-day order (matters — see "Facts about the scanner side"):
1. On the MARS: `./slice_agent --listen`.
2. Start/restart the marshal with `SLICE_AGENT_HOST` set.
3. Only then switch the sequence's WIP toggle **"Dynamic Slice Control"** on.

## Ridaa — UI side (nothing to change for the channel to work)

- `±` buttons → `POST /write/slice_delta {"translation_mm":[0,0,±1]}` = one mm
  along the slice normal. Rotation sliders → `{"rotation_rad":[r,0,0]}` etc. =
  rotation about the slice's own read / phase / normal axis.
- "Send Absolute Position N" → `POST /write/slice_target` with the saved header
  pose. Unchanged.
- Conventions are now pinned: **translation and rotation deltas are both in
  the slice's own frame** (read, phase, normal). The old doc line "translation
  in scanner-frame mm" was wrong and is fixed.
- Response fields worth showing: `delivered` (packet written to a connected
  agent), `agent_connected`, `enabled`, `base` (`"commanded"` or
  `"image_header"`), `geometry` (the absolute geometry that was sent).
  Note: the write-server wraps the marshal reply as
  `{backend_response: {...}}`, so read `backend_response.delivered` — today
  the UI reads `result.delivered`, which is always undefined ("cached").
- A relative move before any image of the scan and before any `slice_target`
  returns **409** (no base geometry). A `slice_target` always works.
- `GET /read/slice_commanded` returns the last absolute geometry sent plus the
  wire angles and agent status — useful for a "where is the slice now" display,
  because the scanner does not write moved positions back into image headers.

## Andrew — scanner side

Nothing to build for the channel: the marshal is a client of your
`slice_agent --listen` exactly per `agent/PROTOCOL.md` (56-byte LE
`SliceCommand`, absolute values, `0xDEAD` on shutdown). Findings from reading
the two folders that you may want to address (none block the marshal work):

1. **No MDH geometry write in RadialCardiac2D.** `DynamicSlicePos.cpp:586-620`
   stamps the applied position/rotation into the ADC MDH; the cardiac sequence
   has no equivalent, so image headers (and DICOM) keep the prescribed
   geometry while the excitation moves. The marshal therefore keeps its own
   "commanded geometry" as the base for relative moves instead of trusting
   headers after the first command.
2. **Frame-counter lockout after reconnect.** The agent restarts `frame` at 0
   per TCP client; the sequence accepts only `frameIndex >= m_lastFrame`
   (`RadialCardiac2D.cpp:660`) and `prepare()` never resets `m_lastFrame`.
   After any reconnect (agent restart, network blip) commands are ignored
   until the counter passes the old value. The marshal holds one connection
   for the whole session and re-sends after a reconnect, but it cannot fix
   this. Suggest: reset `m_lastFrame` in `prepare()` and/or accept any
   change (`!=`) instead of `>=`.
3. **Identity on connect/disconnect** (`slice_agent.cpp:282-320`) moves the
   slice to an axial plane at isocenter, not to the prescribed slice
   (`applySliceTransform` is absolute; PROTOCOL.md's "offsets from nominal"
   and "reset to nominal" are wrong). The marshal never connects idle and
   re-sends its last geometry for ~2 s after every connect to cover this.
4. **Toggle on + no agent = zero matrix.** `shm_open(O_CREAT)` creates a
   zero-filled segment that passes the seqlock check; the cardiac sequence
   then applies an all-zero rotation matrix. Folder 1 seeds nominal geometry
   in `prepare()`; the cardiac sequence should too (or reject a singular
   matrix).
5. **Read gate on reversed repetitions.** `lProjection == 0` (`:657`) lands on
   the *last* spoke in every other dense repetition (`run():540-543, 582`), so
   a frame can straddle two geometries. Gate on `lProj == 0`.
6. `check()` also runs the shm read (arm 0) and advances `m_lastFrame` before
   `run()`.
7. Row/column: `buildRotMatrix` builds `R = Rz·Ry·Rx` with the axes as
   **columns**; `SliceXfm.h`/`applySliceTransform` read the rows as
   read/phase/normal. Unless `sROT_MATRIX` is column-major these differ by a
   transpose. The marshal defaults to the row reading and has
   `--slice-transpose` for the other case — please confirm which is right.
8. `RadialCardiac2D.cpp:434` sets the online ICE program on `rSeqLim` instead
   of `rSeqExpo`.

## One-time check on the scanner (settles the axis mapping)

The mapping from ISMRMRD header directions to the sequence's rotation rows
goes through closed-source Siemens code and cannot be proven offline.

1. `slice_agent --listen` running, marshal up with `SLICE_AGENT_HOST`, toggle
   on, scan running, first image received.
2. `curl -s localhost:8080/read/slice_geometry` → copy the `position` and
   three direction vectors of `latest_slice`.
3. `POST /write/slice_target` with exactly those values.
4. **Expected: the image does not move.**
   - rotated ~90° in-plane → add `--slice-swap-read-phase` to `SLICE_AGENT_EXTRA`
   - mirrored / wrong side of isocenter → `--slice-axis-sign` (e.g. `+,-,+`)
   - tilted the opposite way after a rotation delta → `--slice-transpose`
5. Then press `+` once: the slice must move 1 mm along its normal.

## Facts about the scanner side (verified in the code, drive the marshal design)

- Wire: TCP, port 9270, one client, no framing, no replies. 56 bytes LE:
  `tx ty tz` (mm) `rx ry rz` (deg) `flags` (0 / 0xDEAD) `pad`.
- Values are **absolute**. Zeros = axial slice at isocenter.
- Agent publishes identity on every connect and disconnect; frame counter
  restarts per connection.
- Sequence reads shm once per image frame; ignores frames numbered below the
  last one it accepted; falls back to the prescribed slice only when the shm
  read itself fails.
- Cardiac sequence does not write the moved geometry into image headers.
