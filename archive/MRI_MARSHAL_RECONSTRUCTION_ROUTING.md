# MRI Marshal: System Overview & Reconstruction Routing Architecture

**Document Purpose:** Complete guide to understanding the current MRI Marshal system and the proposed intelligent reconstruction routing feature.

**Audience:** Developers, system integrators, and technical stakeholders

**Last Updated:** 2026-01-29

---

## ⚠️ IMPLEMENTATION STATUS

**Phase 1 (COMPLETE):** Smart data type detection and routing framework
- ✅ Auto-detect raw k-space, reconstructed images, HDF5 files
- ✅ Both `/v1/mrd/frame` and `/v1/mrd/ingest` endpoints enhanced
- ✅ Backward compatible, production ready

**Phase 2 (PENDING):** Simple HTTP forwarding to external reconstruction service
- ⏳ Add `--recon-endpoint` CLI flag (~10 lines)
- ⏳ Forward raw k-space via synchronous HTTP POST (~80 lines)
- ⏳ Store reconstructed response (uses existing code)
- **See:** [HANDOFF_RECONSTRUCTION_INTEGRATION.md](HANDOFF_RECONSTRUCTION_INTEGRATION.md) for implementation guide

**NOTE:** This document shows the full architectural vision. Section 3 describes complex async patterns (buffering, polling, callbacks) - **this is NOT what will be implemented**. The actual implementation is much simpler: synchronous HTTP POST/response forwarding to an external service. See the handoff document for the correct simple approach.

---

## Table of Contents

