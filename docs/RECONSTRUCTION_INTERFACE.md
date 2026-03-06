# Reconstruction Service Interface

This document describes the interface between the MRI Marshal and external reconstruction services.

## Current Design: Synchronous HTTP

The marshal uses a **synchronous request/response** pattern:

```
K-Space Streamer                  MRI Marshal                    Recon Service
     │                                 │                              │
     │ POST /v1/mrd/frame              │                              │
     │ (AcquisitionHeader + k-space)   │                              │
     │────────────────────────────────>│                              │
     │                                 │                              │
     │                                 │ POST /reconstruct            │
     │                                 │ Body: raw k-space binary     │
     │                                 │─────────────────────────────>│
     │                                 │                              │
     │                                 │      (processing...)         │
     │                                 │                              │
     │                                 │ HTTP 200 OK                  │
     │                                 │ Body: ImageHeader + pixels   │
     │                                 │<─────────────────────────────│
     │                                 │                              │
     │                                 │ Store to SWMR HDF5           │
     │                                 │                              │
     │ HTTP 201 Created                │                              │
     │<────────────────────────────────│                              │
```

## Interface Requirements

### Request: POST /reconstruct

**Headers:**
- `Content-Type: application/octet-stream`
- `X-MRD-Stream: <stream_name>` (optional, for logging)

**Body:**
- Raw binary data: `AcquisitionHeader` (340 bytes) + k-space samples (complex float array)

### Response: HTTP 200 OK

**Body:**
- Binary data: `ISMRMRD::ImageHeader` (198 bytes) + pixel data (float array)

**Expected pixel data size:**
```
matrix_size[0] * matrix_size[1] * matrix_size[2] * channels * sizeof(float)
```

### Error Response

Return HTTP 4xx/5xx with error message in body. Marshal will return 502 to client.

## Code Reference

Marshal implementation: `.worktrees/mri_data_marshal/src/marshal_http.hpp` lines 370-530

```cpp
// Marshal sends request
http::request<http::string_body> recon_req{http::verb::post, "/reconstruct", 11};
recon_req.set(http::field::content_type, "application/octet-stream");
recon_req.set("X-MRD-Stream", stream_header);
recon_req.body() = body;  // raw k-space binary
http::write(stream, recon_req);

// Marshal reads response synchronously
http::response<http::string_body> recon_res;
http::read(stream, recon_buffer, recon_res);

// Response body = ImageHeader + pixel data
const std::string& reconstructed = recon_res.body();
```

## Why Synchronous?

1. **Simpler** - No callback infrastructure needed
2. **Stateless** - Recon service doesn't track pending jobs
3. **Error handling** - Marshal gets errors immediately
4. **Real-time MRI** - Reconstruction is fast (milliseconds), no need for async
5. **Gadgetron compatible** - Real Gadgetron uses similar synchronous calls

## PLANNED: Async/Callback Design

> **Feedback from Andrew:** "I would have it be async, and let the recon post back with images as they finish. Depending on the complexity of the recon we use it could be many seconds, I wouldn't have the connection sitting."

The current synchronous design won't scale for complex reconstruction that takes seconds. The async design:

```
K-Space Streamer                  MRI Marshal                    Recon Service
     │                                 │                              │
     │ POST /v1/mrd/frame              │                              │
     │────────────────────────────────>│                              │
     │                                 │                              │
     │ HTTP 202 Accepted               │ POST /reconstruct            │
     │ (immediately)                   │ Body: k-space + callback_url │
     │<────────────────────────────────│─────────────────────────────>│
     │                                 │                              │
     │                                 │ HTTP 202 Accepted            │
     │                                 │ {"job_id": "abc123"}         │
     │                                 │<─────────────────────────────│
     │                                 │                              │
     │                                 │   ... recon processes ...    │
     │                                 │   (could be many seconds)    │
     │                                 │                              │
     │                                 │ POST {callback_url}          │
     │                                 │ Body: ImageHeader + pixels   │
     │                                 │<─────────────────────────────│
     │                                 │                              │
     │                                 │ HTTP 200 OK                  │
     │                                 │─────────────────────────────>│
     │                                 │                              │
     │                                 │ Store to SWMR                │
```

### Required Marshal Changes

1. **Callback endpoint**: Marshal needs to expose `POST /v1/mrd/callback` to receive reconstructed images
2. **Async response**: Return `HTTP 202 Accepted` immediately when receiving k-space
3. **Job tracking**: Optional - track pending jobs by ID for monitoring
4. **Timeout handling**: Handle cases where recon never calls back

### Required Recon Service Interface

**Request from Marshal:**
```
POST /reconstruct
Headers:
  Content-Type: application/octet-stream
  X-MRD-Stream: <stream_name>
  X-MRD-Callback: http://mri-marshal:8080/v1/mrd/callback
Body: AcquisitionHeader + k-space binary
```

**Immediate Response:**
```
HTTP 202 Accepted
{"job_id": "abc123", "status": "processing"}
```

**Callback from Recon (when done):**
```
POST http://mri-marshal:8080/v1/mrd/callback
Headers:
  Content-Type: application/octet-stream
  X-MRD-Stream: <stream_name>
  X-MRD-Job-Id: abc123
Body: ImageHeader + pixel data
```

### Benefits of Async Design

- **No hanging connections** - Marshal doesn't wait for slow recon
- **Scalable** - Can handle multiple pending reconstructions
- **GPU-friendly** - Recon can batch process for efficiency
- **Decoupled** - Recon can be on different machine with network latency

### Implementation Priority

This is a **future enhancement**. Current mock-recon uses synchronous for simplicity.
When implementing real Gadgetron integration, switch to async.

## Mock Reconstruction Service

Location: `tests/mock_recon_service.py`

The mock service implements the synchronous interface:

```python
@app.route('/reconstruct', methods=['POST'])
def reconstruct():
    # Receive k-space (ignored in mock)
    raw_data = request.data

    # Generate mock 64x64 gradient image
    image_header = create_ismrmrd_image_header(64, 64, 1)
    pixels = create_gradient_pattern(64, 64)

    # Return ImageHeader + pixels as binary
    return Response(image_header + pixels,
                    mimetype='application/octet-stream')
```

## Implementing a Real Reconstruction Service

To replace mock-recon with real reconstruction (e.g., Gadgetron wrapper):

1. Accept `POST /reconstruct` with binary k-space
2. Parse `ISMRMRD::AcquisitionHeader` from request body
3. Perform FFT / reconstruction algorithm
4. Create `ISMRMRD::ImageHeader` with correct dimensions
5. Return header + pixel data as binary response

Example Python skeleton:

```python
@app.route('/reconstruct', methods=['POST'])
def reconstruct():
    data = request.data

    # Parse AcquisitionHeader (340 bytes)
    acq_header = parse_acquisition_header(data[:340])
    kspace = parse_kspace_samples(data[340:], acq_header)

    # Reconstruct (your algorithm here)
    image = ifft2(kspace)

    # Build response
    img_header = build_image_header(image.shape)
    return Response(img_header + image.tobytes(),
                    mimetype='application/octet-stream')
```

## Configuration

Set reconstruction endpoint via:

```bash
# Environment variable
RECON_ENDPOINT=http://mock-recon:9002

# Or marshal command line
./marshal --recon-endpoint http://localhost:9002
```

If no endpoint is configured, marshal returns HTTP 501 Not Implemented when it receives raw k-space data.
