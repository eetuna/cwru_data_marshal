# MRI Marshal Protocol Contract

This is the MRI marshal contract from the transcript/Slack requirements.

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

The marshal forwards scanner MRD messages to recon over MRD TCP and archives scanner-side ISMRMRD data under `from_scanner/`.

## Marshal to Recon

The marshal opens one MRD TCP connection to the configured recon service for a scan. Recon should see the same MRD message sequence it would see from a direct scanner/client connection.

Recon code should not need marshal-specific changes.

## Recon to Marshal to Scanner

Recon return messages arrive on the recon MRD TCP connection. The marshal pushes scanner-relevant return messages back to the scanner on the original scanner TCP connection.

This return path is not optional for scanner operation. It is how reconstructed images, text/feedback, close notifications, and future MRD feedback messages reach the scanner.

Recon-side images are also archived under `from_reconstruction/`.

## HTTP Side Channel

HTTP is only for non-scanner clients:

- `GET /image/latest`
- `GET/PUT /transform`
- `POST/GET /pose`
- `GET /dump/scanner`
- `GET /dump/recon`
- `GET /health`

`GET /image/latest` returns a JSON file pointer. It does not inline the image bytes. Viz-style clients read the file from the shared filesystem.

## Failure Behavior

The marshal process must remain up if recon fails. Scanner data should continue to be accepted and archived. Recon connection state must be reset so a later scan can reconnect.

For non-scanner clients, recon failure is exposed through `GET /image/latest` with `error: true` and a failure image path. If scanner-visible failure indication is required, it must be sent as a valid MRD return message on the scanner TCP connection.

