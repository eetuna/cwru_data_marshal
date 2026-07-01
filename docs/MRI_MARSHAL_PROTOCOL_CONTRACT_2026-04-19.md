# MRI Marshal Protocol Contract — 2026-04-19 revision

Supersedes `MRI_MARSHAL_PROTOCOL_CONTRACT.md` for marshal behavior from fix/marshal-source-2026-04-18 forward. Same scope and structure as the original contract; clauses that change are the archival mode, the live file contents, `/image/latest` in dump mode, and the non-blocking + lossless retention guarantees.

## Primary Role

The MRI marshal is an ISMRMRD TCP peer/proxy. It must be compatible with scanners that use the ISMRMRD C library to pack messages and reconstruction services based on `python-ismrmrd-server`.

Scanner data does not use HTTP. Recon data does not use HTTP.

## Scanner to Marshal

The scanner connects to the marshal MRD TCP port and sends the same wire protocol used by `python-ismrmrd-server/connection.py`:

- `uint16_t` little-endian message ID
- message-specific body

Required message IDs:

- `1` CONFIG_FILE
- `2` CONFIG_TEXT
- `3` METADATA_XML
- `4` CLOSE
- `5` TEXT
- `1008` ISMRMRD ACQUISITION
- `1022` ISMRMRD IMAGE
- `1026` ISMRMRD WAVEFORM

The marshal forwards scanner MRD messages to recon over MRD TCP, except scanner-origin IMAGE as described below. Forwarding is not gated on the archival mode.

**Forwarding carve-out for scanner-origin IMAGE (1022).** Scanner-origin IMAGE is pre-reconstructed image data and is not useful input to a k-space reconstruction service. Marshal archives scanner IMAGE (to `live/from_scanner/` or `dump/from_scanner/` depending on mode) but does NOT forward it to recon. All other scanner messages (CONFIG_FILE, CONFIG_TEXT, METADATA_XML, TEXT, ACQUISITION, WAVEFORM, CLOSE) are forwarded to recon.

Scanner archival depends on the archival mode (see Archival Mode).

## Marshal to Recon

The marshal opens one MRD TCP connection to the configured recon service for a scan. With the scanner-IMAGE carve-out above, recon sees the MRD sequence it would see from a direct scanner/client connection.

Recon code should not need marshal-specific changes.

## Recon to Marshal to Scanner

Recon return messages arrive on the recon MRD TCP connection. The marshal pushes scanner-relevant return messages back to the scanner on the original scanner TCP connection unconditionally; pushback is not gated on the archival mode.

This return path is not optional for scanner operation. It is how reconstructed images, text/feedback, close notifications, and future MRD feedback messages reach the scanner.

Recon archival depends on the archival mode (see Archival Mode).

## Archival Mode

`--dump` selects the archival mode. Modes are mutually exclusive; marshal commits to one mode at process startup.

Both modes use the same storage model: during the scan, incoming wire bytes are written to a flat raw-MRD spool file (`scan_<ts>.h5.spool`). On scan close, the spool is converted to the canonical ISMRMRD HDF5 file (`scan_<ts>.h5`). The raw spool is retained next to the converted `.h5` by default so the byte-exact wire stream remains recoverable independent of the HDF5 converter.

### Live mode (default, no `--dump`)

- `live/from_scanner/scan_<ts>.h5` archives scanner-origin IMAGE (1022) and WAVEFORM (1026), including ECG.
- `live/from_reconstruction/scan_<ts>.h5` archives recon-returned IMAGE and WAVEFORM.
- Raw ACQUISITION (1008) is NOT archived in live mode.
- `latest_image.h5` snapshot is published atomically under `live/from_scanner/` and `live/from_reconstruction/` as images arrive.

### Dump mode (`--dump`)

- `dump/from_scanner/scan_<ts>.h5` archives the full scanner stream: ACQUISITION + IMAGE + WAVEFORM + TEXT + CONFIG_FILE + CONFIG_TEXT + METADATA_XML.
- `dump/from_reconstruction/scan_<ts>.h5` archives the full recon return stream: IMAGE + WAVEFORM + TEXT.
- No files are written under `live/`. `latest_image.h5` is not produced.

Within the active mode, scanner and recon archive files for a given scan share the same `<ts>`.

### Mid-scan readability

