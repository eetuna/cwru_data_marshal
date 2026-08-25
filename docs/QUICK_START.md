# Quick Start

Follow the steps in order. Every option at every step is a complete
copy-paste command — pick the one that matches what you want.

**Run every command from the repo folder** (the one containing
`docker-compose.yml`) — several commands mount `./scripts` and
`./session-data` from the current directory.

---

## Step 1 — Build / rebuild the images

```bash
./scripts/build-client-images.sh
```

Builds the 5 images the stack runs: `cwru/mri-marshal`, `fire-python`
(test recon + test clients), `cwru/robot-marshal`, `cwru/robot-clients`,
`cwru/webgl-client`. First build takes ~10–20 min (downloads + compiles);
rebuilds are fast. Success ends with `Build complete! All 5 images ready.`

> **Getting code updates:** rerun this script — it fast-forwards its build
> worktrees from GitHub and prints the exact commit each image is built from
> (check that line if a feature seems missing). Note that `git pull` alone
> does NOT update the code: `main` carries only docs/compose, the code lives
> on the `mri-data-marshal` and robot branches, which this script refreshes
> for you. After rebuilding, restart the stack with
> `docker compose up -d --force-recreate` — a plain `up -d` keeps the old
> containers running on the old images.

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

> **Never pass `RECON_HOST` through as an empty value.** The stock compose file
> guards against it (`${RECON_HOST:-recon}` substitutes the default even for a
> set-but-empty variable), but a hand-edited compose file, `docker run -e
> RECON_HOST=`, or a direct `marshal --recon-host` with a missing value makes
> the flag swallow the next flag as its hostname — the marshal detects this and
> refuses to start with a clear error naming the cause. Leave the variable
> unset to get the default. (Setting `MARSHAL_LATEST=` empty is fine — there
> the flag and value travel together inside the variable.) There is no
> recon-less mode: with no reachable recon the marshal keeps working but logs
> `Failed to connect to recon` per scan and pushes a failure image.

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

**5f. Slice-command check** — verifies the UI→marshal→`slice_agent` control
channel with a mock agent (no scanner needed). The channel is off unless the
marshal is started with `SLICE_AGENT_HOST`:

```bash
docker run -d --rm --name slice-agent-mock --network cwru-demo-net fire-python:latest \
  python3 -c "$(cat scripts/slice_agent_mock.py)" --port 9270      # inline: works from a devcontainer too
SLICE_AGENT_HOST=slice-agent-mock docker compose up -d --force-recreate mri-marshal
curl -s -X POST localhost:8080/write/file_slice_translation -d '{"client_id":"t","values":[1]}'
docker logs slice-agent-mock
```
> You'll see: `{"delivered":true,"enabled":true,...,"state":{...,"tz":1.0}}`
> and the mock prints `CMD {"frame": 0, "tx": 0.0, "ty": 0.0, "tz": 1.0, ...}` — the
> 56-byte packet Andrew's real `slice_agent` receives, identical to a PgUp in
> his `slice_control` tool. A rotation slider adds degrees to `rx/ry/rz` the
> same way (W/S, A/D, Q/E). Without a reachable agent the POST returns
> `"delivered":false` (state kept, re-sent when the agent appears); with
> `SLICE_AGENT_HOST` unset it returns `"enabled":false`.

**On the real scanner** (instead of the mock):
1. On the MARS: `./slice_agent --listen` (Andrew's program, same as with his keyboard tool).
2. `SLICE_AGENT_HOST=<MARS ip> docker compose up -d`
   (MARS ip: after any scan, `docker logs cwru-mri-marshal | grep "Scanner connected from"`).
3. In the sequence, turn on the WIP toggle **"Dynamic Slice Control"**.

Then the `+`/`−` buttons and rotation sliders in the browser move the slice.

Knobs for 5b–5d: `--fps N` (pace) · `--frames N` (how many; 0 = until Ctrl-C)
· `--matrix N` (image size) · `--slices N` (multislice) ·
`--mode kspace|image` (through recon | straight to viewer).

---

**5g. Three orthogonal planes — the 3-D multiplane check.** The 3-D panel
keeps the **last 3 distinct slice planes, across scans**: run three short
scans, one per orientation, and all three stay visible together.
```bash
for o in tra sag cor; do
  docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
    python3 /scripts/fire_stream.py --address mri-marshal --port 9100 \
    --mode kspace --fps 5 --frames 10 --matrix 96 --orient $o
done
```
> You'll see: after the first scan one plane; after the second, **two** (the
> first one stays); after the third, one transverse + one sagittal + one
> coronal plane crossing in the 3-D panel, and the three 2-D panels showing
> one of each. Now run the loop body once more with `--orient sag`: only the
> sagittal plane refreshes — transverse and coronal stay put. A parallel
> stack (`--slices 3`, one orientation) shows all three parallel slices
> instead. Full pass/fail matrix for the August-2026 scanner-test symptoms:
> [TESTING.md](TESTING.md#regression-checks--2026-08-18-scanner-test-notes).

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

## Record & replay

Record an experiment, then replay it later as if a scanner were sending it —
the stack cannot tell the difference.

**Record** — run the stack in dump mode (Step 2c/2d) and scan; everything the
scanner sends (k-space included) lands in `session-data/dump/from_scanner/scan_<ts>.h5`.

**Replay at the recorded pace** (default; uses the recording's timestamps —
if the recording has none, it paces at `--fallback-fps`, default 10):
```bash
docker run --rm --network cwru-demo-net \
  -v "$PWD/session-data:/data" -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/replay_scan.py /data/dump/from_scanner/scan_<ts>.h5 --preload
```
> You get: the recorded scan plays back through marshal → recon → viewer at
> its original rate. `--preload` reads the file into RAM first (slow HDF5
> read paid up front) so the pacing is exact.

**Replay as fast as possible** (correctness check, not realistic timing):
```bash
docker run --rm --network cwru-demo-net \
  -v "$PWD/session-data:/data" -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/replay_scan.py /data/dump/from_scanner/scan_<ts>.h5 --full-speed --preload
```

Force a specific rate instead: `--fps N`. Details and limits: [TESTING.md](TESTING.md).

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
- Recon looks dead (no recon images; viewer shows only scanner-sent data)?
  `docker logs cwru-mri-marshal | grep -i recon` — a
  `Failed to connect to recon at <host>:<port>` line means `RECON_HOST` points
  at nothing reachable (the 2026-08-18 scanner-test failure mode). Check the
  env you launched compose with, and that the recon machine's port is open.
- Full test matrix, performance checks, troubleshooting: [TESTING.md](TESTING.md).
- Every endpoint and flag: [API_REFERENCE.md](API_REFERENCE.md).
