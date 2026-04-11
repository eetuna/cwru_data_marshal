# System Architecture

## Overview

The CWRU Data Marshal is a persistent intermediary between the MRI scanner and the reconstruction service. It archives all data, forwards scanner MRD messages to recon, receives recon MRD return messages back, pushes scanner-relevant return messages to the scanner, and serves query endpoints for visualization and control clients.

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
                              │                  │                    │
                         ┌────┴────┐        ┌────┴─────┐         ┌───┴────┐
                         │Viz Clnt │        │Front-End │         │Pose Clnt│
                         │(file rd)│        │(delta API)│        │(HTTP)  │
                         └─────────┘        └──────────┘         └────────┘
```

**Scanner → Marshal:** MRD TCP on `--mrd-port` (default 9100). The scanner opens a persistent TCP connection and sends MRD messages using python-ismrmrd-server's 2-byte message ID framing.

**Marshal → Recon:** MRD TCP to `--recon-host:--recon-port`. The marshal opens a TCP connection to the recon service and forwards scanner messages using the same framing. Recon sends return messages back on this connection.

**Marshal → Scanner (return path):** Recon return messages are pushed back to the scanner on the SAME TCP socket the scanner connected on. This satisfies the requirement: "Marshall needs to push, and it must come over the existing connection that was established for the scan."

**Query/Control clients → Marshal:** HTTP on `--http` (default 0.0.0.0:8080). The viz client, pose client, tracker, and any external consumer use HTTP GET/POST for querying images, transforms, poses, and health.

**Viz client:** Polls `GET /image/latest` over HTTP, receives a file path, opens the standalone binary file directly from disk. No HDF5 concurrent access needed.

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

## Docker Compose Topology

```
docker-compose.demo.yml services:

  mri-marshal        --http 0.0.0.0:8080 --mrd-port 9100 --recon-host mock-recon --recon-port 9002
  mock-recon          MRD TCP server on port 9002
  kspace-streamer     MRD TCP client → mri-marshal:9100 (acquisitions + ECG waveforms with --ecg)
  image-streamer      MRD TCP client → mri-marshal:9100 (pre-reconstructed images)
  viz-client          HTTP client → mri-marshal:8080 (GET /image/latest + file read)
  pose-client         HTTP client → mri-marshal:8080 (POST /pose)
  robot-marshal       HTTP server on port 8081 (independent)
  robot-clients       catheter-tracking, controller, planning, front-end, surface-tracking → robot-marshal:8081
```

## Storage Layout

```
${dump_dir}/
├── from_scanner/
│   └── scan_<timestamp>.h5          canonical ISMRMRD HDF5 (acquisitions, images, waveforms)
├── from_reconstruction/
│   ├── scan_<timestamp>.h5          canonical ISMRMRD HDF5 (reconstructed images)
│   ├── latest_image.bin             standalone file for live viz (raw ISMRMRD image wire bytes)
│   └── latest_error.png             reconstruction-failed indicator (if applicable)
```

- `from_scanner/*.h5` — everything received from the scanner via MRD TCP
- `from_reconstruction/*.h5` — reconstructed images received from the recon service
- HDF5 files use canonical libismrmrd layout (`appendAcquisition`, `appendImage`, `appendWaveform`)
- HDF5 files are readable only after `/close` (no concurrent access mode)
- `latest_image.bin` is updated atomically (write-to-temp + rename) during the scan for live viewing
- `latest_error.png` is a pre-made failure image written when the recon forwarder detects recon is down

## Fault Tolerance

The marshal stays running regardless of recon failure:

- Recon return reading runs on a background thread.
- If recon is unreachable, scanner-side archival continues and recon connection state is reset for a later reconnect.
- `latest_error.png` is written so the viz client visually shows "reconstruction failed"
- T4 test: kill recon mid-scan, assert marshal still accepts MRD TCP connections and GET /health returns 200

If `--recon-host`/`--recon-port` are not set, the marshal runs in archival-only mode: scanner data is archived but never forwarded.

## WebSocket (Optional)

The marshal has an optional WebSocket listener (`--ws-port`). It is always compiled into the binary but only activated when the flag is passed. The WS interface broadcasts JSON event notifications. It is NOT used for scanner data transport (that's MRD TCP) or for image delivery (that's file-based via GET /image/latest).
