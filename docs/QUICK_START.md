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

> **Getting code updates:** just rerun this script. It fetches the two code
> branches from GitHub, builds each from a clean export at its commit (never
> from a working folder), and prints `Building from: <branch> @ <commit>` —
> check that line if a feature seems missing. `git pull` alone does NOT
> update the code: `main` carries only docs/compose; the code lives on the
> `mri-data-marshal` and robot branches, which the script fetches itself.
> Developers testing uncommitted changes: `MRI_BUILD_DIR=<checkout>` or
> `ROBOT_BUILD_DIR=<checkout>` in front of the command builds that folder
> as-is (the script says so loudly). After rebuilding, restart the stack with
> `docker compose up -d --force-recreate` — a plain `up -d` keeps the old
> containers running on the old images. If the script cannot reach the
> cwru-mercis GitHub repo it stops with an error (it never silently builds
> old local code). The built marshal prints its `<branch> @ <commit>` version
> in its log and in `/status`.

---

## Step 2 — Start the stack (pick ONE command)

**First, once per machine:** settings live in `.env` next to
`docker-compose.yml`. Copy the template and set `RECON_HOST` (required —
there is no default; without it `docker compose` stops with
`RECON_HOST is not set - copy .env.example to .env and set it`):
```bash
cp .env.example .env     # then edit: RECON_HOST=recon (test recon) or the real recon's IP
```

Every configuration has four settings. Every command below states
what you get for **all four**, so nothing is implicit:

> **recon** (test or real) · **data folder** · **snapshot** (RAM or disk) · **ports**

There is one mode: every scan is always archived in full (k-space included,
under `session-data/dump/`) *and* the viewer updates live during the scan.

**2a. Demo / testing** (the usual choice; `.env` has `RECON_HOST=recon`):
```bash
docker compose --profile test-recon up -d
```
> You get: recon = **bundled test recon** · data → **./session-data** · snapshot =
> **RAM** · ports = **3000 UI / 8080 API / 9100 scanner / 8081 robot**

**2b. Production — real recon** (`.env` has `RECON_HOST=10.0.0.5`, `RECON_PORT=9002`):
```bash
docker compose up -d
```
> You get: recon = **yours at 10.0.0.5:9002** · data →
> **./session-data** · snapshot = **RAM** · ports = **defaults as above**

**2c/2d. Recording session:** no separate command any more — 2a and 2b
already record everything the scanner and recon send.

**2e. MRI-only — no robot** (scanner → marshal → recon → viewer; the robot
marshal and the 6 robot data clients are not started at all):
```bash
docker compose --profile test-recon up -d --no-deps mri-marshal recon webgl-client
```
> You get: recon = **bundled test recon** · data →
> **./session-data** · snapshot = **RAM** · ports = **3000 UI / 8080 API /
> 9100 scanner** (no robot, so nothing on 8081). The viewer's robot panel
> stays empty and webgl logs harmless robot-connection errors — expected.
> Measured 2026-07-06: this subset sustains the full 20 fps kspace stream.

For MRI-only with a **real** recon: same idea, drop the profile and the recon
service, with the recon address in `.env`:
```bash
docker compose up -d --no-deps mri-marshal webgl-client
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
> You get: test recon · data → **/data/experiment1** · snapshot = RAM · default ports

**Snapshot on disk instead of RAM** (you normally never need this — the
snapshot is the viewer's throwaway current-frame file; RAM just makes it
stall-proof; your scan data is on disk either way):
```bash
MARSHAL_LATEST= docker compose --profile test-recon up -d
```
> You get: test recon · data → ./session-data · snapshot = **disk**
> (`latest_image.h5` under `session-data/`) · default ports

**Different ports:**
```bash
UI_PORT=13000 HTTP_PORT=18080 MRD_PORT=19100 docker compose --profile test-recon up -d
```
> You get: test recon · data → ./session-data · snapshot = RAM ·
> ports = **13000 UI / 18080 API / 19100 scanner / 8081 robot**

**Several at once** (they always stack):
```bash
UI_PORT=13000 SESSION_DATA_DIR=/data/exp1 \
RECON_HOST=10.0.0.5 RECON_PORT=9002 docker compose up -d
```
> You get: recon = **yours** · data → **/data/exp1** ·
> snapshot = RAM · ports = **13000 UI**, others default

(Values typed in front of the command override `.env` for that run; put
anything you want to keep in `.env`.)

Rule: bundled recon = `RECON_HOST=recon` plus `--profile test-recon`.
Real recon = `RECON_HOST=<its IP>`, drop the profile. Never both.

> **`RECON_HOST` must not be empty.** The stock compose file stops with an
> error if it is unset or empty. A hand-edited compose file, `docker run -e
> RECON_HOST=`, or a direct `marshal --recon-host` with a missing value makes
> the flag swallow the next flag as its hostname — the marshal detects this and
> refuses to start with a clear error naming the cause; `--recon-host` is
> required whenever `--mrd-port` is used. (Setting `MARSHAL_LATEST=` empty is
> fine — there the flag and value travel together inside the variable.) At
> startup the marshal tries the recon address once and logs `Recon <host>:<port>
> is reachable` or `is NOT reachable: <reason> — scans will fail until it is`.
> There is no recon-less mode: with no reachable recon the marshal keeps
> running, archives the scan, and sends the scanner a `RECON FAILED` image and
> a CLOSE so the scan still finishes.

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
{"version":"<branch> @ <commit>", "status":"waiting for scan", "scanner_connected":false,
 "recon":{"configured":true,"connected":false,"startup_check":"reachable"},
 "scan":{"active":false}, "disk_free_gb":41.3, ...}
```
`scanner_connected` and `recon.connected` are `false` until a scan starts —
that's normal. (`recon.connected` turns `true` when a scan connects.) If
`recon.startup_check` is not `"reachable"`, it says why — fix that before
scanning.
`/status` is the first thing to check whenever anything seems wrong.

