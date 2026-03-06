# MRI Marshal - Complete HTTP Routing Examples

**Document Purpose:** Complete, standalone examples showing how the MRI Marshal routes different data types through `/v1/mrd/frame` and `/v1/mrd/ingest` endpoints.

**Last Updated:** 2026-01-29

---

## Overview

The MRI Marshal automatically detects data types and routes them appropriately:

| Data Type | Detection | /v1/mrd/frame | /v1/mrd/ingest |
|-----------|-----------|---------------|----------------|
| **IMAGE** (reconstructed) | ImageHeader signature | Store to SWMR | Store to SWMR (with warning) |
| **ACQUISITION** (raw k-space) | AcquisitionHeader signature | Forward to recon → SWMR | Forward to recon → Complete file |
| **HDF5_FILE** (complete file) | HDF5 signature | Internal forward to /ingest | Save as-is |
| **UNKNOWN** (invalid) | No valid signature | HTTP 400 | HTTP 400 |

---

## Example 1: `/v1/mrd/frame` + Reconstructed Image

**Use Case:** Scanner sends pre-reconstructed image frames (production mode)

### HTTP Request

```http
POST /v1/mrd/frame HTTP/1.1
Host: localhost:8080
X-MRD-Stream: cardiac_scan
Content-Type: application/octet-stream
Content-Length: 262484

[Binary Body: ImageHeader (198 bytes) + pixel data (262144 bytes)]
```

### Complete Flow

```
1. Marshal receives HTTP POST with 262484 bytes
2. Detect type by inspecting first 340 bytes:
   - version = 1 ✓
   - matrix_size[0] = 256 ✓
   - matrix_size[1] = 256 ✓
   - channels = 1 ✓
   - data_type = 5 (FLOAT) ✓
   → Result: MrdDataType::IMAGE
3. Route: IMAGE → Store to SWMR directly
4. Open/create: /session-data/run_20260129_123456/mrd/cardiac_scan.mrd
5. Append frame 42 to HDF5 file using SWMR mode
6. Flush to disk
```

### HTTP Response

```http
HTTP/1.1 200 OK
Content-Type: application/json

{
  "path": "/session-data/run_20260129_123456/mrd/cardiac_scan.mrd",
  "frame_index": 42,
  "dims": [256, 256, 1],
  "flushed": true
}
```

---

## Example 2: `/v1/mrd/frame` + HDF5 Complete File

**Use Case:** Client accidentally sends complete HDF5 file to streaming endpoint

### HTTP Request

```http
POST /v1/mrd/frame HTTP/1.1
Host: localhost:8080
Content-Type: application/octet-stream
Content-Length: 52428800

[Binary Body: Complete HDF5 file - 50 MB]
```

### Complete Flow

```
1. Marshal receives HTTP POST with 52428800 bytes
2. Detect type by inspecting first 8 bytes:
   - Bytes 0-7: 89 48 44 46 0D 0A 1A 0A ✓
   → Result: MrdDataType::HDF5_FILE
3. Route: HDF5_FILE → Internal forward to ingest logic (calls mrd::ingest_payload)
4. Generate filename: 2026-01-29_000042.mrd
5. Write complete file: /session-data/run_20260129_123456/mrd/2026-01-29_000042.mrd
```

### HTTP Response

```http
HTTP/1.1 201 Created
Content-Type: application/json

{
  "path": "/session-data/run_20260129_123456/mrd/2026-01-29_000042.mrd",
  "size_bytes": 52428800,
  "type": "mrd",
  "note": "HDF5 file detected, forwarded to ingest logic"
}
```

---

## Example 3A: `/v1/mrd/frame` + Raw K-Space (NO Reconstruction)

**Use Case:** Scanner sends raw k-space but marshal not configured for reconstruction

### HTTP Request

```http
POST /v1/mrd/frame HTTP/1.1
Host: localhost:8080
X-MRD-Stream: cardiac_scan
Content-Type: application/octet-stream
Content-Length: 33108

[Binary Body: AcquisitionHeader (340 bytes) + k-space samples (32768 bytes)]
```

### Complete Flow

```
1. Marshal receives HTTP POST with 33108 bytes
2. Detect type by inspecting first 340 bytes:
   - version = 1 ✓
   - number_of_samples = 256 ✓
   - active_channels = 32 ✓
   - trajectory_dimensions = 0 ✓
   → Result: MrdDataType::ACQUISITION
3. Route: ACQUISITION → Check if reconstruction enabled
4. state.recon_enabled = false (no --recon-endpoint flag)
```

### HTTP Response

```http
HTTP/1.1 501 Not Implemented
Content-Type: application/json

{
  "error": "reconstruction service not configured",
  "detected_type": "ACQUISITION",
  "message": "Raw k-space detected but no reconstruction service available",
  "hint": "Start marshal with --recon-endpoint http://localhost:9002"
}
```

