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

## Streaming (fire only, watch it update at localhost:3000)

Must be **live** mode, browser open. Fire protocol only (no mock streamers). `Ctrl-C` stops.

### Realistic single-connection streaming — `scripts/fire_stream.py` (recommended)
One persistent MRD connection, paced, a *moving* phantom (image changes every frame) —
closest to a real scan. Uses python-ismrmrd-server's `connection.py`.

```bash
# k-space -> recon -> webgl  (5 fps, forever; Ctrl-C to stop)
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/fire_stream.py --address mri-marshal --port 9100 --mode kspace --fps 5 --frames 0 --matrix 128

# direct image -> webgl (bypass recon)
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/fire_stream.py --address mri-marshal --port 9100 --mode image --fps 5 --frames 0 --matrix 128
```
Flags: `--mode kspace|image` · `--fps` rate · `--frames 0` = forever · `--matrix` size.
The orbiting bright dot makes each frame visibly different. `--mode kspace` lands in
`from_reconstruction`, `--mode image` in `from_scanner`.

**Dump streaming:** flip marshal to dump, run the same command; UI blank, archives grow:
```bash
MARSHAL_DUMP=--dump docker compose --profile test-recon up -d --force-recreate mri-marshal
# ...run fire_stream.py (either mode)...
watch -n1 'ls session-data/dump/from_scanner/ | wc -l'     # climbs each frame
```

### Quick loop alternative (client.py, contrast flips each frame)
```bash
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest \
  sh -c 'i=0; while true; do
    [ $((i%2)) -eq 0 ] && c=invertcontrast || c=simplefft
    python3 client.py -c $c -o /data/out.h5 --address mri-marshal --port 9100 /data/phantom.h5
    i=$((i+1)); sleep 0.5
  done'
```
Browser (`from_reconstruction`) flips inverted ↔ normal every ~0.5s.

### Stream direct images → webgl (bypass recon)
Prep a few images with different noise once, then stream them direct in rotation:
```bash
# prep (recon k-space to image files, varying noise)
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest sh -c '
  for n in 0.05 0.3 0.6; do
    python3 generate_cartesian_shepp_logan_dataset.py -o /data/p.h5 -n $n >/dev/null 2>&1
    python3 client.py -c simplefft -o /data/img_$n.h5 --address mri-marshal --port 9100 /data/p.h5
  done'

# stream them DIRECT, in a loop (from_scanner, no recon; grain changes each frame)
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest sh -c '
  while true; do for n in 0.05 0.3 0.6; do
    python3 client.py -o /data/out.h5 --address mri-marshal --port 9100 /data/img_$n.h5
    sleep 0.5
  done; done'
```
Browser (`from_scanner`) cycles through the three noise levels — visibly changing, recon untouched.

### Streaming in dump mode (archives pile up; UI blank by design)
Flip marshal to dump, then run **either stream loop above, unchanged**:
```bash
MARSHAL_DUMP=--dump docker compose --profile test-recon up -d --force-recreate mri-marshal
# ...now run the k-space loop OR the direct-image loop from above...
```
Nothing shows in the browser (`/image/latest` = 404). Instead watch the per-scan archives
accumulate — one `scan_<ts>.h5` per loop iteration:
```bash
watch -n1 'ls session-data/dump/from_scanner/ | wc -l'          # count climbs each frame
# k-space also fills dump/from_reconstruction/:
ls session-data/dump/from_reconstruction/ | wc -l
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