---

## Step 4 — Open the viewer

Open **http://localhost:3000** in a browser (your `UI_PORT` if you changed
it) — *before* sending data, so you don't miss the stream. It shows the robot
scene immediately; the MRI panel stays empty until the first image arrives in
step 5. Opened from another machine (`http://<marshal-host>:3000`) it works
the same. Until an image arrives it shows the marshal's status in words, e.g.
`No image yet — ✗ RECON FAILED (cannot connect to recon at …)`.

---

## Step 5 — Send data (pick ONE command)

**5a. Real scanner.** Point it at `<marshal-host>:9100` over raw MRD TCP
(the host's IP on your network; port = your `MRD_PORT` if you changed it).
Nothing else to run — the marshal forwards everything the scanner sends to the
recon and pushes the recon's images back to the scanner and to the viewer
automatically. One scan per scanner connection; a second connection waits its
turn and is served when the current scan ends.

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

**5d. Moving test phantom as ready-made images** — the scanner side sends
images instead of k-space; the marshal forwards them to the recon like
everything else and the viewer shows what the recon sends back:
```bash
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/fire_stream.py --address mri-marshal --port 9100 \
  --mode image --fps 15 --frames 0 --matrix 128 --slices 1
```
> You'll see: the orbit, paced by what the recon returns.

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
`--mode kspace|image` (send k-space | send images; both go through the recon).

---

**5g. Three orthogonal planes — the 3-D multiplane check.** The 3-D panel
keeps the **last 3 orientations, across scans** (a same-orientation stack
keeps all of its slices): run three short scans, one per orientation, and
all three stay visible together.
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
> stack (`--slices 5`, one orientation) shows all five slices as parallel
> planes instead. Full pass/fail matrix for the August-2026 scanner-test symptoms:
> [TESTING.md](TESTING.md#regression-checks--2026-08-18-scanner-test-notes).

## Step 6 — Check the results

Ports = yours if remapped:
```bash
# viewer at :3000 shows the moving phantom
curl localhost:8080/status                      # "status":"scanning" during a scan, "scan finished" after
ls session-data/dump/from_scanner/              # full-stream archives incl. k-space
ls session-data/dump/from_reconstruction/       # recon output archives
ls session-data/marshal.log                     # the marshal's log, kept on disk
```
Each scan gets one `scan_<timestamp>.h5` per side. The HDF5 file is written in
the background just after the scan ends, so the next scan never waits for it.

A client on **another machine** (no shared folder needed):
```bash
curl http://<marshal-host>:8080/image/latest.h5 -o latest.h5    # the actual image bytes
```

---

## Record & replay

Record an experiment, then replay it later as if a scanner were sending it —
the stack cannot tell the difference.

**Record** — nothing to switch on: run the stack (Step 2) and scan; everything the
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
- Routing: everything the scanner sends → recon; recon's replies → scanner + viewer.
- ISMRMRD headers 340/198/40 must match scanner/marshal/recon.
- Recon looks dead (no recon images; viewer says RECON FAILED)?
  `docker logs cwru-mri-marshal | grep -i recon` — a
  `Failed to connect to recon at <host>:<port>` line means `RECON_HOST` points
  at nothing reachable (the 2026-08-18 scanner-test failure mode). Check
  `.env`, and that the recon machine's port is open. A slow recon is simply
  waited for — after the scanner's CLOSE the marshal waits with no time limit
  for the recon's CLOSE; a dead recon host is detected by TCP keepalive.
- Full test matrix, performance checks, troubleshooting: [TESTING.md](TESTING.md).
- Every endpoint and flag: [API_REFERENCE.md](API_REFERENCE.md).