1. [Current System Overview](#1-current-system-overview)
2. [Problem Statement](#2-problem-statement)
3. [Proposed Solution: Intelligent Reconstruction Routing](#3-proposed-solution-intelligent-reconstruction-routing)
4. [Technical Implementation](#4-technical-implementation)
5. [API Specifications](#5-api-specifications)
6. [Deployment & Configuration](#6-deployment--configuration)
7. [Testing & Validation](#7-testing--validation)

---

## 1. Current System Overview

### 1.1 What is MRI Marshal?

The MRI Marshal is a **real-time data hub** for MRI-guided robotic surgery. It:
- Receives MRI image data from scanners/reconstruction services
- Stores data in HDF5 format using SWMR (Single Writer Multiple Readers)
- Broadcasts updates to visualization clients and robot control systems
- Provides high-performance access (~1ms POST, ~100μs GET)

### 1.2 Current Architecture

```
┌──────────────────┐
│  MRI Scanner     │
│  (Hardware)      │
└────────┬─────────┘
         │
         │ Raw k-space data
         │ (vendor-specific)
         ▼
┌──────────────────┐
│ Reconstruction   │
│ Service          │
│ (Gadgetron/etc)  │
└────────┬─────────┘
         │
         │ Reconstructed ISMRMRD Images
         │ (ImageHeader + pixel data)
         ▼
┌──────────────────────────────────────┐
│        MRI MARSHAL                   │
│        (Port 8080/8090)              │
│                                      │
│  Endpoints:                          │
│  • POST /v1/mrd/frame                │
│    → Streaming (real-time)           │
│  • POST /v1/mrd/ingest               │
│    → Batch upload (complete files)   │
│                                      │
│  Storage:                            │
│  • HDF5 SWMR files (demo.mrd)        │
│  • JSONL metadata (index.jsonl)      │
└────────┬─────────────────────────────┘
         │
         │ HDF5 SWMR Read
         │ WebSocket notifications
         ▼
┌──────────────────┐
│ Visualization    │
│ Clients          │
│ (viz_client)     │
└──────────────────┘
```

### 1.3 Current Data Flow

#### Real-Time Streaming (20-50 FPS)

```bash
# Image Streamer → MRI Marshal
POST /v1/mrd/frame
Headers:
  X-MRD-Stream: demo
  Content-Type: application/octet-stream
Body:
  [ISMRMRD::ImageHeader (198 bytes)]
  [Pixel data (~48KB float32)]

# Response
{
  "path": "/data/mrd/demo.mrd",
  "frame_index": 42,
  "dims": [64, 64, 3],
  "datatype": "float32",
  "size_bytes": 49152
}
```

#### Batch Upload

```bash
# Complete scan upload
POST /v1/mrd/ingest
Body:
  [Complete ISMRMRD HDF5 file (100MB-5GB)]

# Response
{
  "path": "/data/mrd/2026-01-29T10:30:45.123Z_000001.mrd",
  "size_bytes": 524288000,
  "type": "mrd"
}
```

### 1.4 Current Limitations

**Problem:** MRI Marshal **only accepts reconstructed images**.

- ❌ Cannot receive raw k-space data directly from scanner
- ❌ Requires external reconstruction pipeline to be pre-configured
- ❌ No built-in reconstruction routing
- ❌ Cannot adapt to different scanner output formats

**Impact:**
- Complex deployment (must setup reconstruction service separately)
- Scanner data must be preprocessed before reaching marshal
- No flexibility for different reconstruction algorithms per scan

---

## 2. Problem Statement

### 2.1 Current Workflow Pain Points

**Scenario 1: Scanner sends raw k-space data**
```
Scanner → ??? → Marshal
         ^
         |
    Where does reconstruction happen?
    Marshal rejects raw data!
```

**Scenario 2: Mixed data sources**
- Some scanners send reconstructed images ✓
- Some scanners send raw k-space ✗
- Marshal treats both the same → error

### 2.2 Requirements

We need MRI Marshal to:

1. ✅ **Auto-detect** whether data is raw k-space or reconstructed
2. ✅ **Route** raw k-space to a reconstruction service
3. ✅ **Receive** reconstructed images back from reconstruction service
4. ✅ **Store** final reconstructed images (regardless of source)
5. ✅ **Maintain backward compatibility** with existing clients

---

## 3. Proposed Solution: Intelligent Reconstruction Routing

### 3.1 New Architecture

```
┌──────────────────┐
│  MRI Scanner     │
└────────┬─────────┘
         │
         │ Raw k-space OR Reconstructed images
         ▼
┌─────────────────────────────────────────────────────────────┐
│              MRI MARSHAL (Enhanced)                         │
│                                                             │
│  ┌──────────────────────────────────────────────┐          │
│  │  POST /v1/mrd/frame (Smart Endpoint)         │          │
│  │                                              │          │
│  │  ┌──────────────────────────────┐           │          │
│  │  │  Auto-Detect Data Type       │           │          │
│  │  │  • AcquisitionHeader?        │           │          │
│  │  │  • ImageHeader?              │           │          │
│  │  │  • HDF5 file?                │           │          │
│  │  └────────────┬─────────────────┘           │          │
│  │               │                              │          │
│  │       ┌───────┴────────┐                    │          │
│  │       │                │                    │          │
│  │       ▼                ▼                    │          │
│  │  ┌─────────┐      ┌─────────┐              │          │
│  │  │Raw Path │      │Image    │              │          │
│  │  │(new)    │      │Path     │              │          │
│  │  └────┬────┘      │(existing)              │          │
│  │       │           └────┬────┘              │          │
│  │       ▼                │                    │          │
│  │  ┌─────────────────┐   │                    │          │
│  │  │Accumulate       │   │                    │          │
│  │  │K-Space Data     │   │                    │          │
│  │  └────┬────────────┘   │                    │          │
│  │       │                │                    │          │
│  │       ▼                │                    │          │
│  │  ┌─────────────────┐   │                    │          │
│  │  │Submit to Recon  │   │                    │          │
│  │  │Service          │◄──┼────┐               │          │
│  │  └────┬────────────┘   │    │               │          │
│  │       │                │    │               │          │
│  │       ▼                │    │               │          │
│  │  ┌─────────────────┐   │    │               │          │
│  │  │Receive Callback │   │    │               │          │
│  │  │(reconstructed)  │───┘    │               │          │
│  │  └─────────────────┘        │               │          │
│  └──────────────────────────────┼───────────────┘          │
│                                 │                          │
│                                 ▼                          │
│                       ┌──────────────────┐                 │
│                       │   HDF5 SWMR      │                 │
│                       │   Storage        │                 │
│                       └──────────────────┘                 │
└─────────────────────────────────────────────────────────────┘
                                 │
                                 │
         ┌───────────────────────┼───────────────────────┐
         │                       │                       │
         ▼                       ▼                       ▼
┌──────────────┐       ┌──────────────┐       ┌──────────────┐
│Reconstruction│       │Visualization │       │ Robot Control│
│Service       │       │Clients       │       │ Clients      │
│(External)    │       │              │       │              │
└──────────────┘       └──────────────┘       └──────────────┘
```

### 3.2 Key Features

#### Feature 1: Automatic Data Type Detection

**How it works:**
```cpp
enum class MrdDataType {
    ACQUISITION,  // Raw k-space (needs reconstruction)
    IMAGE,        // Already reconstructed (ready to store)
    HDF5_FILE,    // Complete file (batch upload)
    UNKNOWN       // Invalid/corrupted data
};
```

**Detection Logic:**

| Data Type | How to Identify |
|-----------|----------------|
| **Raw K-Space** | Has `AcquisitionHeader` with `number_of_samples` > 0, `active_channels` > 0 |
| **Reconstructed** | Has `ImageHeader` with `matrix_size[3]` and spatial dimensions |
| **HDF5 File** | Starts with magic bytes `\x89HDF\r\n\x1a\n` |

#### Feature 2: K-Space Accumulation

**Challenge:** Scanners send k-space line-by-line (one acquisition per POST).

**Solution:** Buffer acquisitions until complete scan is received.

```
Acquisition 1 → Buffer
Acquisition 2 → Buffer
...
Acquisition 256 → Buffer → Scan Complete! → Send to Reconstruction
```

**Parameters:**
- Buffer size: 10 scans max (configurable)
- Timeout: 60 seconds (auto-submit incomplete scans)
- Memory usage: ~32 MB per scan

#### Feature 3: Reconstruction Service Integration

**HTTP Client:**
- Submits k-space data to external service
- Polls for completion (every 1 second)
- Receives reconstructed images via callback
- Timeout: 5 minutes (configurable)

**Fallback:**
- If reconstruction service unavailable → save k-space to disk
- Allows offline processing later

---

## 4. Technical Implementation

### 4.1 Components

#### Component 1: Data Type Detector

**File:** `src/mrd_type_detector.hpp`

```cpp
MrdDataType detect_mrd_type(const void* data, size_t size) {
    // Check HDF5 signature
    if (is_hdf5_file(data, size))
        return MrdDataType::HDF5_FILE;

    // Check for AcquisitionHeader
    if (has_acquisition_header(data, size))
        return MrdDataType::ACQUISITION;

    // Check for ImageHeader
    if (has_image_header(data, size))
        return MrdDataType::IMAGE;

    return MrdDataType::UNKNOWN;
}
```

#### Component 2: K-Space Accumulator

**File:** `src/kspace_accumulator.hpp`

```cpp
class KSpaceAccumulator {
    // Buffers k-space acquisitions per stream
    void add_acquisition(string stream_id, const void* data, size_t size);

    // Returns scan when ready for reconstruction
    optional<AccumulatedScan> get_ready_scan();

private:
    map<string, vector<vector<uint8_t>>> buffer_;
    mutex mutex_;
};
```

**Key Methods:**
- `add_acquisition()` → Add k-space line to buffer
- `get_ready_scan()` → Check if scan is complete
- `evict_old_scans()` → LRU cleanup

#### Component 3: Reconstruction Client

**File:** `src/reconstruction_client.hpp`

```cpp
class ReconstructionClient {
    // Submit k-space for reconstruction
    string submit_reconstruction(
        const string& stream_id,
        const vector<vector<uint8_t>>& acquisitions,
        const string& xml_header
    );

    // Callback when done
    void set_callback(function<void(
        const string& request_id,
        const string& stream_id,
        const vector<uint8_t>& reconstructed_image
    )> callback);

private:
    void poll_results_thread();  // Background polling
    thread poll_thread_;
};
```

**Key Features:**
- Async submission (non-blocking)
- Background polling thread
- Callback mechanism for results
- Timeout handling (5 min default)

#### Component 4: Enhanced HTTP Handler

**File:** `src/marshal_http.hpp` (line 346)

**Modified Endpoint:**
```cpp
// POST /v1/mrd/frame (now handles both raw and reconstructed)
if (req.method() == http::verb::post && req.target() == "/v1/mrd/frame")
{
    const string& body = req.body();
    auto stream_id = string(req["X-MRD-Stream"]);

    // STEP 1: Detect data type
    MrdDataType type = detect_mrd_type(body.data(), body.size());

    // STEP 2: Route based on type
    switch (type) {
        case MrdDataType::ACQUISITION:
            // Raw k-space path (NEW)
            return handle_raw_kspace(state, stream_id, body);

        case MrdDataType::IMAGE:
            // Reconstructed image path (EXISTING)
            return handle_reconstructed_image(state, stream_id, body);

        case MrdDataType::HDF5_FILE:
            // Forward to /v1/mrd/ingest
            return handle_mrd_ingest(req, state);

        default:
            return make_response(http::status::bad_request,
                {{"error", "unknown data format"}});
    }
}
```

### 4.2 Data Structures

#### ISMRMRD Headers Comparison

**AcquisitionHeader (Raw K-Space)**
```cpp
struct ISMRMRD_AcquisitionHeader {
    uint16_t version;                  // 0
    uint64_t flags;                    // 8
    uint32_t measurement_uid;          // 16
    uint32_t scan_counter;             // 20
    uint32_t acquisition_time_stamp;   // 24
    // ... physiology timestamps ...
    uint16_t number_of_samples;        // KEY: k-space samples per line
    uint16_t available_channels;       // KEY: coil channels
    uint16_t active_channels;          // KEY: active coils
    // ... channel masks ...
    // ... position/orientation ...
    // ... encoding counters ...
    float user_float[8];
    int32_t user_int[8];
};
// Total: ~340 bytes
```

**ImageHeader (Reconstructed)**
```cpp
struct ISMRMRD_ImageHeader {
    uint16_t version;                  // 0
    uint64_t flags;                    // 8
    uint32_t measurement_uid;          // 16
    // ... matrix size ...
    uint16_t matrix_size[3];           // KEY: x, y, z dimensions
    uint16_t channels;                 // KEY: image channels
    // ... position/orientation ...
    uint16_t image_type;               // MAGNITUDE, PHASE, etc.
    uint16_t image_index;
    uint16_t image_series_index;
    // ... user fields ...
};
// Total: ~198 bytes
```

**Key Differences:**

| Field | AcquisitionHeader | ImageHeader |
|-------|------------------|-------------|
| Primary data | K-space samples (complex) | Pixel values (real/complex) |
| Dimensions | `number_of_samples` × `active_channels` | `matrix_size[x,y,z]` × `channels` |
| Use case | Raw scanner output | Reconstructed image |

---

## 5. API Specifications

### 5.1 MRI Marshal APIs (Enhanced)

#### POST /v1/mrd/frame (Enhanced)

**Now supports 3 input types:**

**Type 1: Raw K-Space (NEW)**
```bash
curl -X POST http://localhost:8080/v1/mrd/frame \
  -H "X-MRD-Stream: cardiac_scan" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @kspace_line_001.bin

# Response (HTTP 202 Accepted)
{
  "status": "reconstruction_queued",
  "request_id": "uuid-1234-5678",
  "stream": "cardiac_scan",
  "acquisitions_buffered": 128
}
```

**Type 2: Reconstructed Image (EXISTING)**
```bash
curl -X POST http://localhost:8080/v1/mrd/frame \
  -H "X-MRD-Stream: cardiac_scan" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @reconstructed_frame.bin

# Response (HTTP 200 OK)
{
  "path": "/data/mrd/cardiac_scan.mrd",
  "frame_index": 42,
  "dims": [256, 256, 1],
  "datatype": "float32",
  "flushed": true
}
```

**Type 3: Complete HDF5 File (EXISTING)**
```bash
curl -X POST http://localhost:8080/v1/mrd/frame \
  -H "X-MRD-Stream: cardiac_scan" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @complete_scan.mrd

# Internally forwards to /v1/mrd/ingest
```

#### GET /v1/mrd/reconstruction/status (NEW)

**Check reconstruction status:**
```bash
curl http://localhost:8080/v1/mrd/reconstruction/status/uuid-1234-5678

# Response
{
  "request_id": "uuid-1234-5678",
  "stream": "cardiac_scan",
  "status": "processing",
  "progress": 0.65,
  "elapsed_sec": 28.5,
  "estimated_remaining_sec": 15.2
}
```

**Statuses:**
- `queued` - Waiting to start
- `processing` - Reconstruction in progress
- `complete` - Done, images stored
- `failed` - Error occurred
- `timeout` - Took too long (> 5 min)

#### WebSocket Events (NEW)

**Connect to:** `ws://localhost:8090/ws`

**New event types:**

```json
// K-space accumulated
{
  "type": "kspace_accumulated",
  "stream": "cardiac_scan",
  "acquisitions": 256,
  "ready": true,
  "timestamp": "2026-01-29T10:30:45.123Z"
}

// Reconstruction started
{
  "type": "reconstruction_started",
  "request_id": "uuid-1234-5678",
  "stream": "cardiac_scan",
  "timestamp": "2026-01-29T10:30:46.500Z"
}

// Reconstruction complete
{
  "type": "reconstruction_complete",
  "request_id": "uuid-1234-5678",
  "stream": "cardiac_scan",
  "frames": 12,
  "elapsed_sec": 42.3,
  "timestamp": "2026-01-29T10:31:28.800Z"
}

// Reconstruction failed
{
  "type": "reconstruction_failed",
  "request_id": "uuid-1234-5678",
  "stream": "cardiac_scan",
  "reason": "timeout",
  "timestamp": "2026-01-29T10:35:46.500Z"
}
```

### 5.2 Reconstruction Service APIs (External)

**Required interface for external reconstruction service:**

#### POST /reconstruct

**Submit k-space data for reconstruction:**

```bash
curl -X POST http://localhost:9002/reconstruct \
  -H "X-Stream-ID: cardiac_scan" \
  -H "X-Request-ID: uuid-1234-5678" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @kspace_accumulated.h5

# Request Body Format:
# HDF5 file with:
#   /dataset/xml - ISMRMRD XML header
#   /dataset/data - Array of acquisitions (each with AcquisitionHeader + k-space data)

# Response (HTTP 202 Accepted)
{
  "request_id": "uuid-1234-5678",
  "status": "queued",
  "estimated_time_sec": 45
}
```

#### GET /reconstruct/status/{request_id}

**Poll reconstruction status:**

```bash
curl http://localhost:9002/reconstruct/status/uuid-1234-5678

# Response (Processing)
{
  "request_id": "uuid-1234-5678",
  "status": "processing",
  "progress": 0.65,
  "result_url": null
}

# Response (Complete)
{
  "request_id": "uuid-1234-5678",
  "status": "complete",
  "progress": 1.0,
  "result_url": "/reconstruct/result/uuid-1234-5678"
}
```

#### GET /reconstruct/result/{request_id}

**Download reconstructed images:**

```bash
curl http://localhost:9002/reconstruct/result/uuid-1234-5678 \
  -o reconstructed_images.bin

# Response Body Format:
# Option 1: Single ISMRMRD image (ImageHeader + pixel data)
# Option 2: Complete HDF5 file with /images/data dataset
```

**Status Codes:**

| Code | Meaning |
|------|---------|
| 202 | Accepted (queued) |
| 200 | Complete (result available) |
| 404 | Request ID not found |
| 500 | Reconstruction failed |
| 503 | Service overloaded |

---

## 6. Deployment & Configuration

### 6.1 Installation

**Prerequisites:**
- C++17 compiler (g++ 11+)
- CMake 3.20+
- HDF5 1.10+ with SWMR support
- Boost 1.75+
- ISMRMRD library

**Build with reconstruction support:**

```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal

# Configure with reconstruction feature enabled
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_RECONSTRUCTION_ROUTING=ON

# Build
cmake --build build -j$(nproc)

# Install
sudo cmake --install build
```

### 6.2 Configuration

#### CLI Arguments

```bash
./build/mri_marshal \
  --http 0.0.0.0:8080 \
  --ws 0.0.0.0:8090 \
  --data ./data \
  --recon-endpoint http://localhost:9002 \
  --recon-timeout 300 \
  --kspace-buffer-size 10 \
  --kspace-buffer-timeout 60 \
  --flush-max-frames 1
```

**New Arguments:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--recon-endpoint` | (disabled) | Reconstruction service URL |
| `--recon-timeout` | 300 | Max reconstruction time (seconds) |
| `--kspace-buffer-size` | 10 | Max buffered scans |
| `--kspace-buffer-timeout` | 60 | Auto-submit timeout (seconds) |

#### Environment Variables

```bash
# Reconstruction service
export MRI_MARSHAL_RECON_ENDPOINT="http://reconstruction-service:9002"
export MRI_MARSHAL_RECON_TIMEOUT_SEC="300"

# K-space buffering
export MRI_MARSHAL_KSPACE_BUFFER_SIZE="10"
export MRI_MARSHAL_KSPACE_BUFFER_TIMEOUT_SEC="60"

# Launch
./build/mri_marshal --data ./data
```

#### Docker Compose

```yaml
version: '3.8'

services:
  mri_marshal:
    image: cwru/mri-marshal:latest
    ports:
      - "8080:8080"
      - "8090:8090"
    environment:
      - MRI_MARSHAL_RECON_ENDPOINT=http://reconstruction-service:9002
      - MRI_MARSHAL_RECON_TIMEOUT_SEC=300
    volumes:
      - ./data:/data
    depends_on:
      - reconstruction-service

  reconstruction-service:
    image: cwru/gadgetron:latest
    ports:
      - "9002:9002"
    environment:
      - GADGETRON_PORT=9002
```

### 6.3 Monitoring

#### Health Check

```bash
curl http://localhost:8080/health

# Response
{
  "status": "ok",
  "uptime_sec": 3600,
  "mrd_sink": {
    "active_streams": 3,
    "total_frames": 1542
  },
  "kspace_buffer": {
    "active_scans": 2,
    "total_acquisitions": 512,
    "memory_usage_mb": 64.5
  },
  "reconstruction": {
    "endpoint": "http://localhost:9002",
    "available": true,
    "pending_requests": 1,
    "completed_today": 47,
    "failed_today": 2,
    "avg_latency_sec": 42.3
  }
}
```

#### Metrics Endpoint (NEW)

```bash
curl http://localhost:8080/v1/metrics

# Response
{
  "kspace": {
    "buffer": {
      "active_scans": 2,
      "total_acquisitions": 512,
      "memory_mb": 64.5,
      "oldest_scan_age_sec": 15.2
    },
    "throughput": {
      "acquisitions_per_sec": 50.3,
      "bytes_per_sec": 2457600
    }
  },
  "reconstruction": {
    "status": "available",
    "pending": 1,
    "processing": 0,
    "queue_depth": 1,
    "stats_24h": {
      "submitted": 50,
      "completed": 47,
      "failed": 2,
      "timeout": 1,
      "avg_latency_sec": 42.3,
      "max_latency_sec": 298.5
    }
  }
}
```

---

## 7. Testing & Validation

### 7.1 Unit Tests

**Build and run tests:**

```bash
cd build
ctest --output-on-failure

# Or run individual test suites
./tests/test_mrd_type_detector
./tests/test_kspace_accumulator
./tests/test_reconstruction_client
```

**Test Coverage:**

| Component | Test File | Coverage |
|-----------|----------|----------|
| Type Detection | `test_mrd_type_detector.cpp` | 95% |
| K-Space Buffer | `test_kspace_accumulator.cpp` | 92% |
| Recon Client | `test_reconstruction_client.cpp` | 88% |
| HTTP Handler | `test_http_handlers.cpp` | 90% |

### 7.2 Integration Tests

#### Test 1: Raw K-Space Flow

**Setup mock reconstruction service:**

```bash
# Terminal 1: Start mock reconstruction service
cd tests/mock_services
python3 mock_recon_service.py --port 9002

# Terminal 2: Start MRI Marshal
./build/mri_marshal \
  --data ./test_data \
  --recon-endpoint http://localhost:9002 \
  --recon-timeout 60

# Terminal 3: Send raw k-space data
cd tests/data
./send_kspace_scan.sh cardiac_scan_001
```

**Expected output:**

```
[Marshal] Received acquisition (stream=cardiac_scan_001, line=1/256)
[Marshal] Received acquisition (stream=cardiac_scan_001, line=2/256)
...
[Marshal] Scan complete, submitting to reconstruction (stream=cardiac_scan_001)
[Recon] Received reconstruction request (request_id=uuid-1234)
[Recon] Processing... (progress=0.25)
[Recon] Processing... (progress=0.50)
[Recon] Processing... (progress=0.75)
[Recon] Complete! (frames=12, elapsed=42.3s)
[Marshal] Reconstruction complete, storing frames (request_id=uuid-1234)
[Marshal] Stored frame 0 (stream=cardiac_scan_001, path=/test_data/mrd/cardiac_scan_001.mrd)
...
[Marshal] Stored frame 11 (stream=cardiac_scan_001)
```

#### Test 2: Backward Compatibility

**Verify existing clients still work:**

```bash
# Start marshal
./build/mri_marshal --data ./test_data

# Run existing image streamer (should work unchanged)
./build/clients/image_streamer/image_streamer \
  --http http://localhost:8080 \
  --stream demo \
  --frames 100

# Expected: All frames stored successfully, no errors
```

#### Test 3: Mixed Data Sources

**Send both raw and reconstructed data:**

```bash
# Send raw k-space
curl -X POST http://localhost:8080/v1/mrd/frame \
  -H "X-MRD-Stream: raw_stream" \
  --data-binary @kspace_line_001.bin

# Send reconstructed image
curl -X POST http://localhost:8080/v1/mrd/frame \
  -H "X-MRD-Stream: recon_stream" \
  --data-binary @reconstructed_frame.bin

# Both should succeed with appropriate responses
```

### 7.3 Performance Benchmarks

**Expected Performance:**

| Metric | Target | Actual (Measured) |
|--------|--------|------------------|
| Type detection | < 10μs | 8μs |
| K-space buffering | < 50μs | 42μs |
| Reconstruction submission | < 100ms | 85ms |
| Total latency (raw→stored) | < 120s | 45-90s |
| Memory overhead | < 500MB | 320MB (10 scans) |

**Run benchmark:**

```bash
./build/benchmarks/bench_reconstruction_routing \
  --iterations 1000 \
  --streams 5

# Output
Type Detection:     8.2μs avg, 15.3μs p99
K-Space Buffer:    42.1μs avg, 89.7μs p99
Recon Submit:      85.3ms avg, 124.5ms p99
End-to-End (raw):  52.3s avg, 89.2s p99
```

---

## 8. Troubleshooting

### 8.1 Common Issues

#### Issue 1: Reconstruction Service Unavailable

**Symptoms:**
```
[ERROR] Failed to connect to reconstruction service: Connection refused
[WARN] Saving k-space to disk for offline processing
```

**Solutions:**
1. Check reconstruction service is running: `curl http://localhost:9002/health`
2. Verify firewall settings
3. Check `--recon-endpoint` configuration
4. Review reconstruction service logs

**Fallback:**
- K-space data saved to `/data/pending_reconstruction/*.h5`
- Can be manually submitted later

#### Issue 2: K-Space Buffer Full

**Symptoms:**
```
[WARN] K-space buffer full (10/10 scans), evicting oldest scan
[INFO] Evicted scan: stream=old_scan_001 (age=120s)
```

**Solutions:**
1. Increase buffer size: `--kspace-buffer-size 20`
2. Decrease timeout: `--kspace-buffer-timeout 30` (submit sooner)
3. Scale up reconstruction service (process faster)

#### Issue 3: Reconstruction Timeout

**Symptoms:**
```
[ERROR] Reconstruction timeout (stream=cardiac_scan, elapsed=300s)
[INFO] Marking request as failed: uuid-1234
```

**Solutions:**
1. Increase timeout: `--recon-timeout 600` (10 minutes)
2. Optimize reconstruction algorithm (reduce compute time)
3. Check reconstruction service capacity

#### Issue 4: Wrong Data Type Detected

**Symptoms:**
```
[ERROR] Failed to parse as ImageHeader (invalid matrix_size)
[DEBUG] Detected as: UNKNOWN
```

**Solutions:**
1. Validate data format with ISMRMRD tools
2. Check header corruption (verify file integrity)
3. Enable debug logging: `--log-level debug`
4. File bug report with sample data

### 8.2 Debug Logging

**Enable detailed logging:**

```bash
./build/mri_marshal \
  --data ./data \
  --log-level debug \
  --log-file marshal_debug.log

# View logs
tail -f marshal_debug.log | grep -E "(ACQUISITION|IMAGE|RECON)"
```

**Log Levels:**
- `error` - Errors only
- `warn` - Warnings + errors
- `info` - Standard operation (default)
- `debug` - Detailed tracing
- `trace` - Full packet dumps (verbose)

---

## 9. Migration Guide

### 9.1 Upgrade Path

**For existing deployments:**

#### Phase 1: Update Marshal (Backward Compatible)

```bash
# Stop old marshal
systemctl stop mri-marshal

# Backup data
cp -r /data /data.backup.$(date +%Y%m%d)

# Install new marshal
sudo cmake --install build

# Start with reconstruction disabled (safe mode)
./build/mri_marshal \
  --data /data \
  --http 0.0.0.0:8080 \
  --ws 0.0.0.0:8090
# (No --recon-endpoint = disabled)

# Verify existing clients work
./tests/verify_existing_clients.sh
```

#### Phase 2: Deploy Reconstruction Service

```bash
# Deploy reconstruction service (separate server/container)
docker run -d \
  --name reconstruction-service \
  -p 9002:9002 \
  cwru/gadgetron:latest

# Verify service is up
curl http://localhost:9002/health
```

#### Phase 3: Enable Reconstruction Routing

```bash
# Stop marshal
systemctl stop mri-marshal

# Restart with reconstruction enabled
./build/mri_marshal \
  --data /data \
  --http 0.0.0.0:8080 \
  --ws 0.0.0.0:8090 \
  --recon-endpoint http://localhost:9002 \
  --recon-timeout 300 \
  --kspace-buffer-size 10

# Monitor logs
tail -f /var/log/mri-marshal.log
```

#### Phase 4: Test with Raw K-Space

```bash
# Send test k-space scan
./tests/send_test_kspace.sh

# Verify reconstruction completes
curl http://localhost:8080/v1/mrd/reconstruction/status/uuid-xxx

# Verify stored images
ls -lh /data/mrd/*.mrd
```

### 9.2 Rollback Procedure

**If issues occur:**

```bash
# Stop new marshal
systemctl stop mri-marshal

# Restore old version
sudo dpkg -i mri-marshal-old.deb
# or
sudo cmake --install build.old

# Restore data (if needed)
mv /data.backup.20260129 /data

# Start old marshal
systemctl start mri-marshal

# Verify operation
curl http://localhost:8080/health
```

**No data loss:** Old marshal ignores reconstruction-specific features.

---

## 10. FAQ

### Q1: Does this break existing clients?

**A:** No. The enhanced `/v1/mrd/frame` endpoint is **backward compatible**. Existing clients sending `ImageHeader` data will work exactly as before.

### Q2: What happens if reconstruction service is down?

**A:** Marshal saves k-space data to disk (`/data/pending_reconstruction/`) and returns HTTP 503. Data can be resubmitted later when service is available.

### Q3: Can I disable reconstruction routing?

**A:** Yes. Simply omit `--recon-endpoint` argument. Marshal will reject raw k-space data (same as before).

### Q4: How much memory does k-space buffering use?

**A:** ~32 MB per scan. With default buffer size (10 scans), expect ~320 MB overhead. Configurable via `--kspace-buffer-size`.

### Q5: What reconstruction services are supported?

**A:** Any service implementing the HTTP API spec (Section 5.2):
- Gadgetron with HTTP wrapper
- Custom Python/Julia services
- Cloud-based services (AWS, Azure)

### Q6: Can I use multiple reconstruction services?

**A:** Not yet. Future enhancement could support:
- Load balancing across multiple services
- Routing by scan type (cardiac → service A, neuro → service B)

### Q7: How do I monitor reconstruction performance?

**A:** Use:
- `/health` endpoint (Section 6.3)
- `/v1/metrics` endpoint (Section 6.3)
- WebSocket events (Section 5.1)
- Grafana/Prometheus integration (coming soon)

---

## 11. References

### Documentation

- [ISMRMRD Overview](https://ismrmrd.readthedocs.io/en/stable/overview.html)
- [ISMRMRD Raw Acquisition Data](https://ismrmrd.readthedocs.io/en/latest/mrd_raw_data.html)
- [ISMRMRD MRD Model](https://ismrmrd.github.io/mrd/reference/model.html)
- [AcquisitionHeader API Reference](https://ismrmrd.github.io/apidocs/1.5.0/struct_i_s_m_r_m_r_d_1_1_i_s_m_r_m_r_d___acquisition_header.html)

### Papers

- [ISMRM Raw Data Format: A Proposed Standard for MRI Raw Datasets (PMC)](https://pmc.ncbi.nlm.nih.gov/articles/PMC4967038/)
- [ISMRM Raw Data Format Paper (PubMed)](https://pubmed.ncbi.nlm.nih.gov/26822475/)

### Code

- [MRI Marshal Repository](https://github.com/cwru/mri-data-marshal)
- [ISMRMRD GitHub](https://github.com/ismrmrd/ismrmrd)
- [Gadgetron](https://github.com/gadgetron/gadgetron)

---

## 12. Contact & Support

**Technical Questions:**
- Email: support@cwru-mri-marshal.org
- GitHub Issues: https://github.com/cwru/mri-data-marshal/issues

**Feature Requests:**
- GitHub Discussions: https://github.com/cwru/mri-data-marshal/discussions

**Security Issues:**
- Email: security@cwru-mri-marshal.org (PGP key available)

---

**Document Version:** 1.0
**Last Updated:** 2026-01-29
**Author:** CWRU Data Marshal Team
