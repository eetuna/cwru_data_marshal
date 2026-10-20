# Reconstruction Service Interface

This document describes the MRD TCP interface between the MRI Marshal and reconstruction services.

## Transport

Marshal is the **client** on the recon leg. It opens an MRD TCP connection to the
recon server as soon as a scanner connects, forwards everything the scanner sends,
and reads the recon's messages back on the *same* connection. The wire protocol is **raw TCP** with python-ismrmrd-server's 2-byte
message-ID framing — the same protocol the scanner uses, so the recon server is
agnostic to whether a scanner or the marshal is driving it.

Configure via: `--recon-host <host> --recon-port <port>` (`--recon-host` is
required with `--mrd-port`; the marshal refuses to start without it). In the stack
(`docker-compose.yml`) these come from `RECON_HOST` / `RECON_PORT` in `.env` —
`RECON_HOST` has no default; set `RECON_HOST=recon` and use `--profile test-recon`
for the bundled `recon` service. At startup the marshal tries the address once and
logs whether it is reachable. Both directions are always archived as H5.

## Data Flow

```
MRI Marshal                         Recon Service
    │                                     │
    │── CONFIG_FILE(1) ──────────────────>│  select handler (e.g. "invertcontrast")
    │── METADATA_XML(3) ─────────────────>│  ISMRMRD XML header
    │── ACQUISITION(1008) × N ───────────>│  k-space lines
    │── IMAGE(1022) × K (optional) ──────>│  scanner-sent images
    │── WAVEFORM(1026) × M (optional) ──>│  ECG / physio data
    │── CLOSE(4) ────────────────────────>│  end of scan
    │                                     │
    │<── IMAGE(1022)/TEXT(5)/etc. ────────│  recon return messages
    │<── CLOSE(4) ────────────────────────│  recon done
    │                                     │
```

**Routing.** Pass-through, like python-ismrmrd-server: one recon connection per
scanner connection (one scan), opened when the scanner connects. Every scanner
message (config, metadata XML, text, acquisitions, images, waveforms) is forwarded
verbatim and in order — scanner-sent IMAGE(1022) included. Every recon message
(images, text, waveforms, close) is relayed back to the scanner; recon images also
feed the viewer snapshot. On the scanner's CLOSE the marshal forwards CLOSE and
waits with no time limit for the recon's CLOSE (or the end of its connection),
relays it, and closes the scanner connection — a slow recon is simply waited for.

**Image geometry.** Every IMAGE(1022) carries its spatial pose in the ISMRMRD
`ImageHeader`: `position[3]` (slice center in scanner coordinates, mm) and the
direction cosines `read_dir[3]`, `phase_dir[3]`, `slice_dir[3]` (with
`field_of_view[3]` and `patient_table_position[3]`). Together these fix where the
slice sits and how it is oriented. Downstream consumers use them to place the image
in 3D; the marshal serves the latest one at `GET /read/slice_geometry` and the UI can
feed it back as an absolute prescription (`/write/slice_target`). Relative slice moves
do **not** start from it — they accumulate from zero like Andrew's `slice_control`
(see [SLICE_CONTROL_HANDOFF.md](SLICE_CONTROL_HANDOFF.md)).

## Recon service contract

The recon service is a TCP server that:

1. **Listens** on a port (e.g. 9002).
2. **Accepts** one TCP connection per scan.
3. **Reads** messages using the 2-byte tag framing:
   - CONFIG_FILE(1) or CONFIG_TEXT(2) — selects the reconstruction handler
   - METADATA_XML_TEXT(3) — ISMRMRD XML header describing the scan
   - ACQUISITION(1008) — one k-space line (340B header + trajectory + samples)
   - IMAGE(1022) — scanner-sent images, if any
   - WAVEFORM(1026) — physio data (40B header + uint32 samples), used for cardiac gating
   - CLOSE(4) — end of input
4. **Processes** the acquisitions (e.g. 2D IFFT, GRAPPA).
5. **Writes** return messages back on the SAME TCP connection:
   - IMAGE(1022) — 198B ImageHeader + 8B uint64 attribute_len + attribute string + pixel data
   - TEXT(5) — status/feedback text
   - Other known MRD message types when needed by the recon workflow
   - CLOSE(4) — end of output

This is identical to python-ismrmrd-server's `Connection` class in `connection.py`.

## Using / swapping the reconstruction service

The default stack runs **python-ismrmrd-server** as the recon server — the `recon`
service in `docker-compose.yml` uses the `fire-python` image and runs:

```
python3 /opt/code/python-ismrmrd-server/main.py -v -H=0.0.0.0 -p=9002 --defaultConfig=invertcontrast
```

To swap in a different reconstruction, point marshal's `--recon-host` /
`--recon-port` at **any MRD-TCP recon server** that speaks the 2-byte-tag wire
protocol described above — python-ismrmrd-server, Gadgetron, or your own server.
Nothing in the marshal is coupled to a specific recon implementation.

Options, in increasing order of effort:

1. **Keep python-ismrmrd-server, change the config.** Change `--defaultConfig` (or
   have the scanner send a `CONFIG_TEXT` naming another handler) to select a
   different `process(connection, config, metadata)` handler module.
2. **Point at Gadgetron or another MRD-TCP server.** Set `--recon-host` /
   `--recon-port` to its address. As long as it reads ACQUISITION/CONFIG/METADATA
   and writes IMAGE(1022)/CLOSE back on the same connection, marshal is happy.
3. **Write your own MRD-TCP server** honoring the contract below.

## Fault tolerance

If the recon service is unreachable or crashes mid-scan:

- The marshal's recon forwarder marks the recon connection disconnected and logs a warning
- The marshal keeps running and keeps archiving the scanner data
- The scanner gets a `RECON FAILED` MRD `IMAGE(1022)` (geometry copied from the scan's raw data, int16 pixels) and a CLOSE, so the scan finalizes
- `GET /image/latest` returns `"status": "recon failed"` with the reason in `status_detail` (e.g. `"cannot connect to recon at host:port"`, `"recon connection ended without CLOSE"`)
- The webgl-client shows that status in words
- The next scanner session opens a fresh recon connection; a dead recon host is detected by TCP keepalive

No special error handling is needed on the recon side — TCP disconnection is sufficient.

## Storage

- Always: `${dump_dir}/dump/from_scanner/scan_<ts>.h5` and `${dump_dir}/dump/from_reconstruction/scan_<ts>.h5` (canonical ISMRMRD HDF5, one per scan per side, converted from a spool in the background after the scan). There is no `live/` archive.
- Live clients read the closed companion file at the path returned by `GET /image/latest` (`null` until the scan has an image). It holds the newest recon image per slice position; images of different sizes are in `image_0`, `image_1`, ...
