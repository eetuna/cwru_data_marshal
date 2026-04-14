# Reconstruction Service Interface

This document describes the MRD TCP interface between the MRI Marshal and reconstruction services.

## Transport

Marshal connects to recon via **raw TCP** using python-ismrmrd-server's 2-byte message ID framing. This is the same protocol the scanner uses — the recon service is agnostic to whether it's talking to a scanner or the marshal.

Configure via: `--recon-host <host> --recon-port <port>` (default: not set, so scanner data is accepted but not forwarded to recon; add `--dump` if you also want H5 recording).

## Data Flow

```
MRI Marshal                         Recon Service
    │                                     │
    │── CONFIG_FILE(1) ──────────────────>│  select handler (e.g. "simplefft")
    │── METADATA_XML(3) ─────────────────>│  ISMRMRD XML header
    │── ACQUISITION(1008) × N ───────────>│  k-space lines
    │── WAVEFORM(1026) × M (optional) ──>│  ECG / physio data
    │── CLOSE(4) ────────────────────────>│  end of scan
    │                                     │
    │<── IMAGE/TEXT/etc. ─────────────────│  recon return messages
    │<── CLOSE(4) ────────────────────────│  recon done
    │                                     │
```

## Recon service contract

The recon service is a TCP server that:

1. **Listens** on a port (e.g. 9002).
2. **Accepts** one TCP connection per scan.
3. **Reads** messages using the 2-byte tag framing:
   - CONFIG_FILE(1) or CONFIG_TEXT(2) — selects the reconstruction handler
   - METADATA_XML_TEXT(3) — ISMRMRD XML header describing the scan
   - ACQUISITION(1008) — one k-space line (340B header + trajectory + samples)
   - WAVEFORM(1026) — physio data (40B header + uint32 samples), used for cardiac gating
   - CLOSE(4) — end of input
4. **Processes** the acquisitions (e.g. 2D IFFT, GRAPPA).
5. **Writes** return messages back on the SAME TCP connection:
   - IMAGE(1022) — 198B ImageHeader + 8B uint64 attribute_len + attribute string + pixel data
   - TEXT(5) — status/feedback text
   - Other known MRD message types when needed by the recon workflow
   - CLOSE(4) — end of output

This is identical to python-ismrmrd-server's `Connection` class in `connection.py`.

## Implementing a real reconstruction service

Replace `mock_recon.py` with your own TCP server. The simplest approach: use python-ismrmrd-server directly.

```python
# Use python-ismrmrd-server as-is:
python main.py -p 9002

# Or write a custom handler module:
# 1. Create your_handler.py with a process(connection, config, metadata) function
# 2. Run: python main.py -p 9002
# 3. Send CONFIG_TEXT("your_handler") to select it
```

Or write a standalone TCP server:

```python
import socket, struct, ismrmrd, numpy as np

def handle_client(conn):
    # Read CONFIG
    tag = struct.unpack('<H', conn.recv(2))[0]
    # ... read config payload ...

    # Read METADATA_XML
    tag = struct.unpack('<H', conn.recv(2))[0]
    # ... read XML payload ...

    # Read acquisitions
    acquisitions = []
    while True:
        tag = struct.unpack('<H', conn.recv(2))[0]
        if tag == 4:  # CLOSE
            break
        elif tag == 1008:  # ACQUISITION
            acq = ismrmrd.Acquisition.deserialize_from(conn.recv)
            acquisitions.append(acq)
        elif tag == 1026:  # WAVEFORM
            wf = ismrmrd.Waveform.deserialize_from(conn.recv)
            # Use for cardiac gating if needed

    # Reconstruct
    image_data = your_reconstruction(acquisitions)

    # Send images back
    image = ismrmrd.Image.from_array(image_data)
    conn.send(struct.pack('<H', 1022))  # IMAGE tag
    image.serialize_into(conn.send)

    conn.send(struct.pack('<H', 4))  # CLOSE
    conn.shutdown(socket.SHUT_RDWR)
    conn.close()
```

## Fault tolerance

If the recon service is unreachable or crashes mid-scan:

- The marshal's recon forwarder marks the recon connection disconnected and logs a warning
- The marshal continues accepting scanner data (`--dump` H5 recording continues when enabled)
- A scanner-visible MRD `IMAGE(1022)` failure image is pushed on the active scanner TCP connection
- A "reconstruction failed" PNG is written to `latest_error.png`
- `GET /image/latest` returns `{"path": "...latest_error.png", "error": true}`
- The viz client displays the failure visually
- Later scanner sessions can reconnect to recon when it is available again

No special error handling is needed on the recon side — TCP disconnection is sufficient.

## Storage

- Live scanner data (always): `${dump_dir}/live/from_scanner/scan_<ts>.h5` (canonical ISMRMRD HDF5, appended).
- Live recon data (always): `${dump_dir}/live/from_reconstruction/scan_<ts>.h5` (canonical ISMRMRD HDF5, appended; images grouped by `image_<image_series_index>`).
- Dump mirrors (with `--dump`): `${dump_dir}/dump/from_scanner/scan_<ts>.h5` and `${dump_dir}/dump/from_reconstruction/scan_<ts>.h5`.
- Live clients read the recon-side live file at the path returned by `GET /image/latest`, using the `newest_series` field to pick the current volume's image group.
