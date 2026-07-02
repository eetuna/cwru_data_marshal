# System Architecture

## Overview

The CWRU Data Marshal is a persistent intermediary between the MRI scanner and the reconstruction service. It forwards scanner MRD messages to recon, receives recon MRD return messages back, pushes scanner-relevant return messages to the scanner, optionally records canonical H5 dumps, and serves query endpoints for visualization and control clients.

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

**Scanner → Marshal:** MRD TCP on `--mrd-port` (default 9100). The scanner opens a persistent TCP connection and sends MRD messages using python-ismrmrd-server's 2-byte message ID framing.

**Marshal → Recon:** MRD TCP to `--recon-host:--recon-port`. The marshal opens a TCP connection to the recon service and forwards scanner messages using the same framing. Recon sends return messages back on this connection.

**Marshal → Scanner (return path):** Recon return messages are pushed back to the scanner on the SAME TCP socket the scanner connected on. This satisfies the requirement: "Marshall needs to push, and it must come over the existing connection that was established for the scan."

**Query/Control clients → Marshal:** HTTP on `--http` (default 0.0.0.0:8080). The
webgl-client and any external consumer use it: `GET /image/latest`,
`GET/PUT /transform`, `POST/GET /pose`, `GET /health`, `GET /dump/scanner`,
`GET /dump/recon`, and `/debug/*` diagnostics.

**Image client (webgl-client):** Polls `GET /image/latest` over HTTP, receives
`{path, error}` pointing at a closed companion HDF5 file, and opens it with default
ISMRMRD / h5py settings. Marshal atomically renames a new companion snapshot on each
incoming live IMAGE, so readers always see a closed file. `GET /image/latest` returns
`204 No Content` before the first IMAGE, and `404 Not Found` in dump mode.

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
  → WAVEFORM(1026) × M (optional)       ECG / physio (waveform_id=0 = ECG)
  → CLOSE(4)                            end of scan
  ← IMAGE/TEXT/etc.                     recon return messages pushed back
  ← CLOSE(4)                            recon done
```

## Routing and Behavioral Guarantees

These guarantees define how marshal routes messages and how it protects the
scanner/recon path. They are contractual — clients and recon depend on them.

**Recon is engaged only for k-space.** Marshal opens a recon session lazily: the
recon connection is established when the **first ACQUISITION(1008) arrives**.
CONFIG_FILE/CONFIG_TEXT, METADATA_XML_TEXT, TEXT, and WAVEFORM are buffered until
then (and replayed into the session once it opens). A scanner-sent IMAGE(1022) is
**not** sent to recon — it is published directly to the live lane / `latest_image.h5`
snapshot. This means an image/metadata-only scan never touches recon.

**Scan-completion CLOSE.** On end of scan:
- **k-space scan** (recon session was opened): recon's CLOSE(4) is relayed back to
  the scanner, so the scanner learns the scan is done from recon.
- **image/metadata-only scan** (no recon session): marshal emits **its own** CLOSE(4)
  to the scanner so it is never left waiting on a recon that was never engaged.

**Archival never blocks the protocol path.** HDF5 writes and the spool→H5 conversion
run off the scanner/recon forwarding path and must never stall it. TCP flow control on
the MRD sockets is the **only** backpressure that applies to the protocol path;
archival I/O pressure is absorbed independently and cannot throttle the scanner or
recon.

## Docker Compose Topology

There is a single `docker-compose.yml`. Env knobs: `MARSHAL_DUMP` (`""` = live,
`--dump` = dump/archival), `RECON_HOST`/`RECON_PORT` (recon target, default
`recon:9002`), `SESSION_DATA_DIR` (data path), and the exposed ports
(`HTTP_PORT`/`MRD_PORT`/`ROBOT_PORT`/`UI_PORT`/`WRITE_PORT`). The bundled test recon is
off by default and enabled with `--profile test-recon`. The scanner is **not** a
compose service — it is external and drives `mri-marshal:9100` over raw MRD TCP (a real
scanner, or python-ismrmrd-server's `client.py`).

```
docker-compose.yml services (network cwru-demo-net):

  mri-marshal     marshal --http 0.0.0.0:8080 --mrd-port 9100 --dump-dir /session-data
                          --recon-host $RECON_HOST --recon-port $RECON_PORT $MARSHAL_DUMP
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