---

## Example 3B: `/v1/mrd/frame` + Raw K-Space (WITH Reconstruction)

**Use Case:** Scanner sends raw k-space, marshal forwards to reconstruction service

### Marshal Configuration

```bash
# Marshal started with reconstruction endpoint
./marshal --recon-endpoint http://reconstruction-service:9002
```

### HTTP Request (Client → Marshal)

```http
POST /v1/mrd/frame HTTP/1.1
Host: localhost:8080
X-MRD-Stream: cardiac_scan
Content-Type: application/octet-stream
Content-Length: 33108

[Binary Body: AcquisitionHeader (340 bytes) + k-space samples (32768 bytes)]
```

### Complete Flow

```
1. Marshal receives HTTP POST with 33108 bytes (from client)
2. Detect type: Result = MrdDataType::ACQUISITION
3. Route: ACQUISITION → Check reconstruction enabled
4. state.recon_enabled = true
5. Marshal forwards to reconstruction service:
```

### HTTP Request (Marshal → Reconstruction Service)

```http
POST /reconstruct HTTP/1.1
Host: reconstruction-service:9002
Content-Type: application/octet-stream
Content-Length: 33108

[Body: raw k-space - 33108 bytes as received]
```

### HTTP Response (Reconstruction Service → Marshal)

```http
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Length: 262484

[Body: ImageHeader (198 bytes) + reconstructed pixels (262144 bytes)]
```

### Processing by Marshal

```
6. Marshal receives reconstruction response (262484 bytes)
7. Marshal parses reconstructed response:
   - Read first 340 bytes as ImageHeader
   - Validate: version=1, matrix_size=[256,256,1], channels=1, data_type=5
   - Extract pixel data from bytes 340-262484
8. Marshal stores to SWMR:
   - Stream: "cardiac_scan"
   - File: /session-data/run_20260129_123456/mrd/cardiac_scan.mrd
   - Append frame 42
   - Flush to disk
```

### HTTP Response (Marshal → Client)

```http
HTTP/1.1 201 Created
Content-Type: application/json

{
  "status": "reconstructed_and_stored",
  "path": "/session-data/run_20260129_123456/mrd/cardiac_scan.mrd",
  "frame_index": 42,
  "stream": "cardiac_scan",
  "reconstructed": true
}
```

### Visual Flow

```
Client → Marshal → Reconstruction Service → Marshal → Storage → Client
  │         │              │                    │        │          │
  1. POST   2. Detect      5. POST              6. HTTP  8. Save    10. HTTP
     raw       type            /reconstruct        200      to         201
     k-space   =ACQU.          (forward)           OK       SWMR       Created
               3. Check                         7. Parse
                  recon=T                          response
               4. Forward
```

---

## Example 4: `/v1/mrd/ingest` + HDF5 Complete File

**Use Case:** Batch upload of complete scan (expected/normal use of /ingest)

### HTTP Request

```http
POST /v1/mrd/ingest HTTP/1.1
Host: localhost:8080
Content-Type: application/octet-stream
Content-Length: 52428800

[Binary Body: Complete HDF5 ISMRMRD file - 50 MB]
```

### Complete Flow

```
1. Marshal receives HTTP POST with 52428800 bytes
2. Detect type: Bytes 0-7 = HDF5 signature
   → Result: MrdDataType::HDF5_FILE
3. Route: HDF5_FILE → Save as complete file (expected for /ingest)
4. Generate filename: 2026-01-29_000042.mrd
5. Write to: /session-data/run_20260129_123456/mrd/2026-01-29_000042.mrd
```

### HTTP Response

```http
HTTP/1.1 201 Created
Content-Type: application/json

{
  "path": "/session-data/run_20260129_123456/mrd/2026-01-29_000042.mrd",
  "size_bytes": 52428800,
  "type": "mrd"
}
```

---

## Example 5: `/v1/mrd/ingest` + Reconstructed Image

**Use Case:** Single image sent to batch endpoint (unusual, but allowed)

### HTTP Request

```http
POST /v1/mrd/ingest HTTP/1.1
Host: localhost:8080
X-MRD-Stream: cardiac_scan
Content-Type: application/octet-stream
Content-Length: 262484

[Binary Body: ImageHeader (198 bytes) + pixel data (262144 bytes)]
```

### Complete Flow

```
1. Marshal receives HTTP POST with 262484 bytes
2. Detect type: ImageHeader detected
   → Result: MrdDataType::IMAGE
3. Route: IMAGE → Store to SWMR (allowed but unusual for /ingest)
4. Open/create: /session-data/run_20260129_123456/mrd/cardiac_scan.mrd
5. Append frame 42 to HDF5 file
6. Flush to disk
```

