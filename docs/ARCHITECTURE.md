# Architecture Overview

CWRU Data Marshal is a single-process hub that accepts reconstructed MRI data
and pose updates over HTTP/WebSocket, persists them on disk, and fans metadata
out to connected clients. The core components are:

- **HTTP server (`HttpServer`)** — Handles REST endpoints for MRD ingest,
  SWMR frame streaming, pose updates, and metadata queries.
- **WebSocket server (`WsServer`)** — Broadcasts ingest and pose events to
  subscribed clients.
- **Storage helpers (`mrd_io.hpp`)** — Provide atomic file writes, index and
  metadata maintenance, and sink selection logic for MRD vs. dumpbox modes.
- **SWMR manager (`SwmrManager`)** — Manages extendible HDF5 datasets that stay
  open while new frames arrive.

## Request flow

1. **Ingesting full MRDs** (`POST /v1/mrd/ingest`)
   - The HTTP handler forwards the payload to `mrd::ingest_payload()` which
     writes the file atomically into either `/data/mrd` (live) or the active
     dumpbox session, updates `index.jsonl`/`latest.json`, and emits a WebSocket
     notification.
2. **Streaming frames** (`POST /v1/mrd/frame`)
   - The handler validates stream headers, then asks `SwmrManager` to append the
     frame into an HDF5 dataset. `SwmrManager` ensures one open file per stream
     and uses `SwmrFile` to extend the `/frames` dataset via HDF5 SWMR APIs.
   - Each append logs into the same index files as full MRD ingests so clients
     have a unified view of activity.
3. **Poses and telemetry**
   - Pose updates update the in-memory `PoseStore` and immediately broadcast.

## SWMR file layout

Each SWMR stream creates a file named
`<timestamp>_<sanitized-stream>.mrd` inside the active sink. The file contains a
single dataset `/frames` with dimensions `[frames, channels, z, y, x]` and
attributes describing voxel type and geometry. Writers call `H5Fstart_swmr_write`
once at creation and `H5Dflush`/`H5Fflush` after each frame so SWMR readers can
see consistent snapshots without waiting for the file to close.

`viz_client` demonstrates the reader side by opening the MRD file with
`H5F_ACC_SWMR_READ` whenever `index.jsonl` advertises a new frame. Because the
same index files record both full ingests and SWMR frames, existing tailing
clients continue to work.