`MARSHAL_DUMP` is passed straight through to the marshal command; live mode
forwards k-space to `recon` and serves live snapshots, dump mode archives the full
scanner stream and serves no live snapshot (see Storage Layout).

## Storage Layout

`--dump` selects the archival mode. Modes are mutually exclusive — marshal commits to one at process startup. The selected mode's subtree is the only one populated.

Both modes use the same raw-MRD spool + post-close HDF5 convert model: incoming wire frames are written to `scan_<ts>.h5.spool` during the scan; on scan close the spool is replayed through the converter to produce the canonical `scan_<ts>.h5`. The `.spool` file is retained by default for forensic recovery.

```
${dump_dir}/                          session-data umbrella (--dump-dir flag)
│
├── live/                             ONLY in live mode (no --dump)
│   ├── from_scanner/
│   │   ├── scan_<ts>.h5.spool        raw MRD wire frames, written during scan
│   │   ├── scan_<ts>.h5              ISMRMRD HDF5, produced by converter on CLOSE
│   │   └── latest_image.h5           closed companion, atomic-rename per IMAGE
│   └── from_reconstruction/
│       ├── scan_<ts>.h5.spool
│       ├── scan_<ts>.h5
│       ├── latest_image.h5
│       └── latest_error.png          single overwritten recon-failure indicator
│
└── dump/                             ONLY in dump mode (--dump)
    ├── from_scanner/
    │   ├── scan_<ts>.h5.spool        raw MRD wire frames (full stream incl. ACQ)
    │   └── scan_<ts>.h5              ISMRMRD HDF5, produced on CLOSE
    └── from_reconstruction/
        ├── scan_<ts>.h5.spool
        └── scan_<ts>.h5
```

- `<ts>` is shared between scanner and recon lanes within the active mode.
- In live mode: `scan_<ts>.h5` archives images + waveforms (ECG). Raw acquisitions are NOT archived in live mode.
- In dump mode: `scan_<ts>.h5` archives the full scanner stream — acquisitions + images + waveforms + text + config. No files under `live/`, no `latest_image.h5`.
- HDF5 files use canonical libismrmrd layout (`appendAcquisition`, `appendImage`, `appendWaveform`).
- The per-scan `scan_<ts>.h5` is an archival output finalized on CLOSE. It is NOT a mid-scan readable interface (HDF5's default file locking would block mid-scan opens on an open writer anyway). Clients that need mid-scan reads use `latest_image.h5` (live mode only).
- `GET /image/latest` in live mode: `204 No Content` before first IMAGE; `{path, error}` after. Readers open the companion file's `image_0` group. In dump mode: `404 Not Found` with `{"error":"dump mode; no live snapshot"}`.
- The `.spool` file is present throughout the scan; it is a private format (`[uint16 tag][uint32 length][body]` per record) intended for post-scan replay, not a client-facing interface.

## Fault Tolerance

The marshal stays running regardless of recon failure:

- Recon return reading runs on a background thread.
- If recon is unreachable, scanner-side archival continues and recon connection state is reset for a later reconnect.
- A scanner-visible MRD `IMAGE(1022)` failure image is pushed on the active scanner TCP connection.
- `latest_error.png` is written so the webgl-client visually shows "reconstruction failed"
- T4 test: kill recon mid-scan, assert marshal still accepts MRD TCP connections and GET /health returns 200

If `--recon-host`/`--recon-port` are not set, the marshal has no reconstruction target. With `--dump`, scanner data is archived but never forwarded.

## WebSocket (Optional)

The marshal has an optional WebSocket listener (`--ws-port`). It is always compiled into the binary but only activated when the flag is passed. The WS interface broadcasts JSON event notifications. It is NOT used for scanner data transport (that's MRD TCP) or for image delivery (that's file-based via GET /image/latest).
