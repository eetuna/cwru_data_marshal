# External Client Integration Guide

How to integrate your own client (Python, C++, or any language) with the CWRU
Data Marshal stack after loading the Docker images from USB.

This guide covers **deployment** and the three integrator tasks:

1. Connect a custom MRD TCP client (drive the scanner side).
2. Read the latest reconstructed image (`GET /image/latest` + open the HDF5).
3. Consume / produce robot data (`/read` and `/write`).

For the full endpoint reference see [API_REFERENCE.md](API_REFERENCE.md); for the
MRD TCP wire protocol see [ARCHITECTURE.md](ARCHITECTURE.md). This guide does not
duplicate those.

---

## Deploy

The USB package (produced by `scripts/export_usb.sh`) contains:

- `cwru-images.tar` — a single `docker save` of 5 images:
  `cwru/mri-marshal`, `cwru/robot-marshal`, `cwru/robot-clients`,
  `cwru/webgl-client`, `fire-python`.
- `docker-compose.yml` — the single stack definition.
- `README.md` — receiver quick start.

```bash
# 1. Load images
docker load -i cwru-images.tar
docker images | grep -E "cwru|fire-python"

# 2. Start the stack (point at your production recon)
mkdir -p session-data
RECON_HOST=<recon-ip> RECON_PORT=<port> docker compose up -d   # live mode (default)
docker compose ps                                              # wait until healthy

# 3. Verify
curl http://localhost:8080/health           # MRI Marshal  -> {"status":"ok"}
curl http://localhost:8081/                  # Robot Marshal (HTML)
```

**Recon target.** The marshal forwards k-space to the recon service at
`RECON_HOST:RECON_PORT` (default `recon:9002`). Point it at a production recon
with `RECON_HOST=<ip> RECON_PORT=<port>`. To run the bundled test recon instead,
enable the `test-recon` profile (the `recon` service is off by default):

```bash
docker compose --profile test-recon up -d
```

Prefix either start command with `MARSHAL_DUMP=--dump` for dump/archival mode.

**Configuration knobs:**

- `MARSHAL_DUMP` — unset = live mode (mid-scan `/image/latest` snapshot pipeline
  active); `--dump` = archival-only mode (canonical ISMRMRD H5 written at scan
  close, no live snapshot). The two modes are mutually exclusive.
- `RECON_HOST` / `RECON_PORT` — recon target (default `recon:9002`). Set to the
  real recon's IP in production; leave unset to get the default. Never pass an
  *empty* `RECON_HOST` through to the marshal command line (possible with a
  hand-edited compose file or `docker run -e RECON_HOST=`) — the marshal
  detects the resulting misparse and refuses to start.
- `MARSHAL_LATEST` — RAM snapshot toggle. Default on (`--latest-dir /latest`,
  host `/dev/shm/cwru-latest`); set empty (`MARSHAL_LATEST=`) to put the
  snapshot back on disk under `session-data/live/`.
- `SESSION_DATA_DIR` — host path for the session-data volume.
- `HTTP_PORT` / `MRD_PORT` / `ROBOT_PORT` / `UI_PORT` / `WRITE_PORT` — exposed
  host ports.

---

## Services, ports, and networking

| Service | Container | Host port | Purpose |
|---------|-----------|-----------|---------|
| MRI Marshal | `mri-marshal` | 8080 | HTTP query/control API |
| MRI Marshal | `mri-marshal` | 9100 | **MRD TCP** (scanner connects here) |
| Recon (`python-ismrmrd-server`) | `recon` | 9002 (internal) | bundled test recon (`test-recon` profile); production uses `RECON_HOST`/`RECON_PORT` |
| Robot Marshal | `robot-marshal` | 8081 | HTTP `/read` + `/write` API |
| Robot data clients | 6 containers | — | fill robot-marshal so webgl has a scene |
| WebGL client (viewer) | `webgl-client` | 3000 / 3001 | browser UI (`http://localhost:3000`) |

The **viewer is the webgl-client** at `http://localhost:3000` — open it in a
browser on the host.

Ports bind to `0.0.0.0`, so remote clients on the same LAN can connect using the
host IP:

| Client location | MRI Marshal | Robot Marshal |
|-----------------|-------------|---------------|
| Same machine | `http://localhost:8080` | `http://localhost:8081` |
| Same LAN | `http://<host-ip>:8080` | `http://<host-ip>:8081` |
| Inside a container on the stack network | `http://mri-marshal:8080` | `http://robot-marshal:8081` |

