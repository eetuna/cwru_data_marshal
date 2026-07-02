# Testing — live/dump × k-space/image

One command runs the whole matrix:

```bash
./scripts/test-modes.sh
```

Or step by step (WSL, repo root). Images must be built once (`./scripts/build-client-images.sh`).

```bash
# start (live + bundled test recon)
docker compose --profile test-recon up -d
docker compose ps                          # wait healthy
```

**Open the browser now → http://localhost:3000** (on WSL use the Windows browser). Open it
*before* pushing data — it's blank until the first push, then renders and updates live, so
you don't miss a stream. (For a single push the last image is retained anyway; blank in dump.)

```bash
# phantom (k-space)
docker run --rm -v "$PWD/session-data:/data" fire-python:latest \
  python3 generate_cartesian_shepp_logan_dataset.py -o /data/phantom.h5
```

## 1 — LIVE + k-space (recon)
```bash
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest \
  python3 client.py -c invertcontrast -o /data/out.h5 --address mri-marshal --port 9100 /data/phantom.h5
curl -s localhost:8080/image/latest        # 200 -> .../from_reconstruction/latest_image.h5
```

## 2 — LIVE + image (bypass recon)
```bash
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest \
  python3 client.py -o /data/out_img.h5 --address mri-marshal --port 9100 \
  /data/live/from_reconstruction/latest_image.h5
curl -s localhost:8080/image/latest        # 200 -> .../from_scanner/latest_image.h5
```

## Switch to dump
```bash
MARSHAL_DUMP=--dump docker compose --profile test-recon up -d --force-recreate mri-marshal
```

## 3 — DUMP + k-space
```bash
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest \
  python3 client.py -c invertcontrast -o /data/out.h5 --address mri-marshal --port 9100 /data/phantom.h5
curl -s -o /dev/null -w "%{http_code}\n" localhost:8080/image/latest   # 404
ls session-data/dump/from_scanner session-data/dump/from_reconstruction # scan_*.h5
```

## 4 — DUMP + image
```bash
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest \
  python3 client.py -o /data/out_img.h5 --address mri-marshal --port 9100 \
  /data/live/from_scanner/latest_image.h5
curl -s -o /dev/null -w "%{http_code}\n" localhost:8080/image/latest   # 404
```

## Back to live / teardown
```bash
docker compose --profile test-recon up -d --force-recreate mri-marshal   # live again (webgl at :3000)
docker compose --profile test-recon down                                 # stop
```

Notes: from WSL `curl localhost:8080` works (ports publish to the host). Same phantom looks
identical every run — confirm freshness via file mtime, a new `scan_<ts>.h5`, or
`curl localhost:8080/debug/perf` counters.
