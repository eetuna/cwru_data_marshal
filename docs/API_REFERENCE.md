# API reference

This document introduces the marshal's external interfaces from the perspective
of an integrator seeing the project for the first time. All examples use
`http://localhost:8080` as the base URL; substitute your deployment address as
needed.

The marshal exposes a small HTTP API for ingesting MRD data, plus an optional
WebSocket broadcast channel for metadata fan-out.

---

## Conventions

- **Content types:** binary MRD payloads use `application/octet-stream`. JSON
  responses are encoded as UTF-8.
- **Timestamps:** metadata files and WebSocket messages use ISO-8601 timestamps
  with millisecond precision in UTC.
- **Authentication:** not provided by the marshal. Run it behind your own
  ingress/proxy if needed.
- **Error format:** errors return an HTTP status code (4xx or 5xx) and a JSON
  body of the form `{ "error": "human readable" }`.

---

## `POST /v1/mrd/ingest`

Upload a complete ISMRMRD (`.mrd`) file. The marshal writes it to the current
sink (live MRD or dumpbox session) and updates the corresponding metadata files.

### Request

- **Headers**
  - `Content-Type: application/octet-stream`
- **Body**: raw MRD file bytes.

### Response

- **Status 200** on success with JSON metadata describing where the file was
  stored. Example (live MRD sink):

  ```json
  {
    "path": "streamA.mrd",
    "size": 16384,
    "timestamp": "2025-09-20T01:23:45.123Z"
  }
  ```
- **Status 400** if the body is empty or the file cannot be written.
- **Status 500** for unexpected errors.

### Side effects

- Writes the binary file to either `data/mrd/` (live mode) or
  `data/dumpbox/<session>/files/` (record mode).
- Appends a JSON line to the appropriate `index.jsonl`.
- Rewrites the relevant `latest.json` with the most recent metadata, including
  a `flushed` boolean so readers can tell when the HDF5 dataset has been
  refreshed.
- Broadcasts the metadata JSON over the WebSocket channel (if any clients are
  connected).

---

## `POST /v1/ismrmrd/frame`

Append an ISMRMRD Image message directly into a growing HDF5 dataset while the
file remains open in Single-Writer-Multiple-Reader (SWMR) mode. Each logical
stream maps to one `.mrd` file under the current sink.

The sink monitors geometry (`nx`, `ny`, `nz`, channels). If an incoming frame
does not match the active file, the marshal flushes the existing MRD and
continues in a new file whose name encodes the new dimensions. This keeps SWMR
streams fast without manual intervention.

### Required headers

| Header | Description |
| ------ | ----------- |
| `X-MRD-Stream` | Logical stream identifier (ASCII). Determines the `.mrd` filename. |

### Body

Binary concatenation of:

1. `ISMRMRD::ImageHeader` (as defined in `ismrmrd/ismrmrd.h`).
2. Raw voxel payload whose size matches `matrix_size × channels × datatype`.

Supported datatypes are:

- Real `float32`
- Signed `int16`
- Unsigned `uint16`
- Complex `complex64` (two `float32` values per voxel)

#### Packing example (Python)

Repository helpers: `tools/make_image_message.py` (single frame) and
`tools/stream_image_series.py` (continuous wobble) wrap the same logic. The C++
counterparts live in `tools/make_image_message.cpp` and
`clients/image_streamer/image_streamer_main.cpp` respectively.

```bash
pip install ismrmrd numpy  # once per environment
python - <<'PY'
import numpy as np
import ismrmrd

# Create a 4x3 single-channel float32 image with obvious values
data = np.arange(12, dtype=np.float32).reshape(3, 4)
img = ismrmrd.Image.from_array(data, transpose=False)
head = img.getHead()
head.channels = 1
head.data_type = ismrmrd.DATATYPE_FLOAT
img.setHead(head)

with open('image_message.bin', 'wb') as f:
    f.write(bytes(head))
    f.write(img.data.tobytes())
PY
```

### Response

JSON describing the stream after the append:

