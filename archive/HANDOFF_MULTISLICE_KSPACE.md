# Handoff: Multi-Slice K-Space and Reconstruction

## Context

Currently, the k-space streamer and mock reconstruction service work with **single 2D slices**. For realistic MRI workflows, we need to support **multi-slice 3D volumes** where:
- K-space streamer sends multiple slices worth of k-space data
- Reconstruction service processes all slices and returns a 3D volume
- Marshal stores the complete 3D volume to SWMR

## Current Behavior

### K-Space Streamer
```cpp
// Sends single 2D slice per frame
AcquisitionHeader header;
header.number_of_samples = 256;      // Readout samples
header.active_channels = 1;          // Single coil
// Implicit: 1 slice only
```

### Mock Recon Service
```python
# Returns single 2D reconstructed image
matrix_x, matrix_y, matrix_z = 64, 64, 1  # Only 1 slice
```

## Requirement

Support **3D multi-slice acquisition and reconstruction**:
- K-space streamer: Send k-space data for **N slices** (configurable, default: 5)
- Mock recon: Reconstruct to **3D volume** with same N slices
- Marshal: Store as 3D volume in SWMR (already supports this)

## Design Options

### Option A: Single Request with All Slices (Recommended)

**Approach:** Send all k-space slices in one POST request

**Advantages:**
- Simpler flow (one callback per volume)
- Matches real scanner behavior (volume acquisition)
- Better for 3D reconstruction algorithms

**K-Space Streamer Changes:**
```cpp
// Create multi-slice k-space
const int num_slices = 5;
std::vector<uint8_t> all_kspace;

for (int slice = 0; slice < num_slices; slice++) {
    ISMRMRD::AcquisitionHeader header;
    header.number_of_samples = 256;
    header.active_channels = 1;
    header.idx.slice = slice;           // Slice index
    header.idx.kspace_encode_step_1 = 0; // Phase encode line

    // Append header + samples
    all_kspace.insert(all_kspace.end(),
                      reinterpret_cast<uint8_t*>(&header),
                      reinterpret_cast<uint8_t*>(&header) + sizeof(header));

    // Append k-space samples (complex float)
    std::vector<std::complex<float>> samples(256, {0.5f, 0.25f});
    all_kspace.insert(all_kspace.end(),
                      reinterpret_cast<uint8_t*>(samples.data()),
                      reinterpret_cast<uint8_t*>(samples.data()) + samples.size() * sizeof(std::complex<float>));
}

// Send entire volume at once
POST /v1/mrd/frame
X-MRD-Stream: raw_scan
Body: all_kspace (N acquisitions concatenated)
```

**Mock Recon Changes:**
```python
def reconstruct():
    raw_kspace = request.data
    callback_url = request.headers.get('X-MRD-Callback')

    # Parse multiple acquisitions
    acquisitions = parse_all_acquisitions(raw_kspace)
    num_slices = len(set(acq.idx.slice for acq in acquisitions))

    print(f"[mock-recon] Received {len(acquisitions)} k-space readouts for {num_slices} slices")

    # Spawn thread to reconstruct 3D volume
    thread = Thread(target=process_and_callback_3d,
                    args=(acquisitions, callback_url, stream_name, num_slices))
    thread.start()

    return jsonify({"status": "processing", "slices": num_slices}), 202

def process_and_callback_3d(acquisitions, callback_url, stream_name, num_slices):
    time.sleep(0.5)  # Simulate 3D FFT

    # Create 3D reconstructed volume
    matrix_x, matrix_y, matrix_z = 64, 64, num_slices
    header = create_image_header(matrix_x, matrix_y, matrix_z, channels=1)

    # Generate 3D gradient pattern
    pixels = create_3d_gradient(matrix_x, matrix_y, matrix_z)

    # POST 3D volume to callback
    requests.post(callback_url, data=header + pixels, headers={...})
```

---

### Option B: Multiple Requests (One Per Slice)

**Approach:** Send each slice as separate POST request

**Advantages:**
- Simpler parsing (one acquisition per request)
- Easier to debug

**Disadvantages:**
- N callbacks (harder to manage)
- Doesn't match real scanner behavior
- Harder to do 3D reconstruction algorithms

**Not recommended** - use Option A instead.

---

## Implementation Plan (Option A)

### 1. K-Space Streamer Changes

**File:** `.worktrees/mri_data_marshal/clients/kspace_streamer/kspace_streamer_main.cpp`

**Add environment variable:**
```cpp
const char* num_slices_env = std::getenv("KSPACE_SLICES");
const int num_slices = num_slices_env ? std::atoi(num_slices_env) : 5;
```

