# System Architecture

## Overview

The CWRU Data Marshal is a persistent intermediary between the MRI scanner and the reconstruction service. It forwards scanner MRD messages to recon, receives recon MRD return messages back, relays every recon return message to the scanner, always records canonical H5 archives of both sides, and serves query endpoints for visualization and control clients.

## Transport Map

```
                        MRD TCP (port 9100)                MRD TCP
    ┌─────────┐         2-byte tag framing         ┌─────────────────┐
    │ Scanner │ ═══════════════════════════════════>│                 │═════════════>┌──────────┐
    │ (client)│                                    │   MRI Marshal   │              │  Recon   │
    │         │<═══════════════════════════════════ │                 │<═════════════│ Service  │
    └─────────┘   MRD return messages              │                 │  MRD return └──────────┘
                  on same socket                   │                 │  messages
                                                   │   ┌─────────┐  │
                                                   │   │  HDF5   │  │
                                                   │   │ Archive │  │
                                                   │   └─────────┘  │
                                                   └────────┬───────┘
                                                            │
                                              HTTP (port 8080)
                                              query/control only
                                                            │
                              ┌──────────────────┬──────────┴──────────┬───────────┐
                              │                  │                    │           │
                         GET /image/latest   GET/PUT /transform  POST/GET /pose  GET /health
                              │                  │                    │           /dump/* /debug/*
                         ┌────┴─────────────────┴────────────────────┴────┐
                         │ webgl-client (:3000 UI, :3001 write-back)       │
                         │ + external consumers (HTTP GET/PUT/POST)        │
                         └────────────────────────────────────────────────┘
```

**Scanner → Marshal:** MRD TCP on `--mrd-port` (default 9100). The scanner opens a TCP connection per scan and sends MRD messages using python-ismrmrd-server's 2-byte message ID framing. A second scanner connection is not rejected: it waits its turn and is served when the current scan ends.

**Marshal → Recon:** MRD TCP to `--recon-host:--recon-port` (`--recon-host` is required with `--mrd-port`). As soon as the scanner connects, the marshal opens its own connection to the recon service and forwards scanner messages using the same framing. Recon sends return messages back on this connection.

**Marshal → Scanner (return path):** Recon return messages are pushed back to the scanner on the SAME TCP socket the scanner connected on. This satisfies the requirement: "Marshall needs to push, and it must come over the existing connection that was established for the scan."

**Marshal → slice_agent (slice control, optional):** the marshal keeps six absolute numbers (`tx ty tz` mm, `rx ry rz` deg, zero at scan start) exactly like Andrew's `slice_control` tool; UI slice commands (`/write/file_slice_translation`, `/write/slice_delta`) add to them and `/write/slice_target` replaces them; after each change the six totals are sent as a 56-byte `SliceCommand` over a separate TCP connection to the scanner-side `slice_agent --listen` on `--slice-agent-host:9270` (the MARS), which publishes them to shared memory for the sequence. Off unless `--slice-agent-host` is set. See [SLICE_CONTROL_HANDOFF.md](SLICE_CONTROL_HANDOFF.md).

**Query/Control clients → Marshal:** HTTP on `--http` (default 0.0.0.0:8080). The
webgl-client and any external consumer use it: `GET /status`, `GET /image/latest`,
`GET /image/latest.h5`, the slice-command endpoints
(`/write|read/file_slice_translation`, `/write|read/slice_delta`,
`/write|read/slice_target`, `GET /read/slice_geometry`), `GET/PUT /transform`,
`POST/GET /pose`, `GET /health`, `GET /dump/scanner`, `GET /dump/recon`, and
`/debug/*` diagnostics. Authoritative list: [API_REFERENCE.md](API_REFERENCE.md).

**Image client (webgl-client):** Polls `GET /image/latest` over HTTP, receives
`{path, images, generation, status, status_detail}` (always `200`; `path` is `null`
until the current scan has an image) pointing at a closed companion HDF5 file, and opens
it with default ISMRMRD / h5py settings. Marshal atomically renames a new companion
snapshot on each change, so readers always see a closed file. The viewer uses the address
the page was opened from, so it works from other machines, and shows the marshal's status
in words while there is no image.

## MRD TCP Wire Protocol

The MRD TCP wire protocol is defined by python-ismrmrd-server (`connection.py`, `constants.py`). Every message starts with a 2-byte little-endian message ID, followed by a type-specific payload.

### Message Types

| ID | Name | Payload |
|---|---|---|
| 1 | CONFIG_FILE | 1024 bytes, null-padded config filename |
| 2 | CONFIG_TEXT | 4-byte LE length + null-terminated text |
| 3 | METADATA_XML_TEXT | 4-byte LE length + null-terminated XML |
| 4 | CLOSE | (no payload) |
| 5 | TEXT | 4-byte LE length + null-terminated text |
| 1008 | ISMRMRD_ACQUISITION | 340-byte AcquisitionHeader + trajectory floats + complex samples |
| 1022 | ISMRMRD_IMAGE | 198-byte ImageHeader + 8-byte LE uint64 attribute_len + attribute string + pixel data |
| 1026 | ISMRMRD_WAVEFORM | 40-byte WaveformHeader + uint32 samples |