To join a containerized client to the stack network, attach it to the external
network `cwru-demo-net`:

```yaml
services:
  my-client:
    networks: [cwru-net]
networks:
  cwru-net:
    name: cwru-demo-net
    external: true
```

Find the host IP with `hostname -I | awk '{print $1}'`.

---

## Task 1 — Connect a custom MRD TCP client (scanner side)

Scanner and recon data do **not** use HTTP. They use **raw MRD over TCP** with
2-byte little-endian message-ID framing — the same wire format as
`python-ismrmrd-server`. Point your scanner-side client at `mri-marshal:9100`
(or `<host-ip>:9100`). A session sends:

```
CONFIG_FILE + METADATA_XML + ACQUISITION x N (+ WAVEFORM) + CLOSE
```

The marshal forwards k-space to the recon service and pushes reconstructed
`IMAGE` messages back on the same socket. (Scanner-sent images bypass recon and
go straight to the viewer.) See [ARCHITECTURE.md](ARCHITECTURE.md) for message
IDs, framing, and body formats.

If you don't have a real scanner, drive the stack with `python-ismrmrd-server`'s
`client.py` from the bundled `fire-python` image:

```bash
# Generate a phantom
docker run --rm -v "$PWD/session-data:/data" fire-python:latest \
  python3 generate_cartesian_shepp_logan_dataset.py -o /data/phantom.h5

# Stream it to the marshal's MRD TCP port
docker run --rm --network cwru-demo-net -v "$PWD/session-data:/data" \
  fire-python:latest python3 client.py -c invertcontrast -o /data/out.h5 \
  --address mri-marshal --port 9100 /data/phantom.h5
```

Any client that speaks the python-ismrmrd-server TCP protocol works here.

---

## Task 2 — Read the latest reconstructed image

In **live mode**, poll `GET /image/latest`. It returns a pointer to a stable,
closed HDF5 snapshot that the marshal atomically renames on each new image:

```json
{ "path": "/latest/live/from_reconstruction/latest_image.h5",
  "error": false, "generation": 42 }
```

- `path` — closed HDF5 file; image data lives under group `image_0`. With the
  default stack the snapshot lives on the RAM-backed `/latest` volume (host
  `/dev/shm/cwru-latest`); with `MARSHAL_LATEST=` (disk mode) it is under
  `session-data/live/...` instead.
- `generation` — increments per published image; poll cheaply by comparing it.
- `error` — `true` if reconstruction is currently failing (`path` then points to
  a `latest_error.png`).
- **`204 No Content`** — no live image published yet this scan.
- **`404 Not Found`** — dump mode: archival-only. Read the per-scan
  `dump/from_reconstruction/scan_*.h5` after the scan ends.

**Reader on another machine (or no shared volume)?** Skip the path entirely and
fetch the bytes over HTTP — `GET /image/latest.h5` returns the snapshot file
itself (supports `ETag`/`If-None-Match` keyed on `generation`):

```python
r = requests.get(f"{MRI}/image/latest.h5")
open("latest.h5", "wb").write(r.content)
```

```python
import time, requests, h5py

MRI = "http://localhost:8080"
while True:
    r = requests.get(f"{MRI}/image/latest")
    if r.status_code == 200:
        info = r.json()
        if not info["error"]:
            # Open with locking disabled; the file is closed & atomically renamed.
            with h5py.File(info["path"], "r") as f:
                img = f["image_0"]["data"][:]   # ISMRMRD image group
                print("image", img.shape)
    elif r.status_code == 204:
        pass  # nothing yet
    time.sleep(0.05)
```

