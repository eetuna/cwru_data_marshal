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
curl -s localhost:8080/image/latest        # 200, path ends .../from_reconstruction/latest_image.h5
# save the recon image as input for test 2 (works for RAM or disk snapshot):
curl -s localhost:8080/image/latest.h5 -o session-data/snap_recon.h5
```

## 2 — LIVE + image (bypass recon)
```bash
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" fire-python:latest \
  python3 client.py -o /data/out_img.h5 --address mri-marshal --port 9100 /data/snap_recon.h5
curl -s localhost:8080/image/latest        # 200, path ends .../from_scanner/latest_image.h5
# save the scanner-lane image as input for test 4:
curl -s localhost:8080/image/latest.h5 -o session-data/snap_scanner.h5
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
  python3 client.py -o /data/out_img.h5 --address mri-marshal --port 9100 /data/snap_scanner.h5
curl -s -o /dev/null -w "%{http_code}\n" localhost:8080/image/latest   # 404
```

## Streaming (fire only, watch it update at localhost:3000)

Must be **live** mode, browser open. Fire protocol only (no mock streamers). `Ctrl-C` stops.

### Realistic single-connection streaming — `scripts/fire_stream.py` (recommended)
One persistent MRD connection, paced, a *moving* phantom (image changes every frame) —
closest to a real scan. Uses python-ismrmrd-server's `connection.py`.

```bash
# k-space -> recon -> webgl  (10 fps, forever; Ctrl-C to stop)
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/fire_stream.py --address mri-marshal --port 9100 --mode kspace --fps 10 --frames 0 --matrix 128

# direct image -> webgl (bypass recon)
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/fire_stream.py --address mri-marshal --port 9100 --mode image --fps 10 --frames 0 --matrix 128

# MULTISLICE: 5-slice k-space volume per frame (2D panel = middle slice, 3D panel = all slices)
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/fire_stream.py --address mri-marshal --port 9100 --mode kspace --fps 5 --frames 0 --matrix 128 --slices 5

# MULTISLICE image mode at target size (192x192x8)
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
  python3 /scripts/fire_stream.py --address mri-marshal --port 9100 --mode image --fps 10 --frames 0 --matrix 192 --slices 8
```
Flags: `--mode kspace|image` · `--fps` rate · `--frames 0` = forever · `--matrix` size · `--slices` N.
The orbiting bright dot makes each frame visibly different (each slice has its own phase).
`--mode kspace` lands in `from_reconstruction`, `--mode image` in `from_scanner`.
Expected rates (Python test-tool caps, not marshal): single-slice k-space ~10 fps,
5-slice k-space ~3-4 volumes/s, image mode ~16 fps at 192×8.

**Verify multislice while streaming** (snapshot = exactly one volume; 3D read = all slices):
```bash
docker exec cwru-webgl-client python3 -c "
import h5py,os; os.environ['HDF5_USE_FILE_LOCKING']='FALSE'
print(h5py.File('/latest/live/from_reconstruction/latest_image.h5','r')['dataset/image_0/data'].shape)"
# (default RAM snapshot path; with MARSHAL_LATEST= (disk) use
#  /session-data/live/from_reconstruction/latest_image.h5)
#   -> (5, 1, 1, 128, 128)  = 5 slices, not growing over time
docker exec cwru-webgl-client sh -c 'curl -s localhost:3000/api/read/client-webgl/1 | python3 -c "import sys,json; d=json.load(sys.stdin); print(d[\"width\"],d[\"height\"],\"depth\",d[\"depth\"])"'
#   -> 128 128 depth 5
```

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

## Troubleshooting

- **First check, always:** `curl localhost:8080/status` — mode, scanner/recon
  connectivity, active scan, last-image age, free disk, in one response.
- **Stop streams with Ctrl-C**, not `docker rm -f` — Ctrl-C sends the MRD CLOSE. (A killed
  stream is tolerated: the marshal accepts a new connection immediately and finalizes the
  abandoned session in the background.)
- **Recon down or unreachable**: scans proceed archived-only; the scanner receives a
  failure image and a CLOSE (never hangs). The recon connect attempt is bounded (5 s) and
  retried once per scan — restart the recon and start the next scan.
- **Viewer static while streaming**: hard-refresh the browser (Ctrl-Shift-R) after any
  webgl-client recreate; confirm frames server-side with
  `curl -s localhost:8080/debug/perf | grep -o '"recon_images":[0-9]*'` (run twice — climbs).
- **Throughput checks**: `latest_writer.last_write_us` should stay flat (~2–10 ms) and the
  snapshot shape constant for any stream length; if either grows, something regressed.

## Performance check (marshal-only)

`scripts/mrd_bench.py` drives the marshal with byte-pump endpoints so neither the
streamer's nor the recon's speed pollutes the numbers. Reference numbers and full
procedure: `docs/reviews/2026-07-03-marshal-performance-audit.md`. Quick forward-path
check (expect ≥60 fps of 5-slice 96² frames, ≥190 MB/s):

```bash
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/probe" fire-python:latest \
  python3 /probe/mrd_bench.py gen --dir /probe/bench_data --matrix 96 --coils 8 --slices 5
docker stop cwru-recon
docker run -d --name bench-sink --network cwru-demo-net --network-alias recon \
  -v "$PWD/scripts:/probe" fire-python:latest \
  python3 /probe/mrd_bench.py sink --dir /probe/bench_data --port 9002 --acq-body 6484 --nacq 480 --once
docker run --rm --network cwru-demo-net -v "$PWD/scripts:/probe" fire-python:latest \
  python3 /probe/mrd_bench.py blast --dir /probe/bench_data --address mri-marshal --port 9100 --frames 200 --fps 0
docker rm -f bench-sink; docker start cwru-recon
```

**Acceptance for the real recon (April):** repeat with the real recon as the sink
target at the experiment's matrix/slices; if recon consumes faster than the marshal
forwards, the marshal is the bottleneck and needs another look — otherwise done.