### Struct Sizes (from libismrmrd `static_assert`)

- `ISMRMRD_AcquisitionHeader` = 340 bytes
- `ISMRMRD_ImageHeader` = 198 bytes
- `ISMRMRD_WaveformHeader` = 40 bytes (NOT 240 — python-ismrmrd-server `connection.py:398` comment is wrong)

### Acquisition Variable Payload

```
trajectory: trajectory_dimensions × number_of_samples × sizeof(float) bytes
samples:    number_of_samples × active_channels × sizeof(complex<float>) bytes
```

### Image Variable Payload

```
attribute_string_len: 8-byte LE uint64
attribute_string:     attribute_string_len bytes
pixel_data:           matrix_size[0] × matrix_size[1] × matrix_size[2] × channels × itemsize(data_type) bytes
```

### Waveform Variable Payload

```
samples: number_of_samples × channels × sizeof(uint32) bytes
```

### Typical Session Flow

```
Scanner connects to marshal MRD TCP port
  → CONFIG_FILE(1) or CONFIG_TEXT(2)     select recon handler
  → METADATA_XML_TEXT(3)                 ISMRMRD XML header
  → ACQUISITION(1008) × N               k-space lines
  → IMAGE(1022) × K (optional)          scanner-made images (also forwarded)
  → WAVEFORM(1026) × M (optional)       ECG / physio (waveform_id=0 = ECG)
  → CLOSE(4)                            end of scan
  ← IMAGE/TEXT/etc.                     recon return messages pushed back
  ← CLOSE(4)                            recon done
```

## Routing and Behavioral Guarantees

These guarantees define how marshal routes messages and how it protects the
scanner/recon path. They are contractual — clients and recon depend on them.

**Pass-through, like python-ismrmrd-server.** One scan per scanner connection. When
the scanner connects, the marshal immediately opens its own recon connection. Every
scanner message (config, metadata XML, text, acquisitions, images, waveforms) is
archived and forwarded to recon verbatim and in order — scanner IMAGE(1022) messages
included. Every recon message (images, text, waveforms, close) is archived and relayed
back to the scanner.

**Scan-completion CLOSE.** On the scanner's CLOSE(4) the marshal forwards CLOSE to
recon, then waits — with no time limit — until recon sends its CLOSE or its connection
ends, relays it, and closes the scanner connection. A slow recon is simply waited for;
a dead recon host is detected by TCP keepalive. If recon cannot be reached or its
connection fails, the scanner gets a `RECON FAILED` IMAGE(1022) and a CLOSE(4), so the
scan still finalizes.

**Archival never blocks the protocol path.** HDF5 writes and the spool→H5 conversion
run off the scanner/recon forwarding path and must never stall it. TCP flow control on
the MRD sockets is the **only** backpressure that applies to the protocol path;
archival I/O pressure is absorbed independently and cannot throttle the scanner or
recon.

## Docker Compose Topology

