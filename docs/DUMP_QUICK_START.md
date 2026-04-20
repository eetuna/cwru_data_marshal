# Dump Quick Start

Use this to validate the MRI marshal's retrospective dump evidence trails.

Dump is not the live viz path. Dump means the marshal records the full MRD traffic it receives on each side of the proxy (including raw acquisitions) so the two sides can be compared after an experiment.

- Dump mode is **exclusive**: marshal is started with `--dump` and only the `dump/` subtree is populated. Live mode (default, no `--dump`) populates only the `live/` subtree. The two modes are mutually exclusive — the selected mode is fixed at process startup.
- Scanner-side records go under `session-data/dump/from_scanner/`.
- Reconstruction-side records go under `session-data/dump/from_reconstruction/`.
- Each scan produces a `scan_<ts>.h5.spool` (raw MRD wire frames, written during the scan) and, on CLOSE, a canonical `scan_<ts>.h5` (ISMRMRD HDF5 produced by replaying the spool through the converter). The `.spool` is retained by default.
- The per-scan `scan_<ts>.h5` is archival and only exists after CLOSE; it is NOT a mid-scan reader interface.
- In dump mode, `GET /image/latest` returns `404 Not Found` (`{"error":"dump mode; no live snapshot"}`). The closed `latest_image.h5` companion that serves the live viz path exists only in live mode.

## Case 1 -- scanner-side dump only

This starts the dump-enabled marshal without starting recon. It validates that scanner-origin MRD traffic is archived even when reconstruction is unavailable.

```bash
# Terminal 1
DUMP_RECON_HOST= cdd --profile dump up mri-marshal-dump

# Terminal 2
cdd --profile dump up kspace-streamer-dump
```

Check scanner-side evidence:

```bash
curl -s http://localhost:8080/dump/scanner | jq
ls -lh session-data/dump/from_scanner/
ls -lh session-data/dump/from_reconstruction/
```

Expected:

- `dump/from_scanner/scan_<ts>.h5.spool` exists during the scan; after CLOSE, `dump/from_scanner/scan_<ts>.h5` is produced by the converter and contains the full scanner stream (acquisitions + images + waveforms + text + config).
- `dump/from_reconstruction/` should not contain a recon `.h5` for this run (no recon was started).
- `live/` is NOT populated in dump mode (mutual exclusion).

Do not run `mri-marshal` and `mri-marshal-dump` at the same time; they both bind ports `8080` and `9100`.

## Case 2 -- full proxy dump

This validates the normal path: scanner input is archived, forwarded to recon, recon output is archived, and recon return messages are pushed back to the scanner on the original MRD TCP connection.

```bash
# Terminal 1
cdd up mock-recon

# Terminal 2
cdd --profile dump up mri-marshal-dump

# Terminal 3
cdd --profile dump up kspace-streamer-dump
```

Check both evidence trails:

```bash
curl -s http://localhost:8080/dump/scanner | jq
curl -s http://localhost:8080/dump/recon | jq
ls -lh session-data/dump/from_scanner/
ls -lh session-data/dump/from_reconstruction/
ls -lh session-data/live/from_scanner/
ls -lh session-data/live/from_reconstruction/
```

Expected:

- `dump/from_scanner/scan_<ts>.h5` (produced on CLOSE from the spool) contains the full scanner stream — acquisitions, images, waveforms, text, config.
- `dump/from_reconstruction/scan_<ts>.h5` contains recon-side standard ISMRMRD objects — images grouped by `image_<image_series_index>` and any waveforms returned by recon.
- `live/` is NOT populated (dump mode is exclusive).
- The scanner client logs reconstructed images received back over MRD TCP (recon pushback is independent of archival mode).

## Case 3 -- scanner-origin images

This sends scanner-side `IMAGE(1022)` messages instead of k-space acquisitions. The images are scanner input, so they belong in `from_scanner/`.

```bash
# Terminal 1
DUMP_RECON_HOST= cdd --profile dump up mri-marshal-dump

# Terminal 2
cdd --profile dump run --rm --no-deps image-streamer \
  ./build/image_streamer --host mri-marshal-dump --port 9100 --count 5
```

Expected:

- `dump/from_scanner/scan_<ts>.h5` contains images sent by `image-streamer`.
- `live/` is NOT populated (dump mode is exclusive).
- Recon-side output appears only in full proxy mode when a recon service actually returns messages.

## Latest File Pointer Is Separate (Live Mode Only)

In **live mode**, non-scanner clients can ask for the latest live display file. In dump mode this endpoint returns `404 Not Found` with `{"error":"dump mode; no live snapshot"}`.

```bash
curl -s http://localhost:8080/image/latest | jq
```

Example live image response:

```json
{"path":"/session-data/live/from_reconstruction/latest_image.h5","error":false}
```

Clients open the closed companion file returned by `/image/latest` and read group `image_0` to render the newest published image update.

Example recon failure response:

```json
{"path":"/session-data/live/from_reconstruction/latest_error.png","error":true}
```

The scanner does not use this endpoint. Scanner-visible recon output and failure images are MRD messages pushed on the scanner's existing TCP connection.

## Stop

```bash
cdd --profile dump --profile viz --profile robot-clients down
```
