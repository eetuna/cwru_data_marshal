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

The marshal forwards scanner MRD messages to recon over MRD TCP unconditionally; forwarding is not gated on the archival mode.

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

## Retention

Archival is lossless. Marshal must persist every incoming message to the active archival sink until the scan closes. Marshal must never drop records.

## Non-blocking Guarantee

Marshal must never stall, backpressure, or damage the scanner or recon session by virtue of its archival work. The scanner → recon forwarding path and the recon → scanner return path are never held on archival writes. Archival is asynchronous: reader threads enqueue and return; worker threads drain into HDF5.

The non-blocking guarantee is paired with the retention guarantee: marshal must not satisfy either by violating the other. If marshal cannot sustain the incoming rate losslessly without blocking the protocol, that is a marshal defect (larger queue, faster writer, lower-overhead format) — not an operating condition.

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

`GET /debug/sinks` returns per-sink counters (`acq`, `img`, `wf`) for all active sinks, last-closed counters so retention is observable after a sink closes, and dump drop counters (which must remain zero under the retention guarantee).

## Failure Behavior

The marshal process must remain up if recon fails. Scanner data continues to be accepted and archived to the active mode's sink. Recon connection state must be reset so a later scan can reconnect.

Recon failure is exposed in two ways:

- scanner clients receive a valid MRD `IMAGE(1022)` failure image on the existing scanner TCP connection (unconditional on archival mode).
- non-scanner clients see `GET /image/latest` return `error: true` and a failure PNG path in live mode; in dump mode `GET /image/latest` still returns `404`.
