# Client Integration Guide

This document summarizes how first-party or custom clients should interact with the marshal once
it is deployed alongside production scanners and visualization endpoints. It reflects the current
behavior of the optimized streamer, marshal, and visualization clients introduced in this change
set.

## 1. Streaming reconstructed images from a scanner

* **Transport.** Use persistent HTTP/1.1 connections to `POST /v1/ismrmrd/frame`. Avoid reconnecting
  between frames; reuse the socket and keep `Connection: keep-alive` semantics, just like the
  refreshed `clients/image_streamer` does. If the connection breaks, reconnect and resend the frame
  that was in-flight.
* **Cadence control.** Maintain your own frame cadence (20–30 fps is now realistic) by sleeping only
  for the time needed to hit the target deadline. The marshal no longer throttles; it accepts frames
  as quickly as your producer can deliver them.
* **Payload shape.** Keep geometry (`nx × ny × nslices × channels`) stable while a stream is active.
  Changing it forces the marshal to roll to a new MRD file. Use the smallest datatype that preserves
  fidelity; `uint16` halves the write bandwidth versus `float32`.
* **Flush policy awareness.** The marshal coalesces HDF5 flushes. Configure it with
  `--flush-max-frames <N>` and `--flush-max-ms <milliseconds>` (defaults: `4` and `50`). Every HTTP
  response and broadcast JSON now carries a `flushed` boolean. When it is `false`, the frame is
  durably written but not yet visible to SWMR readers; keep publishing but expect downstream readers
  to wait for the next `flushed: true` notification before opening the dataset.
* **Headers.** Supply `X-MRD-Stream` per logical stream and ensure the ISMRMRD header accurately
  reflects voxel dimensions, datatype, and channel count.

## 2. Building visualization clients

* **Discovery.** Tail `data/mrd/index.jsonl` for history and `data/mrd/latest.json` for the most
  recent update. Both files now include `flushed`, `frame_index`, voxel dimensions, datatype, and the
  target MRD path. Use that metadata to decide when to refresh.
* **Push updates.** Subscribe to the marshal WebSocket (`/ws`). Each ingest triggers the same JSON
  broadcast that lands in the index files. Ignore entries with `"flushed": false`; they signal that
  a flush is pending. When `flushed` becomes true, call `H5Drefresh`/`H5Frefresh` on your cached
  handles to see the new frame.
* **Reading MRD data.** Open the stream once using `H5F_ACC_SWMR_READ`, cache the dataset handle, and
  reuse it. Select the hyperslab `[frame, channel, z, y, x]` for the frame you need and let HDF5 cast
  to `float32` if necessary. The sample viz client now does this with a background thread and reuses
  buffers to keep up with 20–30 fps streams.
* **Rendering hints.** Apply lightweight reductions (e.g., maximum intensity projection) on the GPU
  or in tight loops that operate on contiguous buffers. The bundled visualization client now
  collapses slices using a single pass over contiguous memory and converts to 8-bit textures with
  OpenCV to remove per-voxel branching.

## 3. Working with the FK client

* **Purpose.** `clients/fk_client` demonstrates how an external motion source can publish pose
  updates to `/v1/pose/update`. The marshal stores the latest pose and rebroadcasts it over the
  WebSocket.
* **Consuming poses.** Visualization clients should listen for `{ "type": "pose", ... }` messages on
  `/ws`. The sample viz client draws a trail using these updates. Custom consumers can apply them to
  their own overlays or registration pipelines.
* **Publishing cadence.** Keep pose updates lightweight (tens of Hz). The marshal is stateless for
  poses—it stores the last report and rebroadcasts it, so redundant updates are cheap.

## 4. Forwarding MRD data from FK into the viz stack

If an FK process needs to forward images (for example, post-processed ROIs) to the viz client,
follow the same pattern as scanner ingest:

1. Publish images via `POST /v1/ismrmrd/frame` using a dedicated `X-MRD-Stream` name.
2. Allow the marshal to fan out notifications (`latest.json`, `index.jsonl`, WebSocket).
3. Have the viz client subscribe to the FK stream’s entries (filter by `stream` in the metadata) and
   use the cached SWMR handles to pull the ROIs alongside the scanner stream.

This keeps all transports unified—FK never talks to viz directly; both interact with the marshal.

## 5. General guidelines for future clients

* Prefer persistent transports (HTTP keep-alive, WebSocket) over connect-per-frame patterns.
* Respect the `flushed` flag before attempting to read; doing so avoids HDF5 refresh churn.
* Reuse file and dataset handles on the reader side. `H5Fopen`/`H5Dopen` per frame does not scale to
  high frame rates.
* Avoid excessive logging inside high-rate loops. The sample clients now log at most once per second
  (every 30 frames) to keep stdout from becoming the bottleneck.
* When extending the marshal, update `docs/API_REFERENCE.md` and this guide so downstream teams know
  about new metadata fields, CLI flags, or handshake requirements.

## 6. Flush semantics and real-time robotics considerations

### Why the marshal flushes

The marshal persists every frame append into an HDF5 SWMR dataset. Flushing (`H5Dflush` followed by
`H5Fflush`) forces those writes to land on durable storage and makes the new hyperslabs visible to
SWMR readers such as visualization or analytics clients. Without a flush, readers that already have
the dataset open would continue to see the previous frame until their next refresh cycle discovers
the committed data.

### Single-frame flushes (legacy behavior)

