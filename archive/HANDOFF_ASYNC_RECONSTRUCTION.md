# Handoff: Implement Async Reconstruction Interface

## Context

The MRI marshal currently uses **synchronous HTTP** for reconstruction: it POSTs k-space to recon and waits for the response. This won't work for complex reconstruction that takes seconds.

## Requirement (from Andrew)

> "I would have it be async, and let the recon post back with images as they finish. Depending on the complexity of the recon we use it could be many seconds, I wouldn't have the connection sitting."

## Design Doc

Full specification is at: `docs/RECONSTRUCTION_INTERFACE.md` (see "PLANNED: Async/Callback Design" section)

## Summary of Changes Needed

### 1. Marshal Changes (`src/marshal_http.hpp`)

**Add new endpoint:** `POST /v1/mrd/callback`
- Receives reconstructed images from recon service
- Parses `X-MRD-Stream` header to identify the stream
- Stores ImageHeader + pixels to SWMR (same logic as current frame handler)

**Modify k-space handling in `/v1/mrd/frame`:**
- When receiving AcquisitionHeader (k-space), return `HTTP 202 Accepted` immediately
- POST to recon endpoint asynchronously (spawn thread or use async I/O)
- Include `X-MRD-Callback: http://mri-marshal:8080/v1/mrd/callback` header

**Current synchronous code to replace** (lines ~370-530):
```cpp
// Current: blocks waiting for response
http::write(stream, recon_req);
http::read(stream, recon_buffer, recon_res);
// ... store to SWMR
```

**New async approach:**
```cpp
// Return 202 immediately to k-space sender
// Spawn async task to:
//   1. POST to recon with callback header
//   2. Recon will POST back to /v1/mrd/callback when done
```

### 2. Mock Recon Changes (`tests/mock_recon_service.py`)

**Current synchronous:**
```python
@app.route('/reconstruct', methods=['POST'])
def reconstruct():
    # Process immediately
    return Response(image_data)
```

**New async:**
```python
@app.route('/reconstruct', methods=['POST'])
def reconstruct():
    callback_url = request.headers.get('X-MRD-Callback')
    stream_name = request.headers.get('X-MRD-Stream')
    raw_data = request.data

    # Spawn background thread
    thread = Thread(target=process_and_callback,
                    args=(raw_data, callback_url, stream_name))
    thread.start()

    # Return immediately
    return jsonify({"status": "processing"}), 202

def process_and_callback(raw_data, callback_url, stream_name):
    # Simulate processing time
    time.sleep(0.5)  # or longer for realistic GPU recon

    # Generate reconstructed image
    image_data = create_mock_image()

    # POST back to marshal
    requests.post(callback_url,
                  data=image_data,
                  headers={
                      'Content-Type': 'application/octet-stream',
                      'X-MRD-Stream': stream_name
                  })
```

### 3. Dockerfile Changes (`docker/Dockerfile.mock-recon`)

Add `requests` package for callback POST:
```dockerfile
RUN pip install --no-cache-dir flask requests
```

## Flow Diagram

```
K-Space Streamer                  MRI Marshal                    Recon Service
     │                                 │                              │
     │ POST /v1/mrd/frame              │                              │
     │ (AcquisitionHeader + k-space)   │                              │
     │────────────────────────────────>│                              │
     │                                 │                              │
     │ HTTP 202 Accepted               │                              │
     │ (immediately)                   │                              │
     │<────────────────────────────────│                              │
     │                                 │                              │
     │                                 │ POST /reconstruct            │
     │                                 │ Headers:                     │
     │                                 │   X-MRD-Callback: http://... │
     │                                 │   X-MRD-Stream: raw_scan     │
     │                                 │ Body: k-space binary         │
     │                                 │─────────────────────────────>│
     │                                 │                              │
     │                                 │ HTTP 202 Accepted            │
     │                                 │ {"status": "processing"}     │
     │                                 │<─────────────────────────────│
     │                                 │                              │
     │                                 │   ... recon processes ...    │
     │                                 │   (could be many seconds)    │
     │                                 │                              │
     │                                 │ POST /v1/mrd/callback        │
     │                                 │ Headers:                     │
     │                                 │   X-MRD-Stream: raw_scan     │
     │                                 │ Body: ImageHeader + pixels   │
     │                                 │<─────────────────────────────│
     │                                 │                              │
     │                                 │ HTTP 200 OK                  │
     │                                 │─────────────────────────────>│
     │                                 │                              │
     │                                 │ Store to SWMR                │
     │                                 │                              │
     │                                 │                              │
Viz Client ───── GET /v1/mrd/latest ──>│                              │
     │                                 │                              │
     │<─────── ImageHeader + pixels ───│                              │
```

## Files to Modify

| File | Changes |
|------|---------|
| `.worktrees/mri_data_marshal/src/marshal_http.hpp` | Add `/v1/mrd/callback` endpoint, make recon forwarding async |
| `.worktrees/mri_data_marshal/tests/mock_recon_service.py` | Add async processing with callback POST |
| `docker/Dockerfile.mock-recon` | Add `requests` pip package |

## Testing Steps

1. Rebuild images:
   ```bash
   ./scripts/build-client-images.sh
   ```

2. Start services:
   ```bash
   docker compose --env-file .env.demo -f docker-compose.demo.yml up mri-marshal
   docker compose --env-file .env.demo -f docker-compose.demo.yml up mock-recon
   docker compose --env-file .env.demo -f docker-compose.demo.yml up kspace-streamer
   ```

3. Verify:
   - K-space streamer gets 202 responses immediately (not waiting)
   - Mock-recon logs show callback POST to marshal
   - Marshal logs show receiving callback and storing to SWMR
   - `curl http://localhost:8080/v1/mrd/latest` returns reconstructed images

## Edge Cases to Handle

1. **Recon never calls back** - Marshal should have timeout/cleanup for stale jobs
2. **Callback arrives before marshal is ready** - Should still work since callback just stores to SWMR
3. **Multiple pending reconstructions** - Each should complete independently
4. **Recon service down** - Marshal should return error, not hang

## Optional Enhancements

- Job tracking with unique IDs for monitoring
- WebSocket notification when reconstruction completes
- Retry logic if callback fails
- Metrics/logging for reconstruction latency
