# Quick Start

Follow the steps in order. Every option at every step is a complete
copy-paste command — pick the one that matches what you want.

**Run every command from the repo folder** (the one containing
`docker-compose.yml`) — several commands mount `./scripts` and
`./session-data` from the current directory.

---

## Step 1 — Build the images (once)

```bash
./scripts/build-client-images.sh
```

Builds the 5 images the stack runs: `cwru/mri-marshal`, `fire-python`
(test recon + test clients), `cwru/robot-marshal`, `cwru/robot-clients`,
`cwru/webgl-client`. First build takes ~10–20 min (downloads + compiles);
rebuilds are fast. Success ends with `Build complete! All 5 images ready.`

---

## Step 2 — Start the stack (pick ONE command)

Every configuration has exactly five settings. Every command below states
what you get for **all five**, so nothing is implicit:

> **recon** (test or real) · **mode** (live or dump) · **data folder** ·
> **snapshot** (RAM or disk) · **ports**

**2a. Demo / testing** (the usual choice):
```bash
docker compose --profile test-recon up -d
```
> You get: recon = **bundled test recon** · mode = **live** (viewer updates
> during scan; archives images+ECG) · data → **./session-data** · snapshot =
> **RAM** · ports = **3000 UI / 8080 API / 9100 scanner / 8081 robot**

**2b. Production — real recon:**
```bash
RECON_HOST=10.0.0.5 RECON_PORT=9002 docker compose up -d
```
> You get: recon = **yours at 10.0.0.5:9002** · mode = **live** · data →
> **./session-data** · snapshot = **RAM** · ports = **defaults as above**

**2c. Recording session — test recon, dump mode** (records everything
including raw k-space; viewer blank by design):
```bash
MARSHAL_DUMP=--dump docker compose --profile test-recon up -d
```
> You get: recon = **bundled test recon** · mode = **dump** (full archive to
> `session-data/dump/`, no live view) · data → **./session-data** · snapshot =
> **none** (dump mode has no snapshot) · ports = **defaults**

**2d. Recording session — real recon, dump mode:**
```bash
MARSHAL_DUMP=--dump RECON_HOST=10.0.0.5 RECON_PORT=9002 docker compose up -d
```
> You get: recon = **yours** · mode = **dump** · data → **./session-data** ·
> snapshot = **none** · ports = **defaults**

**2e. MRI-only — no robot** (scanner → marshal → recon → viewer; the robot
marshal and the 6 robot data clients are not started at all):
```bash
docker compose --profile test-recon up -d --no-deps mri-marshal recon webgl-client
```
> You get: recon = **bundled test recon** · mode = **live** · data →
> **./session-data** · snapshot = **RAM** · ports = **3000 UI / 8080 API /
> 9100 scanner** (no robot, so nothing on 8081). The viewer's robot panel
> stays empty and webgl logs harmless robot-connection errors — expected.
> Measured 2026-07-06: this subset sustains the full 20 fps kspace stream.

For MRI-only with a **real** recon: same idea, drop the profile and the recon
service, set the recon address:
```bash
RECON_HOST=10.0.0.5 RECON_PORT=9002 docker compose up -d --no-deps mri-marshal webgl-client
```

If the **full stack is already running** and you want to switch to MRI-only,
stop the robot side (everything else keeps running):
```bash
docker stop cwru-robot-marshal cwru-catheter-tracking cwru-force-sensor \
  cwru-controller cwru-planning cwru-front-end cwru-surface-tracking
```
To bring the robot side back later:
```bash
docker compose --profile test-recon up -d
```

### Changing the remaining settings — add to the front of ANY command above

Each example shows the FULL command and the FULL result:

**Different data folder:**
```bash
SESSION_DATA_DIR=/data/experiment1 docker compose --profile test-recon up -d
```
> You get: test recon · live · data → **/data/experiment1** · snapshot = RAM · default ports

**Snapshot on disk instead of RAM** (you normally never need this — the
snapshot is the viewer's throwaway current-frame file; RAM just makes it
stall-proof; your scan data is on disk either way):
```bash
MARSHAL_LATEST= docker compose --profile test-recon up -d
```
> You get: test recon · live · data → ./session-data · snapshot = **disk**
> (`session-data/live/.../latest_image.h5`) · default ports