There is a single `docker-compose.yml`; settings live in `.env` (template `.env.example`
in the repo root). Env knobs: `MARSHAL_LATEST` (default `--latest-dir /latest` = RAM
snapshot on host `/dev/shm/cwru-latest`; set empty for the on-disk snapshot
under `session-data/live/`), `RECON_HOST`/`RECON_PORT` (recon target; `RECON_HOST` has
no default — compose stops with `RECON_HOST is not set - copy .env.example to .env and
set it`), `SLICE_AGENT_HOST`/`SLICE_AGENT_PORT`, `SESSION_DATA_DIR` (data path), and the
exposed ports (`HTTP_PORT`/`MRD_PORT`/`ROBOT_PORT`/`UI_PORT`/`WRITE_PORT`). The bundled
test recon is off by default and enabled with `RECON_HOST=recon` in `.env` plus
`--profile test-recon`. `mri-marshal` and `robot-marshal` have `restart: unless-stopped`.
The marshal also writes its log to `session-data/marshal.log` (`MARSHAL_LOG_FILE`), and
image builds bake its version (`MARSHAL_VERSION` = `<branch> @ <commit>`) into the log
and `/status`. The scanner is **not** a
compose service — it is external and drives `mri-marshal:9100` over raw MRD TCP (a real
scanner, or python-ismrmrd-server's `client.py`).

```
docker-compose.yml services (network cwru-demo-net):

  mri-marshal     marshal --http 0.0.0.0:8080 --mrd-port 9100 --dump-dir /session-data
                          $MARSHAL_LATEST --recon-host $RECON_HOST --recon-port $RECON_PORT
                  ports 8080 (HTTP API) + 9100 (MRD TCP, scanner-facing);
                  volume $SESSION_DATA_DIR -> /session-data
  recon           fire-python: python-ismrmrd-server main.py, MRD TCP :9002,
                  --defaultConfig=invertcontrast  (profile: test-recon, off by default)
  robot-marshal   HTTP server on port 8081 (independent of the MRI path)
  catheter-tracking, force-sensor, controller, planning, front-end,
  surface-tracking   robot data producers → robot-marshal:8081
  webgl-client    UI on port 3000 + write-back server on 3001;
                  reads mri-marshal:8080 (/image/latest) and robot-marshal:8081

  # External (not a service): scanner → mri-marshal:9100 over raw MRD TCP, e.g.
  #   client.py -c invertcontrast --address mri-marshal --port 9100 phantom.h5
```

There is one mode: the marshal always archives both sides in full and always serves
the viewer snapshot (see Storage Layout).

## Storage Layout

There is one mode: everything from the scanner and from recon is always archived under `dump/`, and the viewer snapshot is always on. There is no `live/` archive.

Archives use a raw-MRD spool + post-scan HDF5 convert model: incoming wire frames are written to `scan_<ts>.h5.spool` during the scan; after the scan the spool is converted in the background to the canonical `scan_<ts>.h5`, so the next scan never waits for it. The `.spool` file is deleted after a successful conversion and kept if conversion fails, for recovery.

```
${dump_dir}/                          session-data umbrella (--dump-dir flag)
│
├── marshal.log                       marshal log (MARSHAL_LOG_FILE)
└── dump/                             always written
    ├── from_scanner/
    │   ├── scan_<ts>.h5.spool        raw MRD wire frames (full stream incl. ACQ)
    │   └── scan_<ts>.h5              ISMRMRD HDF5, converted in background after the scan
    └── from_reconstruction/
        ├── scan_<ts>.h5.spool
        └── scan_<ts>.h5

${latest_dir}/                        snapshot root (--latest-dir; compose default
│                                     /latest = RAM-backed /dev/shm/cwru-latest.
│                                     With MARSHAL_LATEST= unset-to-empty, these
│                                     files live under ${dump_dir}/live/ instead.)
└── live/
    └── from_reconstruction/
        └── latest_image.h5           closed viewer snapshot, atomic-rename per change
```

- `<ts>` is shared between scanner and recon lanes.
- `scan_<ts>.h5` archives the full stream of its side — acquisitions + images + waveforms + text + config.
- `latest_image.h5` holds the newest recon image for each slice position (position + slice orientation + image type from the image header). A new image at a position replaces the old one; a new scan (its metadata header) clears the set. No volume or slice-count guessing. Images of different sizes go into separate variables `image_0`, `image_1`, ...; magnitude and phase at the same position are both kept.
- HDF5 files use canonical libismrmrd layout (`appendAcquisition`, `appendImage`, `appendWaveform`).
- The per-scan `scan_<ts>.h5` is an archival output finalized on CLOSE. It is NOT a mid-scan readable interface (HDF5's default file locking would block mid-scan opens on an open writer anyway). Clients that need mid-scan reads use `latest_image.h5`.
- `GET /image/latest`: always `200` with `{path, images, generation, status, status_detail}`; `path` is `null` until the current scan has an image. Readers open the companion file's `image_0` (and `image_1`, ...) group. `GET /image/latest.h5`: `204` while there is no image.
- The `.spool` file is present throughout the scan; it is a private format (`[uint16 tag][uint32 length][body]` per record) intended for post-scan replay, not a client-facing interface.

## Fault Tolerance

The marshal stays running regardless of recon failure:

- Recon return reading runs on a background thread.
- At startup the marshal tries the recon address once and logs `Recon host:port is reachable` or `is NOT reachable: <reason> — scans will fail until it is` (also in `/status` as `recon.startup_check`).
- If recon is unreachable or its connection fails, scanner-side archival continues and the next scan opens a fresh recon connection.
- A `RECON FAILED` MRD `IMAGE(1022)` (geometry copied from the scan's raw data, int16 pixels) and a CLOSE are pushed on the active scanner TCP connection, so the scan finalizes.
- `/image/latest` reports `status: "recon failed"` with the reason in `status_detail`, so the webgl-client shows it in words
- T4 test: kill recon mid-scan, assert marshal still accepts MRD TCP connections and GET /health returns 200

`--recon-host` is required when `--mrd-port` is used; the marshal refuses to start without it.

## WebSocket (Optional)

The marshal has an optional WebSocket listener (`--ws-port`). It is always compiled into the binary but only activated when the flag is passed. The WS interface broadcasts JSON event notifications. It is NOT used for scanner data transport (that's MRD TCP) or for image delivery (that's file-based via GET /image/latest).
