# MRI Data Marshal: Usage and API Reference

Technical reference for operating and integrating with the MRI Data Marshal.

---

## Overview

The MRI Data Marshal is an HTTP server that sits between the MRI scanner and the reconstruction service. It:

- Archives all scanner data to canonical libismrmrd HDF5 files (`from_scanner/`)
- Forwards scanner data to the reconstruction service over HTTP
- Archives reconstructed images to HDF5 files (`from_reconstruction/`)
- Serves the latest reconstructed image for live visualization
- Caches robot pose and slice-repositioning transforms
- Stays running even if reconstruction fails

---

## Running the Marshal

```bash
./build/marshal --http 0.0.0.0:8080 \
                --dump-dir ./data \
                --recon-url http://recon-host:9002
```

### Command-Line Flags

| Flag | Description | Default |
|------|-------------|---------|
| `--http host:port` | HTTP listen address | `0.0.0.0:8080` |
| `--dump-dir PATH` | Root for `from_scanner/` and `from_reconstruction/` directories | required |
| `--recon-url URL` | Reconstruction service base URL | unset (archival-only) |
| `--ws-port N` | Optional WebSocket listener port | unset |

When `--recon-url` is omitted, the marshal runs in archival-only mode: scanner data is written to HDF5 but not forwarded anywhere.

---

## HTTP API

### Scanner-Facing (archive + forward to recon)

| Method | Path | Body | Response | Description |
|--------|------|------|----------|-------------|
| POST | `/header` | ISMRMRD XML bytes | 200 / 400 | Start scan, open HDF5 file |
| POST | `/config` | Config name bytes | 200 / 409 | Set recon config (after /header) |
| POST | `/frame` | One ISMRMRD message | 202 / 409 | Archive + forward acquisition/image/waveform |
| POST | `/close` | empty | 200 | End scan, close HDF5 files |

### Recon-Facing (archive only, never forwarded)

| Method | Path | Body | Response | Description |
|--------|------|------|----------|-------------|
| POST | `/image` | ISMRMRD image wire bytes | 200 | Archive recon image, update latest file |

### Query and Control

| Method | Path | Body | Response | Description |
|--------|------|------|----------|-------------|
| GET | `/image/latest` | -- | JSON | Path to latest image file |
| GET | `/transform` | -- | JSON | Read and zero slice delta |
| PUT | `/transform` | JSON | 200 | Write slice delta |
| POST | `/pose` | JSON | 200 | Update cached pose |
| GET | `/pose` | -- | JSON | Read cached pose |
| GET | `/dump/scanner` | -- | JSON | List scanner HDF5 files |
| GET | `/dump/recon` | -- | JSON | List recon HDF5 files |
| GET | `/health` | -- | JSON | Health check |

---

## Scan Lifecycle

```
Scanner                    Marshal                     Recon
  |                          |                           |
  |-- POST /header --------->|-- POST /header ---------->|
  |-- POST /config --------->|-- POST /config ---------->|
  |-- POST /frame (acq) ---->|-- POST /frame (acq) ---->|
  |-- POST /frame (acq) ---->|-- POST /frame (acq) ---->|
  |          ...              |          ...              |
  |                          |<--- POST /image ----------|
  |                          |<--- POST /image ----------|
  |-- POST /close ---------->|-- POST /close ----------->|
  |                          |                           |
```

The marshal opens `from_scanner/scan_<ts>.h5` on `/header` and closes it on `/close`. The recon sink (`from_reconstruction/scan_<ts>.h5`) is opened lazily on the first `POST /image` and closed by `/close`.

---

## Data Storage

### Directory Layout

```
${dump_dir}/
  from_scanner/
    scan_1712764800.h5       # canonical ISMRMRD HDF5 (acquisitions, images, waveforms)
    scan_1712764900.h5
  from_reconstruction/
    scan_1712764800.h5       # canonical ISMRMRD HDF5 (reconstructed images)
    latest_image.bin         # standalone file: latest recon image (raw wire bytes)
    latest_error.png         # written when recon is down
```

### HDF5 Format

All HDF5 files use the canonical libismrmrd layout written via `appendAcquisition`, `appendImage`, and `appendWaveform`. They are standard ISMRMRD datasets readable with:

```python
import ismrmrd
dset = ismrmrd.Dataset("scan_1712764800.h5", "dataset", False)
header = dset.read_xml_header()
acq = dset.read_acquisition(0)
dset.close()
```

### Standalone Image File

The `latest_image.bin` file contains the raw ISMRMRD image wire format: 198-byte ImageHeader + 8-byte uint64 attribute_string_len + attribute string + pixel data. This is the same byte layout as the `POST /image` request body. The viz client reads this file directly for live display.

---

## Visualization

The viz client polls `GET /image/latest` for the file path, then opens the file from disk:

- If `error` is `false`: reads `latest_image.bin` as ISMRMRD image wire bytes
- If `error` is `true`: reads `latest_error.png` as a PNG (reconstruction failed)

```bash
./build/viz_client --http http://localhost:8080
```

No network image delivery. The viz client and marshal share access to the dump directory (via filesystem or Docker volume mount).

---

## Reconstruction Interface

The reconstruction service receives forwarded scanner traffic at its own HTTP port:

- `POST /header` -- ISMRMRD XML
- `POST /config` -- config name
- `POST /frame` -- raw ISMRMRD message (byte-for-byte from scanner)
- `POST /close` -- end of scan

The recon service posts results back to the marshal:

- `POST /image` -- ISMRMRD image wire bytes

The marshal is a transparent proxy: the recon sees identical bytes whether data comes from a real scanner or from a mock.

---

## Resilience

If the reconstruction service is unreachable:

1. `POST /frame` still returns `202` -- the marshal never blocks on recon.
2. Scanner data continues to be archived to `from_scanner/`.
3. The marshal writes a "reconstruction failed" PNG and updates `GET /image/latest` to point to it with `"error": true`.
4. When recon comes back, new frames are forwarded normally.

---

## Clients

### C++ Clients (in `clients/`)

| Binary | Purpose |
|--------|---------|
| `kspace_streamer` | Full scanner mock: sends header + config + acquisitions + close |
| `image_streamer` | Sends synthetic ISMRMRD images via /header + /config + /frame + /close |
| `viz_client` | Polls /image/latest, reads standalone file, displays with OpenCV |

### Python Mock Clients (in `clients/mocks/`)

| Script | Purpose |
|--------|---------|
| `ecg_client.py` | Sends ISMRMRD waveforms (ECG) via POST /frame |
| `pose_client.py` | Sends pose updates via POST /pose |
| `http_tracker.py` | Polls GET /image/latest and GET /pose |