**Different ports:**
```bash
UI_PORT=13000 HTTP_PORT=18080 MRD_PORT=19100 docker compose --profile test-recon up -d
```
> You get: test recon · live · data → ./session-data · snapshot = RAM ·
> ports = **13000 UI / 18080 API / 19100 scanner / 8081 robot**

**Several at once** (they always stack):
```bash
UI_PORT=13000 SESSION_DATA_DIR=/data/exp1 MARSHAL_DUMP=--dump \
RECON_HOST=10.0.0.5 RECON_PORT=9002 docker compose up -d
```
> You get: recon = **yours** · mode = **dump** · data → **/data/exp1** ·
> snapshot = none (dump) · ports = **13000 UI**, others default

Rule: bundled recon = keep `--profile test-recon`, don't set `RECON_HOST`.
Real recon = set `RECON_HOST`, drop the profile. Never both.

---

## Step 3 — Verify it's up

```bash
docker compose ps            # every service "healthy" / "running"
curl localhost:8080/status   # use your HTTP_PORT here if you changed it in step 2
```

`docker compose ps` should list: `cwru-mri-marshal` (healthy),
`cwru-robot-marshal` (healthy), `cwru-webgl-client`, the six robot data
clients — and `cwru-recon` only if you used the test-recon profile.
(With MRI-only **2e**: just `cwru-mri-marshal`, `cwru-recon`,
`cwru-webgl-client`.)

Healthy `/status` looks like:
```json
{"mode":"live", "scanner_connected":false, "recon":{"configured":true,"connected":false},
 "scan":{"active":false}, "disk_free_gb":41.3, ...}
```
`scanner_connected` and `recon.connected` are `false` until a scan starts —
that's normal. (`recon.connected` turns `true` on the first k-space scan.)
`/status` is the first thing to check whenever anything seems wrong.

---

## Step 4 — Open the viewer

Open **http://localhost:3000** in a browser (your `UI_PORT` if you changed
it) — *before* sending data, so you don't miss the stream. It shows the robot
scene immediately; the MRI panel stays empty until the first image arrives in
step 5. In dump mode the MRI panel stays blank the whole time — by design.

---

## Step 5 — Send data (pick ONE command)

