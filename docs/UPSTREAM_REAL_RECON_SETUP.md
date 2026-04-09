# Running the Real-Reconstruction Demo from Upstream

This is a self-contained recipe for someone who has just cloned the upstream
repository (`cwru-mercis/cwru_data_marshal`) and wants to run the full
real-reconstruction pipeline end-to-end.

There is no merge into `main`; the work lives on two feature branches that
must be checked out together:

- `feature/umbrella-real-recon` — on the umbrella repo (the top-level
  clone). Contains the updated `docker/Dockerfile.mock-recon`, the pinned
  `MRI_BRANCH` in `scripts/build-client-images.sh`, the bumped
  `KSPACE_INTERVAL` in `.env.demo`, and this document.
- `feature/kspace-streamer-real-recon` — on the MRI data marshal repo
  (fetched into the worktree). Contains the vendored
  `python-ismrmrd-server`, the HTTP→TCP shim, the kissfft-backed C++
  `kspace_streamer` that emits a real Shepp-Logan phantom, and the
  `DELETE /v1/mrd/latest` endpoint.

## 1. Clone and check out the umbrella branch

```bash
git clone https://github.com/cwru-mercis/cwru_data_marshal.git
cd cwru_data_marshal
git checkout feature/umbrella-real-recon
```

The MRI branch does not need to be checked out manually — the build
script below will fetch it and materialize it as a worktree at
`.worktrees/mri_data_marshal` automatically.

## 2. Build the docker images

```bash
./scripts/build-client-images.sh
```

This:

- Fetches `feature/kspace-streamer-real-recon` from upstream (already
  pinned as `MRI_BRANCH` in the script) and creates a worktree at
  `.worktrees/mri_data_marshal`.
- Builds all nine `cwru/*:latest` images, including the rewritten
  `cwru/mock-recon` (now backed by `python-ismrmrd-server` + the HTTP
  shim instead of the old flask gradient stub) and the new
  `cwru/kspace-streamer` (kissfft-backed Shepp-Logan producer).

First-time build takes 15-25 minutes (most of that is compiling
`ismrmrd` from source inside multiple C++ images). Subsequent builds
hit docker's layer cache and are much faster.

## 3. Set up the shell alias (one time)

Paste this once and add it to `~/.bashrc` so every new terminal has it:

```bash
echo "alias cdd='docker compose --env-file .env.demo -f docker-compose.demo.yml'" >> ~/.bashrc
source ~/.bashrc
```

All commands below must be run **from the repo root** (`cd` into it
first) because `cdd` resolves `.env.demo` and `docker-compose.demo.yml`
relative to the current directory.

## 4. Run the real-reconstruction demo

One terminal per service. Each blocks and streams its logs until
`Ctrl-C`. Run them in this order:

```bash
# Terminal 1
cdd up mri-marshal

# Terminal 2
cdd up robot-marshal

# Terminal 3
cdd up mock-recon

# Terminal 4
cdd --profile viz up viz-client

# Terminal 5 (optional)
cdd up ecg-client

# Terminal 6 (optional)
cdd up pose-client

# Terminal 7 (optional)
cdd --profile robot-clients up robot-clients

# Terminal 8
cdd up kspace-streamer
```

What happens:

1. `mri-marshal` starts on `http://localhost:8080`. The
   `RECON_ENDPOINT=http://mock-recon:9002` env var (set in `.env.demo`)
   makes it forward raw k-space to the recon service.
2. `mock-recon` starts `python-ismrmrd-server` internally on port 9004
   and the HTTP shim on port 9002. The container is healthy when its
   healthcheck reports `ok`.
3. `viz-client` opens an OpenCV window that polls
   `GET /v1/mrd/latest` and shows whatever reconstructed frame is most
   recent.
4. `kspace-streamer` starts producing a rotating Shepp-Logan phantom
   volume (5 slices, 128×128 each, gaussian noise per line) and POSTs
   it to marshal every 500 ms (set by `KSPACE_INTERVAL=500` in
   `.env.demo`). Marshal forwards to the shim, the shim speaks ISMRMRD
   TCP to `python-ismrmrd-server`, `simplefft` runs the 2D inverse
   FFT, and the reconstructed images come back to marshal via
   `POST /v1/mrd/callback`. viz-client then displays them.

## 5. Stop everything