Earlier revisions flushed after **every** accepted frame. That ensured near-immediate visibility for
any client polling the dataset—useful when low-latency preview trumped throughput. The trade-off was
that each frame incurred a disk sync, so high-rate streams (20–30 fps) quickly saturated I/O and the
HTTP handler stalled while waiting for storage to acknowledge the write.

### Batched flushes (current behavior)

The marshal now coalesces up to `--flush-max-frames` or `--flush-max-ms` worth of frames before
invoking `H5Dflush`/`H5Fflush`. This reduces synchronous I/O pressure and leaves more budget for the
producer and viz clients at 20–30 fps. The HTTP response (and accompanying WebSocket/index records)
includes `"flushed": false` when a frame is still buffered. Downstream readers wait until a
subsequent notification with `"flushed": true` before attempting to refresh their SWMR handles.

### Relevance to robotic surgery workflows

Robotic control loops often couple multiple clients—scanner-side reconstruction, visualization,
instrument tracking, and autonomy controllers—all pulling through the marshal. Batching improves
throughput so image feeds stay current, but it also introduces a bounded delay (up to the configured
frame or time window) before fresh data become visible. In a tele-surgical scenario that expects
immediate feedback for visual servoing, you may prefer the legacy single-flush mode
(`--flush-max-frames 1 --flush-max-ms 0`) so each frame becomes readable as soon as it is written,
accepting higher disk utilization. Conversely, supervisory visualization displays can tolerate the
small buffering window and benefit from the smoother throughput that batching provides.

### Why there appear to be “two ingestion architectures”

The marshal exposes **one** append path for SWMR datasets—`POST /v1/ismrmrd/frame` over HTTP—and a
separate WebSocket API that handles fan-out notifications plus whole-file uploads via
`mrd::ingest_payload`. The latter path exists for bulk transfers (for example, uploading an MRD file
from archival storage) and never appends into a live SWMR dataset. From a distance this looks like
two ingestion architectures, but only the HTTP route feeds the rolling SWMR streams that viz clients
tail. WebSockets are reserved for metadata and coarse-grained file ingress.

### Why ingestion stays on HTTP

The SWMR append path depends on HTTP request framing: the marshal validates headers, streams the
payload into the dataset, and immediately returns flush metadata. Reusing an HTTP/1.1 keep-alive
socket gives the producer a persistent channel without designing a new protocol. Moving SWMR ingest
to WebSocket would require defining record boundaries, replay semantics, congestion control, and
explicit flush acknowledgements—features already supplied by HTTP plus the existing request/response
handlers—so HTTP persists as the ingestion transport even after the batching change.

### Scenario for WebSocket ingestion

`mrd::ingest_payload` over WebSocket is intended for coarse bursts: e.g., an FK process replaying a
completed scan, or a QA workstation pushing a prepared MRD file into the marshal for later review.
The entire file is uploaded, written atomically, and advertised through the same index/metadata
channel. Because the upload is whole-file based, it bypasses SWMR append semantics and therefore does
not participate in real-time frame streaming. Clients expecting low-latency updates should continue
using the HTTP append path.

Why keep this WebSocket path at all? It shines when you already have a complete MRD artifact and only
need a single connection to shuttle it into the marshal alongside the metadata fan-out you are
already subscribed to. Compared with HTTP multipart uploads, the WS flow avoids re-authenticating or
re-establishing TLS per file, lets you replay a sequence of studies over one session (handy for QA or
simulation rigs), and plays nicely with browser-based tooling that cannot open raw TCP sockets. In
short: use HTTP appends for incremental, low-latency frames; use the WebSocket uploader when you want
a convenient “dropbox” for whole acquisitions that can ride on the same persistent channel your tools
already keep open for notifications.

### Would WebSocket ingestion be faster?

WebSockets could eliminate per-request HTTP headers, but the dominant cost in the current pipeline is
durable storage, not HTTP parsing. Unless WebSocket ingestion also relaxed the flush guarantees, it
would still block on the same `H5Fflush` calls. The persistent HTTP/1.1 connections used by the
updated streamer already amortize handshake costs, so there is little benefit to adding a parallel
WebSocket ingestion path just for throughput.

### Would HTTP/2 or HTTP/3 help?

Higher versions of HTTP multiplex requests and, in the case of HTTP/3, run over QUIC instead of TCP.
Those improvements mainly reduce head-of-line blocking and connection setup costs. Because the marshal
already reuses a single HTTP/1.1 keep-alive socket per producer, the gains from upgrading the
transport are marginal: the request body still has to be delivered in order, and the response must wait
for the same synchronous flush. If you operate over lossy links or need multiple concurrent streams on
one connection, HTTP/2 or HTTP/3 could simplify flow control, but they do not make the HDF5 flushes
complete faster. Disk durability remains the limiting factor.

### Choosing between single and batched flushes

* **Single-flush mode**: best when deterministic latency is paramount—for example, when a robotic arm
  uses the latest frame to update tool trajectories at video rates. Expect higher disk pressure and
  plan for NVMe-class storage to keep up.
* **Batched mode**: best for diagnostic review, guidance overlays, or analytics pipelines that favor
  sustained throughput over sub-frame latency. Monitor the `flushed` flag so consumers know when new
  data are visible and design control logic to handle the additional buffering.

In both modes, keep producer-side retries and reader-side refresh logic resilient: a temporarily slow
flush manifests as longer HTTP response times and delayed `flushed=true` notifications, so clients
should back off gracefully rather than assuming data loss.