```json
{
  "stream": "demo_stream",
  "frame_index": 2,
  "flushed": true,
  "dims": [128, 128, 8],
  "channels": 1,
  "datatype": "float32",
  "size_bytes": 524288,
  "path": "demo_stream.mrd",
  "ts": "2025-09-20T01:23:45.123Z"
}
```

- **Status 200** on success.
- **Status 400** if headers are missing/invalid or the payload length does not
  match the ISMRMRD header metadata.
- **Status 500** if the marshal fails to write or flush the SWMR dataset.

### Reader expectations

- Readers open the `.mrd` file using `H5F_ACC_SWMR_READ`. The marshal flushes
  dataset + file metadata according to its coalescing policy, configured via
  `--flush-max-frames` (default 4) and `--flush-max-ms` (default 50 ms). When
  the response (and corresponding broadcast JSON) includes `"flushed": false`,
  the frame has been written but not yet made visible to SWMR readers. Poll the
  WebSocket or `latest.json` until a `"flushed": true` entry appears before
  attempting to read.
- Image voxels live in `/images/data` with shape `[frames, channels, z, y, x]`.
- A minimal ISMRMRD XML header is stored at `/header`.

---

## `GET /health`

Simple readiness probe. Returns HTTP 200 with JSON `{ "status": "ok",
"uptime_s": <seconds since start> }` when the marshal is running.

---

## `POST /v1/pose/update`

Store the latest scanner pose and broadcast it to connected WebSocket clients.

### Request

- **Headers**
  - `Content-Type: application/json`
- **Body**
  - `p`: array of three translation values (meters)
  - `R`: array of nine rotation matrix coefficients (row-major)
  - Optional `frame`/`source` strings for metadata

### Response

- **Status 200** with JSON `{ "status": "ok", "pose": { ... } }` describing
  the stored pose and including a timestamp.
- **Status 400** if required fields are missing or have the wrong lengths.

---

## `GET /v1/pose/current`

Return the most recently stored pose (if any). Helpful for monitoring dashboards
that want the current scanner transform without subscribing to WebSockets.

### Response

- **Status 200** with JSON `{ "pose": { ... }, "source": "..." }`. If no pose
  has been submitted yet the marshal returns an identity transform.

---

## `GET /v1/mrd/since`

> This endpoint is optional and may be disabled in some builds.

Page through ingest metadata without touching the filesystem directly.

### Query parameters

| Name | Required | Description |
| ---- | -------- | ----------- |
| `ts` | no | ISO-8601 timestamp to start from (exclusive). Defaults to the beginning of the log. |
| `limit` | no | Maximum number of records to return. Defaults to 50. |

### Response

JSON array of metadata objects in chronological order.

---

## WebSocket `/ws`

The marshal accepts binary MRD payloads over WebSocket (mirroring the HTTP
`/v1/mrd/ingest` endpoint) and broadcasts JSON metadata to all connected clients
whenever an ingest completes.

- **Binary frames** are treated exactly like HTTP uploads.
- **Text frames** are ignored.
- **Broadcast payload** matches the JSON returned by the ingest endpoints.

This channel is useful for visualization clients that want push notifications
when new MRD files or SWMR frames arrive.

---

## Filesystem contract

Regardless of mode, clients can rely on the following metadata artefacts:

- `index.jsonl` — newline-delimited metadata describing all ingested MRDs.
- `latest.json` — JSON document mirroring the most recent ingest.

Live mode keeps these files in `data/mrd/`; record mode keeps them within the
active dumpbox session directory.

---

## Related documentation

- [`README.md`](../README.md) — conceptual overview and quick start.
- [`README_MODE_A.md`](../README_MODE_A.md) — full live-mode runbook.
- [`README_MODE_B.md`](../README_MODE_B.md) — full record→replay runbook.
- [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) — deeper dive into internal
  components.
- [`docs/USAGE_WITH_CLIENTS.md`](USAGE_WITH_CLIENTS.md) — walkthroughs that pair
  the marshal with bundled sample clients.
- [`docs/CLIENT_INTEGRATION.md`](CLIENT_INTEGRATION.md) — guidance for production
  scanners, FK publishers, and visualization consumers integrating with the
  marshal.
