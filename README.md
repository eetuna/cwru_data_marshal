# CWRU Data Marshal

Data plane for MRI-guided robotic catheter work: an **MRI marshal** that moves
scanner/reconstruction MRD data and a **robot marshal** that shares robot state,
with a WebGL viewer on top.

**New here? Start with [docs/QUICK_START.md](docs/QUICK_START.md).**

## Run it

```bash
RECON_HOST=<ip> RECON_PORT=<port> docker compose up -d   # real recon (production)
docker compose --profile test-recon up -d                # bundled test recon
docker compose ps                                        # wait until healthy
```

Add `MARSHAL_DUMP=--dump` in front for dump/archival mode. Then open the viewer at
**http://localhost:3000**. Drive data in with a real scanner or `python-ismrmrd-server`
`client.py` against `mri-marshal:9100` (see [QUICK_START](docs/QUICK_START.md)).

Knobs (env on `docker compose up`): `MARSHAL_DUMP` (unset = live, `--dump` = dump), `MARSHAL_LATEST`
(RAM snapshot — default on at `/dev/shm/cwru-latest`; set empty for on-disk),
`RECON_HOST`/`RECON_PORT` (recon target), `SESSION_DATA_DIR` (data path),
`HTTP_PORT`/`MRD_PORT`/`ROBOT_PORT`/`UI_PORT`/`WRITE_PORT`
(exposed ports). The marshal routes automatically: k-space → recon; scanner-sent images →
straight to the viewer (no per-scan choice). UI slice commands (translate/rotate/set
position+orientation) are relayed to the scanner over the live MRD connection — see
[SLICE_CONTROL_HANDOFF.md](docs/SLICE_CONTROL_HANDOFF.md).

## What runs

| Service | Port | Role |
|---|---|---|
| mri-marshal | 8080 (HTTP), 9100 (MRD TCP) | moves scanner↔recon MRD data; serves `/image/latest` |
| recon | 9002 (MRD TCP) | reconstruction (python-ismrmrd-server, invertcontrast) |
| robot-marshal | 8081 (HTTP) | robot-state read/write channels |
| robot clients | — | catheter-tracking, force-sensor, controller, planning, front-end, surface-tracking |
| webgl-client | 3000 (UI), 3001 | browser viewer |

```
scanner ──MRD TCP :9100──> mri-marshal ──MRD TCP :9002──> recon
                              │  <── IMAGE(1022) back ─────┘
                              ├─ publishes /image/latest ──> webgl-client (:3000)
                              └─ archives per-scan H5 under session-data/
robot clients <──HTTP :8081──> robot-marshal ──> webgl-client
```

## Documentation

- **[QUICK_START.md](docs/QUICK_START.md)** — run it, feed data, live vs dump.
- **[TESTING.md](docs/TESTING.md)** — verify live/dump × k-space/image (or `./scripts/test-modes.sh`).
- **[ARCHITECTURE.md](docs/ARCHITECTURE.md)** — how it works: wire protocol, routing, storage, fault tolerance.
- **[API_REFERENCE.md](docs/API_REFERENCE.md)** — HTTP endpoints + MRD TCP messages.
- **[RECONSTRUCTION_INTERFACE.md](docs/RECONSTRUCTION_INTERFACE.md)** — the recon contract (swap in your own MRD-TCP recon).
- **[EXTERNAL_CLIENT_GUIDE.md](docs/EXTERNAL_CLIENT_GUIDE.md)** — connect your own scanner/recon/data client.

## Build & package

```bash
./scripts/build-client-images.sh                 # build the cwru/* images
./scripts/export_usb.sh /path/to/usb             # docker save + compose file for offline transfer
```

Source lives in git worktrees under `.worktrees/` (built into the images):
`mri_data_marshal` (MRI marshal + recon) and `robot_data_marshal` (robot marshal,
clients, webgl).
