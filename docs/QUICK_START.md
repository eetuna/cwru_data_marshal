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
### Ports

| What | Default | Change with |
|---|---|---|
| WebGL UI | 3000 | `UI_PORT` |
| HTTP API | 8080 | `HTTP_PORT` |
| Scanner MRD TCP | 9100 | `MRD_PORT` |
| Robot marshal | 8081 | `ROBOT_PORT` |
| WebGL write-back | 3001 | `WRITE_PORT` |

All knobs are just env vars in front of the command and stack together, e.g.:
```bash
# remap two ports, test recon, live
HTTP_PORT=18080 MRD_PORT=19100 docker compose --profile test-recon up -d

# remap UI port, real recon, dump mode
UI_PORT=13000 MARSHAL_DUMP=--dump RECON_HOST=10.0.0.5 RECON_PORT=9002 docker compose up -d
```

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
Open the viewer first: **http://localhost:3000** (updates live in live mode).
```bash
# real scanner: point it at <marshal-host>:9100

# one-shot test push:
docker run --rm -v "$PWD/session-data:/data" fire-python:latest \
  python3 generate_cartesian_shepp_logan_dataset.py -o /data/phantom.h5
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest \
  python3 client.py -c invertcontrast -o /data/out.h5 --address mri-marshal --port 9100 /data/phantom.h5

# continuous stream (moving phantom; --mode image bypasses recon; --slices N for multislice):
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/fire_stream.py --address mri-marshal --port 9100 \
  --mode kspace --fps 10 --frames 0 --matrix 128 --slices 5      # Ctrl-C to stop
```
Full test matrix and troubleshooting: [TESTING.md](TESTING.md).

## Stop
```bash
docker compose down
```

## Notes
- Knobs: `MARSHAL_DUMP` (live | `--dump`), `RECON_HOST`/`RECON_PORT`, `SESSION_DATA_DIR`, exposed ports `HTTP_PORT`/`MRD_PORT`/`ROBOT_PORT`/`UI_PORT`/`WRITE_PORT`.
- Auto-routing: k-space → recon, images → UI.
- Dump: `/image/latest` = 404, UI blank (archives to `session-data/dump/`).
- ISMRMRD headers 340/198/40 must match scanner/marshal/recon.