### HTTP Response

```http
HTTP/1.1 201 Created
Content-Type: application/json

{
  "path": "/session-data/run_20260129_123456/mrd/cardiac_scan.mrd",
  "frame_index": 42,
  "warning": "Single images should use /v1/mrd/frame for streaming",
  "note": "Use /v1/mrd/ingest for complete HDF5 files"
}
```

---

## Example 6A: `/v1/mrd/ingest` + Raw K-Space (NO Reconstruction)

**Use Case:** Batch raw k-space sent but reconstruction not configured

### HTTP Request

```http
POST /v1/mrd/ingest HTTP/1.1
Host: localhost:8080
X-MRD-Stream: cardiac_scan
Content-Type: application/octet-stream
Content-Length: 33108

[Binary Body: AcquisitionHeader (340 bytes) + k-space samples (32768 bytes)]
```

### Complete Flow

```
1. Marshal receives HTTP POST with 33108 bytes
2. Detect type: AcquisitionHeader detected
   → Result: MrdDataType::ACQUISITION
3. Route: ACQUISITION → Check reconstruction enabled
4. state.recon_enabled = false
```

### HTTP Response

```http
HTTP/1.1 501 Not Implemented
Content-Type: application/json

{
  "error": "reconstruction service not configured",
  "detected_type": "ACQUISITION",
  "message": "Raw k-space detected but no reconstruction service available",
  "hint": "Start marshal with --recon-endpoint http://localhost:9002"
}
```

---

## Example 6B: `/v1/mrd/ingest` + Raw K-Space (WITH Reconstruction)

**Use Case:** Batch raw k-space sent, reconstructed and saved as complete file

### Marshal Configuration

```bash
./marshal --recon-endpoint http://reconstruction-service:9002
```

### HTTP Request (Client → Marshal)

```http
POST /v1/mrd/ingest HTTP/1.1
Host: localhost:8080
X-MRD-Stream: cardiac_scan
Content-Type: application/octet-stream
Content-Length: 33108

[Binary Body: AcquisitionHeader (340 bytes) + k-space samples (32768 bytes)]
```

### Complete Flow

```
1. Marshal receives HTTP POST with 33108 bytes (from client)
2. Detect type: Result = MrdDataType::ACQUISITION
3. Route: ACQUISITION → Reconstruction enabled
4. Marshal forwards to reconstruction service:
```

### HTTP Request (Marshal → Reconstruction Service)

```http
POST /reconstruct HTTP/1.1
Host: reconstruction-service:9002
Content-Type: application/octet-stream
Content-Length: 33108

[Body: raw k-space - 33108 bytes]
```

### HTTP Response (Reconstruction Service → Marshal)

```http
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Length: 262484

[Body: ImageHeader (198 bytes) + reconstructed pixels (262144 bytes)]
```

### Processing by Marshal

```
6. Marshal receives reconstruction response (262484 bytes)
7. Marshal parses reconstructed response:
   - Read first 340 bytes as ImageHeader
   - Validate: version=1, matrix_size=[256,256,1], channels=1, data_type=5
   - Extract pixel data from bytes 340-262484
8. Marshal saves as COMPLETE FILE (this is /ingest, not SWMR):
   - Generate filename: cardiac_scan_reconstructed.mrd
   - Create HDF5 file structure
   - Write ImageHeader + pixels as single complete file
   - Save to: /session-data/run_20260129_123456/mrd/cardiac_scan_reconstructed.mrd
```

### HTTP Response (Marshal → Client)

```http
HTTP/1.1 201 Created
Content-Type: application/json

{
  "status": "reconstructed_and_stored",
  "path": "/session-data/run_20260129_123456/mrd/cardiac_scan_reconstructed.mrd",
  "size_bytes": 262484,
  "type": "mrd",
  "message": "Raw k-space reconstructed and saved as complete file"
}
```

### Visual Flow

```
Client → Marshal → Reconstruction Service → Marshal → Storage → Client
  │         │              │                    │        │          │
  1. POST   2. Detect      4. POST              6. HTTP  8. Save    9. HTTP
     raw       type            /reconstruct        200      complete   201
     k-space   =ACQU.          (forward)           OK       file       Created
               3. Check                         7. Parse
                  recon=T                          response
```

**Key Difference from Example 3B:**
- Example 3B (`/frame`): Reconstructed data → **SWMR append** (streaming)
- Example 6B (`/ingest`): Reconstructed data → **Complete file** (batch)

---

## Example 7: Invalid/Unknown Data

**Use Case:** Corrupted or unsupported data format

### HTTP Request