A container reader that follows the `path` must mount the **`/latest` volume**
read-only (host `/dev/shm/cwru-latest` — see webgl-client in
`docker-compose.yml`, which mounts both `/latest:ro` and `/session-data:ro`);
per-scan archives stay on `session-data`. Readers without volume access use
`GET /image/latest.h5` instead. See [HDF5 notes](#hdf5-reading-notes) below.

---

## Task 3 — Consume and produce robot data

The Robot Marshal is a small virtual-file HTTP service (base
`http://localhost:8081`):

- `GET /` — list channels (HTML).
- `GET /read/<channel>` — latest data: `{"entries":[{"sent_at":<ns>,"values":[...]}]}`.
- `POST /write/<channel>` — body `{"values":[...],"sent_at":<ns>}`.

Channel names are the **verbatim entries** in the robot marshal's `files.json`
(seeded into the image) — full names with the `file_` prefix and `.json`
extension, e.g. `file_tip_position_orientation.json`, `file_user_input.json`,
`file_localization_data.json`, `file_forward_kinematics.json`,
`file_biological_signals.json`, `file_streaming_2D_images.json`,
`file_3D_images.json`. `GET /` lists them all; the full set is in
[API_REFERENCE.md](API_REFERENCE.md).

```python
import requests
ROBOT = "http://localhost:8081"

tip = requests.get(f"{ROBOT}/read/file_tip_position_orientation.json").json()
print("tip values:", tip["entries"][0]["values"])

requests.post(f"{ROBOT}/write/file_user_input.json",
              json={"values": [10, 20, 30], "sent_at": 1706126625123456789})
```

```bash
curl http://localhost:8081/read/file_tip_position_orientation.json
curl -X POST http://localhost:8081/write/file_user_input.json \
  -H "Content-Type: application/json" \
  -d '{"values":[10,20,30],"sent_at":1706126625123456789}'
```

---

## HTTP query/control (pose & transform)

The MRI Marshal also exposes pose and slice-transform endpoints for
non-scanner control clients (bodies use `position`/`orientation` and the
transform delta fields). See [API_REFERENCE.md](API_REFERENCE.md) for exact
shapes; brief example:

```bash
curl http://localhost:8080/pose
curl -X POST http://localhost:8080/pose -H "Content-Type: application/json" \
  -d '{"position":[1,2,3],"orientation":[0,0,0.707,0.707]}'
curl http://localhost:8080/transform          # GET consumes-on-read (zeros delta)
```

For debugging throughput/retention use `GET /debug/perf` and `GET /debug/sinks`
(documented in API_REFERENCE.md).

---

## HDF5 reading notes

Snapshot and archive files are canonical ISMRMRD HDF5, openable with default
ISMRMRD / h5py / HDFView settings.

- Set `HDF5_USE_FILE_LOCKING=FALSE` in the reader's environment.
- `latest_image.h5` is always a closed file (atomic rename), so a plain open is
  safe. The per-scan `scan_<ts>.h5` archive is finalized at scan close — open it
  after the scan, not mid-scan.
- Image contents are under group `image_0`.

---

## C++ integration note

C++ clients use the same HTTP/JSON API — any HTTP client works (e.g.
[cpp-httplib](https://github.com/yhirose/cpp-httplib) +
[nlohmann/json](https://github.com/nlohmann/json)):

```cpp
httplib::Client mri("localhost", 8080);
auto r = mri.Get("/image/latest");
if (r && r->status == 200) {
    auto info = nlohmann::json::parse(r->body);
    // info["path"], info["error"] -> open the HDF5 with an ISMRMRD/HDF5 lib
} // r->status == 204: none yet; 404: dump mode
```

For the scanner side in C++, speak the MRD TCP protocol on port 9100
(see [ARCHITECTURE.md](ARCHITECTURE.md)).

---

## Security

No authentication — designed for an isolated lab/clinical network. For wider
deployment: restrict ports by firewall, run on an isolated segment, and front
with a TLS reverse proxy or VPN as needed.

---

## Troubleshooting

```bash
# Are containers up / healthy?
docker compose ps
docker logs cwru-mri-marshal
docker logs cwru-robot-marshal

# Recon "does nothing" (no recon images, viewer shows only scanner data)?
# A per-scan "Failed to connect to recon at <host>:<port>" line means
# RECON_HOST points at nothing reachable.
docker logs cwru-mri-marshal | grep -i recon

# Rebuilt the images but new features are missing? The build script prints
# "Building from: <branch> @ <commit>" — compare that commit against GitHub
# (it builds a clean export of the branch; git pull on main never changes
# what is built). If the commit is right but the UI is old, the containers
# were not recreated — plain `up -d` keeps old containers on old images:
git ls-remote https://github.com/cwru-mercis/cwru_data_marshal.git mri-data-marshal robot_data_marshal_with_catheter_system_components
docker compose up -d --force-recreate

# Ports listening?
netstat -tlnp | grep -E '8080|8081|9100|3000'

# Health
curl -v http://localhost:8080/health
curl -v http://localhost:8081/

# Data-file permissions (host reader)
sudo chown -R $USER:$USER ./session-data

# HDF5 locking errors
export HDF5_USE_FILE_LOCKING=FALSE
```

Teardown: `docker compose down`.
