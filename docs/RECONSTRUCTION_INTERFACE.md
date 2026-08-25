# Reconstruction Service Interface

This document describes the MRD TCP interface between the MRI Marshal and reconstruction services.

## Transport

Marshal is the **client** on the recon leg. It opens an MRD TCP connection to the
recon server, forwards k-space, and reads reconstructed images back on the *same*
connection. The wire protocol is **raw TCP** with python-ismrmrd-server's 2-byte
message-ID framing — the same protocol the scanner uses, so the recon server is
agnostic to whether a scanner or the marshal is driving it.

Configure via: `--recon-host <host> --recon-port <port>`. In the default stack
(`docker-compose.yml`) marshal is launched with `--recon-host recon --recon-port
9002`, pointing at the `recon` service. If `--recon-host` is left unset, scanner
data is accepted but not forwarded to recon. Add `--dump` for H5 archival mirrors.

## Data Flow

```
MRI Marshal                         Recon Service
    │                                     │
    │── CONFIG_FILE(1) ──────────────────>│  select handler (e.g. "invertcontrast")
    │── METADATA_XML(3) ─────────────────>│  ISMRMRD XML header
    │── ACQUISITION(1008) × N ───────────>│  k-space lines
    │── WAVEFORM(1026) × M (optional) ──>│  ECG / physio data
    │── CLOSE(4) ────────────────────────>│  end of scan
    │                                     │
    │<── IMAGE(1022)/TEXT(5)/etc. ────────│  recon return messages
    │<── CLOSE(4) ────────────────────────│  recon done
    │                                     │
```

**Routing.** Marshal opens the recon session lazily: it connects and forwards the
buffered CONFIG/METADATA only when **k-space (ACQUISITION) arrives**. So the recon
leg engages only for k-space scans. Reconstructed images come back as IMAGE(1022)
on the same connection and are published to downstream clients.

A scanner-sent IMAGE(1022) is already reconstructed, so marshal does **not** forward
it to recon — it is published directly. Only ACQUISITION drives the recon service.

**Image geometry.** Every IMAGE(1022) carries its spatial pose in the ISMRMRD
`ImageHeader`: `position[3]` (slice center in scanner coordinates, mm) and the
direction cosines `read_dir[3]`, `phase_dir[3]`, `slice_dir[3]` (with
`field_of_view[3]` and `patient_table_position[3]`). Together these fix where the
slice sits and how it is oriented. Downstream consumers use them to place the image
in 3D, and the marshal uses the latest one as the starting point of the first
relative slice move of a scan (`/write/slice_delta`, `/write/file_slice_translation`;
see [SLICE_CONTROL_HANDOFF.md](SLICE_CONTROL_HANDOFF.md)).

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
- The marshal continues accepting scanner data (`--dump` H5 recording continues when enabled)
- A scanner-visible MRD `IMAGE(1022)` failure image is pushed on the active scanner TCP connection
- A "reconstruction failed" PNG is written to `latest_error.png`
- `GET /image/latest` returns `{"path": "...latest_error.png", "error": true}`
- The webgl-client renders the failure image
- Later scanner sessions can reconnect to recon when it is available again

No special error handling is needed on the recon side — TCP disconnection is sufficient.

## Storage

- Live scanner data (always): `${dump_dir}/live/from_scanner/scan_<ts>.h5` (canonical ISMRMRD HDF5, appended).
- Live recon data (always): `${dump_dir}/live/from_reconstruction/scan_<ts>.h5` (canonical ISMRMRD HDF5, appended; images grouped by `image_<image_series_index>`).
- Dump mirrors (with `--dump`): `${dump_dir}/dump/from_scanner/scan_<ts>.h5` and `${dump_dir}/dump/from_reconstruction/scan_<ts>.h5`.
- Live clients read the closed companion file at the path returned by `GET /image/latest` and open group `image_0` to read the most recently published image update.