```http
POST /v1/mrd/frame HTTP/1.1
Host: localhost:8080
Content-Type: application/octet-stream
Content-Length: 1024

[Binary Body: Random/corrupted data - not valid ISMRMRD]
```

### Complete Flow

```
1. Marshal receives HTTP POST with 1024 bytes
2. Detect type:
   - Not HDF5 signature ✗
   - Not valid AcquisitionHeader ✗
   - Not valid ImageHeader ✗
   → Result: MrdDataType::UNKNOWN
3. Route: UNKNOWN → Reject
```

### HTTP Response

```http
HTTP/1.1 400 Bad Request
Content-Type: application/json

{
  "error": "invalid_data_format",
  "message": "Could not detect valid ISMRMRD format",
  "expected": "ImageHeader, AcquisitionHeader, or HDF5 file",
  "received_bytes": 1024
}
```

---

## Summary Table

| # | Endpoint | Data Type | Recon Config | Result | HTTP Status |
|---|----------|-----------|--------------|--------|-------------|
| 1 | /frame | IMAGE | N/A | Store to SWMR | 200 OK |
| 2 | /frame | HDF5 | N/A | Forward to ingest logic | 201 Created |
| 3A | /frame | ACQUISITION | No | Not configured error | 501 Not Implemented |
| 3B | /frame | ACQUISITION | Yes | Recon → SWMR | 201 Created |
| 4 | /ingest | HDF5 | N/A | Save complete file | 201 Created |
| 5 | /ingest | IMAGE | N/A | SWMR with warning | 201 Created |
| 6A | /ingest | ACQUISITION | No | Not configured error | 501 Not Implemented |
| 6B | /ingest | ACQUISITION | Yes | Recon → Complete file | 201 Created |
| 7 | either | UNKNOWN | N/A | Reject as invalid | 400 Bad Request |

---

## Key Architectural Points

### 1. Synchronous HTTP Flow

All operations are **synchronous**:
- Client sends request → Waits for response
- Marshal may forward to reconstruction → Waits for response
- Marshal stores data → Returns response to client
- **No async queuing, no background processing, no WebSocket notifications**

### 2. Three-Party Communication Pattern

```
Client ↔ Marshal ↔ Reconstruction Service
```

- Client never talks directly to reconstruction service
- Reconstruction service never talks directly to client
- Marshal is the intermediary for all operations

### 3. Storage Distinction

| Endpoint | Storage Method | Use Case |
|----------|---------------|----------|
| `/v1/mrd/frame` | SWMR (frame-by-frame append) | Real-time streaming |
| `/v1/mrd/ingest` | Complete file (batch save) | Offline batch uploads |

**After reconstruction:**
- From `/frame` → Append to SWMR (streaming mode)
- From `/ingest` → Save as complete file (batch mode)

### 4. Automatic Type Detection

The marshal inspects binary headers to determine data type:
- **HDF5**: First 8 bytes = `89 48 44 46 0D 0A 1A 0A`
- **ImageHeader**: Version + matrix_size + channels + data_type (340 bytes)
- **AcquisitionHeader**: Version + number_of_samples + active_channels (340 bytes)

No client-side declaration needed - fully automatic.

---

## Testing Examples

### Test Example 1: Verify Reconstruction Forwarding

```bash
# Terminal 1: Start reconstruction service
docker compose -f docker-compose.recon.yml up reconstruction-service

# Terminal 2: Start marshal with reconstruction
docker compose -f docker-compose.recon.yml up mri-marshal

# Terminal 3: Send raw k-space
curl -X POST http://localhost:8080/v1/mrd/frame \
  -H "X-MRD-Stream: test_scan" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @test_data/raw_kspace.bin

# Expected: HTTP 201 with "reconstructed_and_stored"
```

### Test Example 2: Verify Detection

```bash
# Send reconstructed image
curl -X POST http://localhost:8080/v1/mrd/frame \
  -H "X-MRD-Stream: test" \
  --data-binary @test_data/reconstructed.bin

# Expected: HTTP 200 (stored directly)

# Send raw k-space without reconstruction
./marshal --data ./data  # No --recon-endpoint
curl -X POST http://localhost:8080/v1/mrd/frame \
  --data-binary @test_data/raw_kspace.bin

# Expected: HTTP 501 (not configured)
```

---

## References

- **Implementation:** [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)
- **Handoff for Phase 2:** [HANDOFF_RECONSTRUCTION_INTEGRATION.md](HANDOFF_RECONSTRUCTION_INTEGRATION.md)
- **Docker Setup:** [DOCKER_RECONSTRUCTION_GUIDE.md](DOCKER_RECONSTRUCTION_GUIDE.md)
- **Quick Overview:** [MRI_MARSHAL_QUICK_OVERVIEW.md](MRI_MARSHAL_QUICK_OVERVIEW.md)