- `latest_image.h5` is the mid-scan readable interface for live clients. It is written by `LatestImageWriter` as an atomic closed-file snapshot (write temp, fsync, rename over the destination) and is safe to open concurrently from any reader at any time.
- `live/from_*/scan_<ts>.h5` and `dump/from_*/scan_<ts>.h5` are archival outputs, finalized after scan close. They are NOT guaranteed mid-scan readable; HDF5's default file locking holds an exclusive lock on the writer anyway, and with the spool-then-convert design the `.h5` does not exist until the converter runs at close.
- The retained `.spool` file is byte-exact of the wire stream and is present throughout the scan, but it is a private implementation format (length-prefixed MRD records) intended for post-scan recovery / offline conversion, not a stable client-facing interface.
- Future mid-scan-readable history (if required) should be added as either (a) HDF5 SWMR mode on the archival sink, or (b) additional periodic closed-file snapshots using the `latest_image.h5` pattern. The current design does not support it and should not be assumed to.

## Retention

Archival is lossless. Marshal persists every incoming message to the active archival sink until the scan closes. Marshal must never drop records due to queue pressure, rate limits, or internal flow-control decisions. Dropped-record counters in `/debug/sinks` are kept for visibility and must remain zero under any non-disk-failure operating condition.

The only acceptable fail-path is a hard disk-write failure (ENOSPC, EIO). In that case the drop counter increments, a one-shot error is logged, and `/debug/sinks` surfaces the error.

## Non-blocking Guarantee (archival)

Marshal must never stall, backpressure, or damage the scanner or recon session by virtue of its archival work. Archival writes run on worker threads that consume a per-lane queue; reader threads enqueue and return. The archival disk path is decoupled from the protocol forwarding path.

This guarantee is specifically about archival: HDF5 writes and conversion must never block scanner or recon. It does NOT prohibit TCP flow control on the protocol path itself (see Protocol Forwarding below), which is the transport-level mechanism every MRD peer — including `python-ismrmrd-server` — relies on.

## Protocol Forwarding

Scanner → recon and recon → scanner forwarding use blocking TCP sends. This matches the reference implementation (`python-ismrmrd-server/connection.py`) and the behavior of Gadgetron and every other MRD peer in the ecosystem. TCP flow control is the protocol-level backpressure mechanism:

- If the scanner's receive buffer fills (scanner is slow to read return messages), the marshal's send to the scanner blocks. That in turn blocks the recon reader thread on its next send, which applies backpressure across the recon TCP connection back to recon itself. Recon pauses producing until scanner catches up. No data is lost.
- Marshal does NOT maintain an in-process queue on the pushback path. A queue at this layer would force a choice between drop (contract violation) and unbounded growth (OOM), both of which TCP flow control already solves.
- Shutdown safety: when marshal's `stop()` closes the scanner socket, any in-flight blocking send returns EPIPE and unwinds cleanly.

"TCP flow control" and "marshal stalling scanner/recon" are distinct. TCP flow control is a transport-layer property of the connection; marshal stalling the peers would be marshal holding up forwarding for its own reasons (archival, bookkeeping, internal locks). The non-blocking guarantee above prohibits the latter; it does not and cannot prohibit the former.

## HTTP Side Channel

HTTP is only for non-scanner clients:

- `GET /image/latest`
- `GET/PUT /transform`
- `POST/GET /pose`
- `GET /dump/scanner`
- `GET /dump/recon`
- `GET /debug/sinks`
- `GET /health`

`GET /image/latest` in live mode returns a JSON file pointer. It does not inline the image bytes. Viz-style clients read the file from the shared filesystem. Before the current scan has published any live IMAGE, `GET /image/latest` returns `204 No Content`.

`GET /image/latest` in dump mode returns `404 Not Found` with body `{"error":"dump mode; no live snapshot"}`.

`GET /debug/sinks` returns per-sink counters (`acq`, `img`, `wf`) for all active sinks, last-closed counters so retention is observable after a sink closes, spool record / byte counters (during scan) and converted HDF5 counts (after close), queue depth + high-watermark hit flags, and dropped-record counters (which must remain zero under the retention guarantee, excluding the disk-failure exception above).

## Failure Behavior

The marshal process must remain up if recon fails. Scanner data continues to be accepted and archived to the active mode's sink. Recon connection state must be reset so a later scan can reconnect.

Recon failure is exposed in two ways:

- scanner clients receive a valid MRD `IMAGE(1022)` failure image on the existing scanner TCP connection (unconditional on archival mode).
- non-scanner clients see `GET /image/latest` return `error: true` and a failure PNG path in live mode; in dump mode `GET /image/latest` still returns `404`.