**Modify send loop:**
```cpp
while (running) {
    std::vector<uint8_t> volume_kspace;

    // Build multi-slice k-space volume
    for (int slice = 0; slice < num_slices; slice++) {
        ISMRMRD::AcquisitionHeader header{};
        header.version = 1;
        header.number_of_samples = 256;
        header.available_channels = 1;
        header.active_channels = 1;
        header.trajectory_dimensions = 0;
        header.idx.slice = slice;
        header.idx.kspace_encode_step_1 = 0;  // Single phase encode for simplicity

        // Append header
        volume_kspace.insert(volume_kspace.end(),
                           reinterpret_cast<uint8_t*>(&header),
                           reinterpret_cast<uint8_t*>(&header) + sizeof(header));

        // Append k-space samples
        std::vector<std::complex<float>> samples(256);
        for (auto& s : samples) {
            s = {0.5f, 0.25f};  // Simple test pattern
        }
        volume_kspace.insert(volume_kspace.end(),
                           reinterpret_cast<uint8_t*>(samples.data()),
                           reinterpret_cast<uint8_t*>(samples.data()) + samples.size() * sizeof(std::complex<float>));
    }

    // Send entire volume
    http::request<http::vector_body<uint8_t>> req{http::verb::post, "/v1/mrd/frame", 11};
    req.set(http::field::host, marshal_host);
    req.set(http::field::content_type, "application/octet-stream");
    req.set("X-MRD-Stream", "raw_scan");
    req.body() = std::move(volume_kspace);
    req.prepare_payload();

    http::write(stream, req);
    // ...
}
```

---

### 2. Mock Recon Service Changes

**File:** `.worktrees/mri_data_marshal/tests/mock_recon_service.py`

**Add helper to parse acquisitions:**
```python
ACQUISITION_HEADER_SIZE = 340

def parse_acquisitions(raw_data):
    """Parse multiple ISMRMRD acquisitions from binary data."""
    acquisitions = []
    offset = 0

    while offset < len(raw_data):
        if offset + ACQUISITION_HEADER_SIZE > len(raw_data):
            break

        # Parse header
        header_bytes = raw_data[offset:offset + ACQUISITION_HEADER_SIZE]

        # Extract key fields
        version = struct.unpack_from('<H', header_bytes, 0)[0]
        number_of_samples = struct.unpack_from('<H', header_bytes, 34)[0]
        active_channels = struct.unpack_from('<H', header_bytes, 38)[0]
        slice_idx = struct.unpack_from('<H', header_bytes, 242)[0]  # idx.slice

        # Calculate sample data size
        samples_size = number_of_samples * active_channels * 8  # complex float = 8 bytes

        acquisitions.append({
            'header': header_bytes,
            'slice': slice_idx,
            'samples': number_of_samples,
            'channels': active_channels,
            'data_size': samples_size
        })

        offset += ACQUISITION_HEADER_SIZE + samples_size

    return acquisitions
```

**Update process_and_callback:**
```python
def process_and_callback(raw_kspace, callback_url, stream_name, session_id, job_id):
    """Process multi-slice k-space and callback with 3D volume."""
    try:
        print(f"[mock-recon] [{job_id}] Starting reconstruction...")

        # Parse all acquisitions
        acquisitions = parse_acquisitions(raw_kspace)
        num_slices = len(set(acq['slice'] for acq in acquisitions))

        print(f"[mock-recon] [{job_id}] Parsed {len(acquisitions)} acquisitions, {num_slices} slices")

        # Simulate 3D reconstruction time
        time.sleep(0.5)

        # Create 3D reconstructed volume
        matrix_x, matrix_y, matrix_z = 64, 64, num_slices
        channels = 1

        header = create_image_header(matrix_x, matrix_y, matrix_z, channels)
        pixels = create_3d_gradient(matrix_x, matrix_y, matrix_z)
        reconstructed = header + pixels

        print(f"[mock-recon] [{job_id}] Reconstruction complete: {matrix_x}x{matrix_y}x{matrix_z}")
        print(f"[mock-recon] [{job_id}] Sending callback to {callback_url}")

        # POST to callback
        response = requests.post(
            callback_url,
            data=reconstructed,
            headers={
                'Content-Type': 'application/octet-stream',
                'X-MRD-Stream': stream_name,
                'X-MRD-Session': session_id or '',
                'X-MRD-Job-Id': job_id
            },
            timeout=30
        )

        if response.status_code == 200:
            print(f"[mock-recon] [{job_id}] Callback successful")
        else:
            print(f"[mock-recon] [{job_id}] Callback failed: {response.status_code}")

    except Exception as e:
        print(f"[mock-recon] [{job_id}] ERROR: {e}")

def create_3d_gradient(matrix_x, matrix_y, matrix_z):
    """Create 3D gradient pattern for visual verification."""
    num_pixels = matrix_x * matrix_y * matrix_z
    pixels = bytearray(num_pixels * 4)  # float32

    for z in range(matrix_z):
        for y in range(matrix_y):
            for x in range(matrix_x):
                idx = ((z * matrix_y + y) * matrix_x + x) * 4
                # Gradient varies by slice
                value = (x + y + z * 20) / (matrix_x + matrix_y + matrix_z * 20 - 2)
                struct.pack_into('<f', pixels, idx, value)

    return bytes(pixels)
```

