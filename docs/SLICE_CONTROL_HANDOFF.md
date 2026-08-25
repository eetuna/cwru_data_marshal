# Slice control — how it works, what each side needs

**Status (2026-08-25):** the marshal drives Andrew's scanner-side `slice_agent`
directly and behaves exactly like his `slice_control` keyboard tool
(branch `feat/slice-agent-client`). Unit tests include a verbatim port of
`slice_control.cpp` driven with random key sequences against the marshal's
arithmetic — byte-identical output. Local e2e verified against the real
`slice_agent` + `shm_peek`. API detail: [API_REFERENCE.md](API_REFERENCE.md).
Hands-on test: [TESTING.md](TESTING.md) "Slice command channel".

## The chain

```
WebGL buttons ──HTTP──▶ write-server ──HTTP──▶ marshal :8080 ──TCP 9270──▶ slice_agent --listen (MARS)
                                                                                      │ shared memory /slice_xfm
                                                                                      ▼
                                                                    RadialCardiac2D sequence (reads once per frame)
```

## What the marshal does (= what Andrew's `slice_control` does)

Six absolute numbers, `tx ty tz` (mm) and `rx ry rz` (degrees), start at zero:

| WebGL control | Andrew's key | Effect on the six numbers |
|---|---|---|
| Slice `+` / `−` | PgUp / PgDn | `(tx,ty,tz) += ±1 mm × row 2 of buildRotMatrix(rx,ry,rz)` (slice normal) |
| Rotation X slider, *d*° | W / S | `rx += d` |
| Rotation Y slider, *d*° | D / A | `ry += d` |
| Rotation Z slider, *d*° | E / Q | `rz += d` |
| (`slice_delta` in-plane) | arrows | `(tx,ty,tz) += step × row 0 / row 1` |
| `POST /write/slice_reset` | `0` | all six = 0 |
| "Send Absolute Position" (`slice_target`) | — | six numbers computed from the header pose (see below) |

After every change the six totals are sent as one 56-byte `SliceCommand`;
`slice_agent` builds the rotation with the same `buildRotMatrix` and publishes
it to shared memory; the sequence applies it at the start of its next frame.
Zero = the identity slice (axial through isocenter) — which is also what the
agent publishes on connect, so a session starts exactly where Andrew's does.
The numbers are cleared at every scan start (new prescription, fresh zero).

Nothing goes on the scanner's MRD socket (the earlier JSON-text relay had no
receiver and was removed).

## Turning it on (compose)

```
SLICE_AGENT_HOST=<MARS ip> docker compose up -d
```

Unset/empty = channel off (endpoints still cache; answer `"enabled": false`).
`SLICE_AGENT_PORT` defaults to 9270. `SLICE_AGENT_EXTRA` can carry
`--slice-max-step-mm`, `--slice-max-step-deg`, `--slice-max-abs-mm`,
`--slice-nudge-mm`, `--slice-resend-ms`.

On the scanner: `./slice_agent --listen` running on the MARS (Andrew's binary,
already exists), then the sequence with WIP "Dynamic Slice Control" on. That
is the same setup Andrew uses with his keyboard tool; the marshal is just the
client instead of `slice_control`.

## Ridaa — UI side (nothing to change for the channel to work)

- `±` → `POST /write/slice_delta {"translation_mm":[0,0,±1]}`; sliders →
  `{"rotation_rad":[r,0,0]}` etc.; "Send Absolute Position" →
  `POST /write/slice_target`. All unchanged.
- Response fields worth showing: `delivered` (packet written to a connected
  agent), `agent_connected`, `enabled`, `state` (the six numbers), `geometry`.
  The write-server wraps the marshal reply under `backend_response`; branch
  `feat/slice-agent-client-webgl` already reads `backend_response.delivered`.
- Useful additions if wanted: a "reset" button (`POST /write/slice_reset`)
  and a readout of `GET /read/slice_commanded` (the six numbers — the scanner
  does not write the moved position back into image headers, so this is the
  only "where is the slice now").

## `slice_target` (the one thing outside Andrew's tested path)

Ridaa's "Send Absolute Position" posts a pose taken from an image header
(`position` + `read_dir/phase_dir/slice_dir`). The marshal takes those vectors
as the rows of `buildRotMatrix` (the way `slice_control` itself reads that
matrix for its translation keys), converts them to `rx ry rz`, and sends the
six numbers. It is validated (unit, orthogonal, right-handed, position clamp)
and works consistently with the `±`/slider path. But Andrew has never sent a
header-derived pose through his agent, so whether the header's read/phase
axes coincide with the sequence's rows is unverified. If a `slice_target` of
the current image pose ever visibly moves the slice, that is the reason —
the `±`/slider path is unaffected either way.

## Andrew — scanner side (facts from his code; nothing required)

1. Values are **absolute** (`applySliceTransform` replaces the prescription);
   `PROTOCOL.md`'s "offsets from nominal / reset to nominal" wording is wrong.
   Zero = axial at isocenter.
2. `slice_agent` publishes identity on every connect and disconnect; the
   marshal re-sends its current numbers right after each connect.
3. The agent restarts its frame counter per TCP connection and the cardiac
   sequence never resets `m_lastFrame` (`RadialCardiac2D.cpp:660`), so after
   a reconnect mid-scan commands are ignored until the counter catches up.
   The marshal keeps one connection per session and reports `reconnects` in
   `/status`.
4. The cardiac sequence does not write the moved geometry into the MDH
   (folder 1's `DynamicSlicePos.cpp:586-620` does) → image headers/DICOM keep
   the prescribed pose.
5. Toggle on with no agent running → zero-filled shm → all-zero rotation
   matrix applied. Start the agent first (as he does).
6. Read gate `lProjection == 0` lands on the *last* spoke in reversed
   repetitions (`run():540-543, 582`); `check()` also reads the shm.
7. `RadialCardiac2D.cpp:434` sets the online ICE program on `rSeqLim`
   instead of `rSeqExpo`.
