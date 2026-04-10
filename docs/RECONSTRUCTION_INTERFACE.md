# Reconstruction Service Interface

This document describes the interface between the MRI Marshal and external reconstruction services in the v2 API.

## Design: Fire-and-forward

The marshal forwards acquisition frames to the recon service and receives reconstructed images back. There are no custom headers, no callback URLs, and no job-tracking IDs in the protocol.

```
Scanner                       MRI Marshal                    Recon Service
  |                                |                              |
  | POST /frame                   |                              |
  | (acquisition binary)          |                              |
  |------------------------------>|                              |
  |                                |                              |
  |                                | POST /image                  |
  |                                | Body: acquisition binary     |
  |                                |----------------------------->|
  |                                |                              |
  |                                |    (processing...)           |
  |                                |                              |
  |                                | POST /image (to marshal)     |
  |                                | Body: reconstructed image    |
  |                                |<-----------------------------|
  |                                |                              |
  |                                | Store to                     |
  |                                | from_reconstruction/*.h5     |
  |                                |                              |
  |                                |    GET /image/latest         |
  |                                |<----------  Clients          |
```

## Recon service contract

### Receive acquisitions

The recon service must accept:

```
POST /image
Content-Type: application/octet-stream
Body: raw acquisition binary
```

### Post results back

When reconstruction is complete, the service posts the result to the marshal:

```
POST /image
Content-Type: application/octet-stream
Body: ImageHeader + pixel data (binary)
```

## Marshal configuration

Set the recon URL when starting the marshal:

```bash
# Flag
./marshal --recon-url http://recon-host:9002

# Or environment variable
RECON_URL=http://recon-host:9002
```

If `--recon-url` is not set, the marshal stores raw acquisitions but does not forward them.

## Storage

- Scanner data: `<dump-dir>/from_scanner/*.h5`
- Reconstructed images: `<dump-dir>/from_reconstruction/*.h5`

HDF5 files are standalone (no concurrent-access mode required). Readers open finished files normally.

## Implementing a real reconstruction service

To replace mock-recon with real reconstruction:

1. Accept `POST /image` with binary acquisition data.
2. Parse the acquisition header and k-space samples.
3. Perform your reconstruction algorithm.
4. POST the resulting image back to the marshal at `POST /image`.

```python
from flask import Flask, request, Response
import requests

app = Flask(__name__)
MARSHAL_URL = "http://mri-marshal:8080"

@app.route('/image', methods=['POST'])
def reconstruct():
    data = request.data

    # Parse and reconstruct (your algorithm here)
    acq_header = parse_acquisition_header(data[:340])
    kspace = parse_kspace_samples(data[340:], acq_header)
    image = ifft2(kspace)

    # Post result back to marshal
    img_header = build_image_header(image.shape)
    result = img_header + image.tobytes()
    requests.post(f"{MARSHAL_URL}/image", data=result,
                  headers={"Content-Type": "application/octet-stream"})

    return Response(status=200)
```