---

### 3. Environment Variable Configuration

**File:** `.env.demo`

Add:
```bash
# K-Space streaming configuration
KSPACE_INTERVAL=0.1       # Seconds between volumes (10 volumes/sec)
KSPACE_SLICES=5           # Number of slices per volume
```

**File:** `docker-compose.demo.yml`

Update kspace-streamer service:
```yaml
kspace-streamer:
  image: cwru/kspace-streamer:latest
  container_name: cwru-kspace-streamer
  networks:
    - cwru-demo-net
  environment:
    - MARSHAL_ENDPOINT=http://mri-marshal:8080
    - STREAM_NAME=raw_scan
    - INTERVAL=${KSPACE_INTERVAL:-0.1}
    - KSPACE_SLICES=${KSPACE_SLICES:-5}  # NEW
    - LOG_STRIDE=10
  depends_on:
    mri-marshal:
      condition: service_healthy
```

---

## Testing Plan

### 1. Build Updated Images

```bash
# Rebuild k-space streamer with multi-slice support
docker build -f docker/Dockerfile.kspace-streamer -t cwru/kspace-streamer:latest .worktrees/mri_data_marshal

# Rebuild mock-recon with 3D reconstruction
docker build -f docker/Dockerfile.mock-recon -t cwru/mock-recon:latest .worktrees/mri_data_marshal
```

### 2. Test Multi-Slice Flow

```bash
# Terminal 1: Start marshal
docker compose --env-file .env.demo -f docker-compose.demo.yml up mri-marshal

# Terminal 2: Start mock-recon
docker compose --env-file .env.demo -f docker-compose.demo.yml up mock-recon

# Terminal 3: Start k-space streamer with 10 slices
KSPACE_SLICES=10 docker compose --env-file .env.demo -f docker-compose.demo.yml up kspace-streamer
```

### 3. Verify Output

**Expected logs:**
```
[kspace-streamer] Sending volume 0 (10 slices)
[mock-recon] Received 10 k-space acquisitions for 10 slices
[mock-recon] Reconstruction complete: 64x64x10
[mock-recon] Callback successful: 200
[mri-marshal] Stored reconstructed callback image: 64x64x10
```

**Check stored data:**
```bash
# Get latest frame metadata
curl http://localhost:8080/v1/mrd/latest | jq

# Expected output:
{
  "dims": {
    "x": 64,
    "y": 64,
    "z": 10,  # <-- 10 slices!
    "channels": 1
  },
  "path": "/session-data/.../raw_scan-64x64x10-g0000.mrd"
}
```

---

## Files to Modify

| File | Changes |
|------|---------|
| `clients/kspace_streamer/kspace_streamer_main.cpp` | Add multi-slice k-space generation |
| `tests/mock_recon_service.py` | Parse multiple acquisitions, reconstruct 3D volume |
| `.env.demo` | Add `KSPACE_SLICES` variable |
| `docker-compose.demo.yml` | Pass `KSPACE_SLICES` to kspace-streamer |
| `docs/MANUAL_TERMINAL_SETUP.md` | Document new `KSPACE_SLICES` variable |

---

## Edge Cases to Handle

1. **Mismatched slice counts**: What if acquisitions have gaps in slice indices?
   - Mock recon should use `max(slice_idx) + 1` as number of slices

2. **Very large volumes**: 100 slices = large payload
   - Current design handles this (just bigger POST body)
   - Consider adding size limits if needed

3. **Partial volumes**: What if only some slices arrive?
   - Current async design doesn't support partial reconstruction
   - Either reconstruct all slices or fail (current behavior: fail)

---

## Optional Enhancements

- **Configurable k-space pattern**: Add different patterns (Cartesian, radial, spiral)
- **Realistic noise**: Add Gaussian noise to k-space
- **Progress updates**: Stream partial reconstruction results as slices complete
- **Compression**: Compress large k-space volumes before sending

---

## Summary

**Current:** Single 2D slice (64x64x1)
**Target:** Multi-slice 3D volume (64x64x5, configurable)

**Key Changes:**
- K-space streamer: Send N acquisitions in one POST
- Mock recon: Parse N acquisitions, return 3D volume
- Environment: Add `KSPACE_SLICES` configuration

**Backward Compatible:** Default `KSPACE_SLICES=1` maintains current behavior
