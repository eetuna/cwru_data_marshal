# Quick Start

## Build (once)
```bash
./scripts/build-client-images.sh
```

## Start
```bash
# LIVE
RECON_HOST=<ip> RECON_PORT=<port> docker compose up -d                      # real recon
docker compose --profile test-recon up -d                                  # test recon

# DUMP (archival)
MARSHAL_DUMP=--dump RECON_HOST=<ip> RECON_PORT=<port> docker compose up -d  # real recon
MARSHAL_DUMP=--dump docker compose --profile test-recon up -d              # test recon

docker compose ps                                                          # wait healthy
```
UI `:3000` · API `:8080` · scanner MRD TCP `:9100`

## Recon: bundled vs real
Marshal always talks to a recon at `RECON_HOST:RECON_PORT` (default `recon:9002`). Two ways to supply it — **pick one, not both**:

| | Enable it | RECON_HOST |
|---|---|---|
| **Bundled test recon** | `--profile test-recon` (starts the `recon` container at `recon:9002`) | leave default — don't set it |
| **Real recon** | omit the profile (bundled recon stays off) | set `RECON_HOST=<ip> RECON_PORT=<port>` |

Setting `RECON_HOST` *and* `--profile test-recon` runs an idle bundled recon that marshal ignores. Confirm which is on:
```bash
docker compose config --services | grep recon   # nothing = bundled recon off
docker compose ps | grep recon                  # cwru-recon = bundled recon running
```

## Send data
```bash
# real scanner: point it at <marshal-host>:9100
# test data instead:
docker run --rm -v "$PWD/session-data:/data" fire-python:latest \
  python3 generate_cartesian_shepp_logan_dataset.py -o /data/phantom.h5
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest \
  python3 client.py -c invertcontrast -o /data/out.h5 --address mri-marshal --port 9100 /data/phantom.h5
```

## Stop
```bash
docker compose down
```

## Notes
- Knobs: `MARSHAL_DUMP` (live | `--dump`), `RECON_HOST`/`RECON_PORT`, `SESSION_DATA_DIR`, exposed ports `HTTP_PORT`/`MRD_PORT`/`ROBOT_PORT`/`UI_PORT`/`WRITE_PORT`.
- Auto-routing: k-space → recon, images → UI.
- Dump: `/image/latest` = 404, UI blank (archives to `session-data/dump/`).
- ISMRMRD headers 340/198/40 must match scanner/marshal/recon.