(If you started in dump mode: all of these still work, data is archived, but
the viewer stays blank — that's the mode, not a failure.)

**5a. Real scanner.** Point it at `<marshal-host>:9100` over raw MRD TCP
(the host's IP on your network; port = your `MRD_PORT` if you changed it).
Nothing else to run — the marshal forwards k-space to the recon and pushes the
reconstructed images back to the scanner and to the viewer automatically.

**5b. Moving test phantom through the recon** — the full-pipeline test
(scanner→marshal→recon→viewer). Ctrl-C to stop:
```bash
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/fire_stream.py --address mri-marshal --port 9100 \
  --mode kspace --fps 10 --frames 0 --matrix 128 --slices 1
```
> You'll see: a bright dot orbiting the phantom in the viewer, updating a few
> times per second (rate limited by the test recon); the terminal prints a
> once-per-second `streamed N frames` heartbeat.

**5c. Same, multislice:**
```bash
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/fire_stream.py --address mri-marshal --port 9100 \
  --mode kspace --fps 10 --frames 0 --matrix 96 --slices 5
```
> You'll see: the same orbit, but noticeably slower volume updates (~5
> volumes/s) — the *test* recon reconstructs slices one at a time; a real
> recon lifts this. The pipeline itself is not the limit.

**5d. Moving test phantom straight to the viewer** — bypasses the recon
entirely; use this to judge viewer smoothness at full speed:
```bash
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/fire_stream.py --address mri-marshal --port 9100 \
  --mode image --fps 15 --frames 0 --matrix 128 --slices 1
```
> You'll see: smooth orbit at the full requested fps. If 5d is smooth but 5b
> is choppy, the difference is the test recon — nothing else.

**5e. One-shot static push** — a single dataset through the recon, then exits
(no motion; good for a minimal end-to-end check):
```bash
docker run --rm -v "$PWD/session-data:/data" fire-python:latest \
  python3 generate_cartesian_shepp_logan_dataset.py -o /data/phantom.h5
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest \
  python3 client.py -c invertcontrast -o /data/out.h5 --address mri-marshal --port 9100 /data/phantom.h5
```
> You'll see: one static (contrast-inverted) phantom image appear in the
> viewer; the recon's returned images are also saved to `session-data/out.h5`.

One-shot **multislice** variant (5e's generator is single-slice only; this
sends exactly one 5-slice volume through the recon, then exits — the scan
closes immediately, so the archive lands right away):
```bash
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/fire_stream.py --address mri-marshal --port 9100 \
  --mode kspace --fps 5 --frames 1 --matrix 96 --slices 5
```

**5f. Slice-command check** — verifies the UI→marshal→scanner control channel
with a mock scanner (no real scanner needed). Two terminals:

Terminal 1 — a mock scanner connects and waits for a command:
```bash
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/slice_command_mock_scanner.py mri-marshal 9100
```
> You'll see: `IMAGE_SENT slice=2 position=(10.5,-20.0,30.0)`, then it waits.

Terminal 2 — read the slice geometry, then send an absolute prescription:
```bash
curl -s localhost:8080/read/slice_geometry
curl -s -X POST localhost:8080/write/slice_target \
  -d '{"position":[12.5,-3,40],"read_dir":[1,0,0],"phase_dir":[0,1,0],"slice_dir":[0,0,1]}'
```
> You'll see: the geometry the mock sent (slice 2 at [10.5, −20, 30]); then
> `{"delivered":true,...}` — and Terminal 1 prints
> `TEXT_RECEIVED:{"type":"slice_target","position":[12.5,-3.0,40.0],...}` and
> exits. That line is the proof: the command went UI-side HTTP → marshal →
> scanner connection. Without Terminal 1 running, the same POST returns
> `"delivered":false` — command cached, no scanner to receive it.

The relative command works the same way (rerun Terminal 1 first):
```bash
curl -s -X POST localhost:8080/write/slice_delta \
  -d '{"translation_mm":[1.5,0,-2],"rotation_rad":[0,0.1,0]}'
```
> You'll see: `{"delivered":true,...}` and Terminal 1 prints
> `TEXT_RECEIVED:{"type":"slice_delta",...}` — or `"delivered":false`
> without Terminal 1.

Knobs for 5b–5d: `--fps N` (pace) · `--frames N` (how many; 0 = until Ctrl-C)
· `--matrix N` (image size) · `--slices N` (multislice) ·
`--mode kspace|image` (through recon | straight to viewer).

---

## Step 6 — Check the results

Live mode (ports = yours if remapped):
```bash
# viewer at :3000 shows the moving phantom
curl localhost:8080/status                      # last_image_age_s small and resetting
ls session-data/live/from_reconstruction/       # scan_*.h5 appears after each scan ends
```

Dump mode:
```bash
ls session-data/dump/from_scanner/              # full-stream archives incl. k-space
ls session-data/dump/from_reconstruction/       # recon output archives
```

A client on **another machine** (no shared folder needed):
```bash
curl http://<marshal-host>:8080/image/latest.h5 -o latest.h5    # the actual image bytes
```

---

## Step 7 — Stop

```bash
docker compose --profile test-recon down
```

Always safe to include the profile: with it, the command tears down
everything in every configuration. Without it, a running test-recon container
would be left behind holding the network. Data in `session-data/` survives —
`down` only removes containers.

---

## Notes

- Everything durable lands in `session-data/` (`SESSION_DATA_DIR` moves it).
  The viewer's current-frame snapshot is internal plumbing — not user data.
- Auto-routing: k-space → recon; scanner-sent images → straight to the UI.
- ISMRMRD headers 340/198/40 must match scanner/marshal/recon.
- Full test matrix, performance checks, troubleshooting: [TESTING.md](TESTING.md).
- Every endpoint and flag: [API_REFERENCE.md](API_REFERENCE.md).
