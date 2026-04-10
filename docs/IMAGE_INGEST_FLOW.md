# Image Ingest & Reconstruction Flow

## Scanner-side ingest

The scanner client posts data through four endpoints:

| Endpoint | Purpose |
|---|---|
| `POST /header` | ISMRMRD XML header for the session. |
| `POST /config` | Reconstruction config (e.g. `simplefft`). |
| `POST /frame` | One acquisition or image frame (binary). |
| `POST /close` | End-of-session marker; flushes and closes the HDF5 file. |

Marshal writes every frame into `from_scanner/*.h5`.

When `--recon-url` is set, each acquisition frame is forwarded to the reconstruction service at `POST /image` on that URL. If `--recon-url` is not set, acquisition frames are stored but no reconstruction occurs.

## Reconstruction callback

The recon service posts reconstructed images back to the marshal:

| Endpoint | Purpose |
|---|---|
| `POST /image` | Reconstructed image (binary). |

Marshal writes these into `from_reconstruction/*.h5`.

## Query endpoints

Downstream consumers (viz, robot bridge, etc.) read data via:

| Endpoint | Method | Purpose |
|---|---|---|
| `/image/latest` | GET | Latest reconstructed image (binary). |
| `/transform` | GET | Current transform. |
| `/pose` | GET | Current pose. |
| `/health` | GET | Liveness check. |

The viz client polls `GET /image/latest` to display frames. It reads a standalone HDF5 file and does not need concurrent-access modes.

## Storage layout

```
<dump-dir>/
  from_scanner/
    *.h5          -- raw data written by marshal
  from_reconstruction/
    *.h5          -- reconstructed images written by marshal
```

The `--dump-dir` flag controls the root directory.

## Diagram

```
                      +-------------+
                      |   Scanner   |
                      +------+------+
                             | POST /header
                             | POST /config
                             | POST /frame
                             | POST /close
                             v
                  +--------------------------+
                  |       MRI Marshal        |
                  |  (--http host:port)      |
                  +-+----------+-----------+-+
                    |          |           |
                    v          |           v
          +--------------+    |    +----------------+
          | from_scanner |    |    | GET /image/    |
          |   /*.h5      |    |    |   latest       |
          +--------------+    |    | GET /transform |
                              |    | GET /pose      |
                              |    | GET /health    |
                              |    +-------+--------+
                              |            |
                   if --recon-url set      v
                              |         Clients
                              v         (viz, bridge)
                    +-----------------+
                    | Recon Service   |
                    | POST /image     |
                    +---------+-------+
                              |
                              | POST /image (callback)
                              v
                  +--------------------------+
                  |       MRI Marshal        |
                  +-----------+--------------+
                              |
                              v
                  +----------------------+
                  | from_reconstruction/ |
                  |   *.h5               |
                  +----------------------+
```

## Flags

| Flag | Purpose |
|---|---|
| `--http host:port` | Address the marshal listens on. |
| `--recon-url URL` | Where to forward acquisition frames for reconstruction. |
| `--dump-dir PATH` | Root directory for HDF5 output. |
