# Quick Start

One compose file. One knob (`MARSHAL_DUMP`). Marshal auto-routes k-space→recon, images→UI.

## Build once
```bash
./scripts/build-client-images.sh
docker build -f .worktrees/mri_data_marshal/third_party/python-ismrmrd-server/docker/Dockerfile \
  -t fire-python:latest .worktrees/mri_data_marshal/third_party/python-ismrmrd-server/docker
```

## Run
```bash
docker compose up -d          # live
docker compose ps             # wait until healthy
```
UI: http://localhost:3000 · API: `:8080` (`/health`, `/image/latest`) · scanner MRD TCP: `:9100`

## Feed test data (no real scanner)
```bash
docker run --rm -v "$PWD/session-data:/data" fire-python:latest \
  python3 generate_cartesian_shepp_logan_dataset.py -o /data/phantom.h5

docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest \
  python3 client.py -c invertcontrast -o /data/out.h5 --address mri-marshal --port 9100 /data/phantom.h5
```

## Dump mode (archive instead of live view)
```bash
MARSHAL_DUMP=--dump docker compose up -d                          # whole stack
MARSHAL_DUMP=--dump docker compose up -d --force-recreate mri-marshal   # switch marshal only
```
`/image/latest` → 404, UI blank (expected). Archives to `session-data/dump/...`.

## Real scanner + recon (production)
```bash
RECON_HOST=<recon-ip> RECON_PORT=<port> docker compose up -d --scale recon=0
```
Point the scanner at `<marshal-host>:9100`. Drop `fire-python`. ISMRMRD header sizes (340/198/40) must match across scanner, marshal, recon.

## Teardown
```bash
docker compose logs -f mri-marshal
docker compose down
```
