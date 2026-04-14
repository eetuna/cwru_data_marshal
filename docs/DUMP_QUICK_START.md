# Dump Quick Start

Use this to validate the MRI marshal's retrospective dump evidence trails.

Dump is not the live viz path. Dump means the marshal records the MRD traffic it receives on each side of the proxy so the two sides can be compared after an experiment.

- Scanner-side records go under `session-data/from_scanner/`.
- Reconstruction-side records go under `session-data/from_reconstruction/`.
- Standard ISMRMRD objects are written to canonical `.h5` files.
- `/image/latest` is only a live client file pointer. It is not the dump.

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
ls -lh session-data/from_scanner/
ls -lh session-data/from_reconstruction/
```

Expected:

- `from_scanner/scan_*.h5` exists and contains standard scanner ISMRMRD objects.
- `from_reconstruction/` should not contain a recon `.h5` for this run.

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
ls -lh session-data/from_scanner/
ls -lh session-data/from_reconstruction/
```

Expected:

- `from_scanner/scan_*.h5` contains scanner-side acquisitions/images/waveforms.
- `from_reconstruction/scan_*.h5` contains recon-side standard ISMRMRD objects, currently images and any waveforms returned by recon.
- The scanner client logs reconstructed images received back over MRD TCP.

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

- `from_scanner/scan_*.h5` contains images sent by `image-streamer`.
- Recon-side output appears only in full proxy mode when a recon service actually returns messages.

## Latest File Pointer Is Separate

Non-scanner clients can ask for the latest live display file:

```bash
curl -s http://localhost:8080/image/latest | jq
```

Example live image response:

```json
{"path":"/session-data/from_reconstruction/latest_image.h5","error":false,"slices":12}
```

Example recon failure response:

```json
{"path":"/session-data/from_reconstruction/latest_error.png","error":true,"slices":12}
```

The scanner does not use this endpoint. Scanner-visible recon output and failure images are MRD messages pushed on the scanner's existing TCP connection.

## Stop

```bash
cdd --profile dump --profile viz --profile robot-clients down
```
