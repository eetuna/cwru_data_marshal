# Quick Start

The MRI data marshal: a Docker Compose stack that ingests scanner data over MRD TCP, reconstructs k-space, and publishes images to a WebGL UI (plus a robot-marshal side for the robot clients). One compose file, one knob.

## Prerequisites

Build the images once:

```bash
./scripts/build-client-images.sh   # cwru/* images

# fire-python (recon + synthetic-data tools)
docker build \
  -f .worktrees/mri_data_marshal/third_party/python-ismrmrd-server/docker/Dockerfile \
  -t fire-python:latest \
  .worktrees/mri_data_marshal/third_party/python-ismrmrd-server/docker
```

## Run (live)

```bash
docker compose up -d
```

Services: `mri-marshal`, `recon`, `robot-marshal`, six robot clients (catheter-tracking, force-sensor, controller, planning, front-end, surface-tracking), and `webgl-client`.

## Verify

```bash
curl localhost:8080/health
curl localhost:8080/image/latest   # {path,error} in live mode
```

Open the UI at http://localhost:3000 (write-back on 3001).

Ports: marshal HTTP `8080` (`/image/latest`, `/health`), marshal MRD TCP `9100` (scanner), recon `9002`, robot-marshal `8081`.

## Feed data

The scanner is external: a real scanner or a `python-ismrmrd-server` client connecting to `mri-marshal:9100` over raw MRD TCP. Marshal auto-routes by message type — k-space (ACQUISITION) goes to recon and the image comes back; a scanner-sent IMAGE is published straight to the UI. Nothing to configure per scan.

To feed synthetic data with no real scanner:

```bash
# generate a phantom
docker run --rm -v "$PWD/session-data:/data" fire-python:latest \
  python3 generate_cartesian_shepp_logan_dataset.py -o /data/phantom.h5

# stream it into the marshal
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest \
  python3 client.py -c invertcontrast -o /data/out.h5 \
  --address mri-marshal --port 9100 /data/phantom.h5
```

Output lands in `session-data/` (live mode writes `live/from_reconstruction/latest_image.h5` and `from_scanner`).

## Dump mode

Set `MARSHAL_DUMP=--dump` to archive to disk instead of publishing live. `GET /image/latest` returns 404 and the UI stays blank — expected.

```bash
# whole stack in dump mode
MARSHAL_DUMP=--dump docker compose up -d

# or switch just the marshal, without bouncing the stack
MARSHAL_DUMP=--dump docker compose up -d --force-recreate mri-marshal

# back to live (omit the var)
docker compose up -d --force-recreate mri-marshal
```

## Manage / teardown

```bash
docker compose ps
docker compose logs -f mri-marshal
docker compose down
```