Always include both profile flags so the profiled services
(`viz-client`, `robot-clients`) are torn down alongside the unprofiled
ones. If you leave them out, profiled containers become orphaned and
the next `up` can trip over stale network references.

```bash
cdd --profile viz --profile robot-clients down
```

## 6. Clear the sticky latest-frame cache (between sessions)

`mri-marshal` caches the most recent stored frame in memory so
`GET /v1/mrd/latest` can serve it instantly. Without explicit cleanup,
that cache sticks around for the life of the marshal process and
`viz-client` keeps showing the last frame of a previous run.

To clear it explicitly:

```bash
curl -X DELETE http://localhost:8080/v1/mrd/latest
```

Marshal returns `204 No Content`, the next `GET /v1/mrd/latest`
returns `204`, and `viz-client` (still running) falls into its
"Waiting for data..." branch until the next real frame arrives.

The endpoint is purely additive. If nothing ever calls it, marshal
behaves exactly as before.

## 7. Swapping in a real scanner-side recon

The `cwru/mock-recon` image is designed as a drop-in placeholder. To
use a real reconstruction service instead:

1. Stop the `mock-recon` container.
2. Start `mri-marshal` with
   `RECON_ENDPOINT=http://<real-recon-host>:<port>` pointing at the
   real service. Edit `.env.demo` or pass it as a shell var before
   `cdd up mri-marshal`.

Nothing else in this guide changes. Marshal's HTTP contract with the
recon service (`POST /reconstruct` out, `POST /v1/mrd/callback` in) is
identical regardless of what's behind the endpoint.

## What lives on each branch (reference)

### `feature/umbrella-real-recon` (this branch)

- `scripts/build-client-images.sh` — `MRI_BRANCH` pinned to
  `feature/kspace-streamer-real-recon`.
- `docker/Dockerfile.mock-recon` — rewritten to build
  `python-ismrmrd-server` + the shim into `cwru/mock-recon:latest`.
  Still listens on port 9002 so nothing else in compose needs to
  change.
- `.env.demo` — `KSPACE_INTERVAL=500` (paces the C++ streamer to match
  recon throughput so callbacks stay in order).
- `docs/QUICK_START.md` — short flow-based run recipes.
- `docs/MANUAL_TERMINAL_SETUP.md` — long per-terminal reference, with
  new Flow A / Flow B guidance and an explicit `--env-file .env.demo`
  warning.
- `docs/UPSTREAM_REAL_RECON_SETUP.md` — this document.

### `feature/kspace-streamer-real-recon` (MRI branch)

Checked out automatically into
`.worktrees/mri_data_marshal/` by the build script.

- `third_party/python-ismrmrd-server/` — vendored upstream
  reconstruction server.
- `third_party/kissfft/` — vendored BSD-licensed FFT library, linked
  only into the `kspace_streamer` binary (marshal and every other
  target stay vanilla).
- `docker/recon-shim/` — the HTTP→TCP shim: `shim.py`, `Dockerfile`,
  `entrypoint.sh`. Listens on 9002, forwards to
  `python-ismrmrd-server` on 9004 inside the same container.
- `clients/kspace_streamer/phantom.hpp` — Shepp-Logan phantom helpers
  and the kissfft-backed `image_to_kspace` used by the C++ producer.
- `clients/kspace_streamer/kspace_streamer_main.cpp` — the C++
  streamer, now emitting 5-slice animated Shepp-Logan k-space with
  per-line gaussian noise.
- `src/marshal_http.hpp` — adds the `DELETE /v1/mrd/latest` handler
  (no other marshal changes).
- `docs/VIZ_CLIENT_STALE_FRAME_ISSUE.md` — background on why the
  stale-frame behavior exists and why the options other than the
  DELETE endpoint were rejected.

## Rules

- Always pass `--env-file .env.demo` (use `cdd`). Without it, marshal
  starts without `--recon-endpoint` and every k-space POST returns
  `501 Not Implemented`.
- Do not mix `image-streamer` with `mock-recon`/`kspace-streamer`.
  They produce different data types on the same stream and will
  fight. `image-streamer` is for the bypass flow only.
- `mock-recon` must be up before `kspace-streamer`, otherwise marshal
  returns `501` until it appears.
- The `viz` and `robot-clients` profiles require explicit
  `--profile <name>` flags on both `up` and `down`.
