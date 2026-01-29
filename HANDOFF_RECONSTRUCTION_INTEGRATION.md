# Handoff: Reconstruction Service Integration

**For:** Next AI Agent
**Status:** Phase 1 Complete (Detection) - Ready for Phase 2 (Integration)
**Priority:** High
**Estimated Effort:** 3-4 weeks

---

## What's Already Done ✅

### Phase 1: Smart Detection (COMPLETE)

Both `/v1/mrd/frame` and `/v1/mrd/ingest` endpoints now automatically detect:
- ✅ Raw k-space data (AcquisitionHeader)
- ✅ Reconstructed images (ImageHeader)
- ✅ Complete HDF5 files

**Files implemented:**
- `include/mrd_type_detector.hpp` - Detection logic
- `src/marshal_http.hpp` - Enhanced endpoints with routing

**Current behavior:**
- Raw k-space → Returns HTTP 501 "Not yet implemented"
- Reconstructed → Stores normally (works perfectly)
- HDF5 file → Stores as-is (works perfectly)

---

## What Needs to Be Done 🔨

### Phase 2: Reconstruction Service Integration

Implement the HTTP client to forward raw k-space data to an external reconstruction service.

---

## Implementation Plan

### Step 1: Create Reconstruction Client (Week 1)

**File:** `include/reconstruction_client.hpp`

**Interface:**
```cpp
namespace mrd {

class ReconstructionClient {
public:
    struct Config {
        std::string endpoint;      // e.g., "http://localhost:9002"
        int timeout_sec = 300;     // 5 minutes
        int poll_interval_ms = 1000; // Check every 1 second
    };

    ReconstructionClient(Config config, MarshalState& state);
    ~ReconstructionClient();

    // Submit k-space data for reconstruction
    // Returns request_id for tracking
    std::string submit(
        const std::string& stream_id,
        const void* data,
        size_t size
    );

    // Callback when reconstruction completes
    using Callback = std::function<void(
        const std::string& request_id,
        const std::string& stream_id,
        const std::vector<uint8_t>& reconstructed_data
    )>;

    void set_callback(Callback cb);

    // Check if service is available
    bool is_available() const;

private:
    Config config_;
    MarshalState& state_;
    Callback callback_;
    std::thread poll_thread_;
    std::atomic<bool> running_{true};
    std::mutex pending_mutex_;
    std::map<std::string, std::string> pending_requests_; // request_id -> stream_id

    void poll_results_loop();
    std::optional<std::vector<uint8_t>> poll_result(const std::string& request_id);
};

} // namespace mrd
```

**Key Methods:**
1. `submit()` - POST k-space data to reconstruction service
2. `set_callback()` - Register callback for completed reconstructions
3. `poll_results_loop()` - Background thread to check for results
4. `is_available()` - Health check for reconstruction service

### Step 2: Add to MarshalState (Week 1)

**File:** `src/marshal_state.hpp`

Add to `MarshalState` struct:
```cpp
struct MarshalState {
    // ... existing fields ...

    // Reconstruction service client
    std::unique_ptr<mrd::ReconstructionClient> recon_client;

    // Configuration
    std::string recon_endpoint;      // From CLI: --recon-endpoint
    int recon_timeout_sec{300};       // From CLI: --recon-timeout
    bool recon_enabled{false};        // Auto-detect if endpoint provided
};
```

### Step 3: Add CLI Arguments (Week 1)

**File:** `src/marshal_main.cpp`

Add command-line parsing:
```cpp
// In main()
std::string recon_endpoint = "";
int recon_timeout_sec = 300;

// Parse arguments
for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--recon-endpoint") {
        if (i + 1 < argc) {
            recon_endpoint = argv[++i];
        }
    }
    else if (arg == "--recon-timeout") {
        if (i + 1 < argc) {
            recon_timeout_sec = std::stoi(argv[++i]);
        }
    }
    // ... other arguments ...
}

// Initialize reconstruction client if endpoint provided
if (!recon_endpoint.empty()) {
    state.recon_endpoint = recon_endpoint;
    state.recon_timeout_sec = recon_timeout_sec;
    state.recon_enabled = true;

    mrd::ReconstructionClient::Config recon_config;
    recon_config.endpoint = recon_endpoint;
    recon_config.timeout_sec = recon_timeout_sec;

    state.recon_client = std::make_unique<mrd::ReconstructionClient>(recon_config, state);

    std::cout << "Reconstruction service enabled: " << recon_endpoint << "\n";
}
```

### Step 4: Update HTTP Handlers (Week 2)

**File:** `src/marshal_http.hpp`

Update the ACQUISITION case in both endpoints:

**For `/v1/mrd/ingest`:**
```cpp
case mrd::MrdDataType::ACQUISITION:
    // Check if reconstruction service is enabled
    if (!state.recon_enabled || !state.recon_client) {
        return make_response(http::status::not_implemented, {
            {"error", "reconstruction service not configured"},
            {"detected_type", "ACQUISITION"},
            {"message", "Raw k-space detected but no reconstruction service available."},
            {"hint", "Start marshal with --recon-endpoint http://localhost:9002"}
        });
    }

    // Check if service is available
    if (!state.recon_client->is_available()) {
        return make_response(http::status::service_unavailable, {
            {"error", "reconstruction service unavailable"},
            {"endpoint", state.recon_endpoint},
            {"message", "Reconstruction service is not responding. Check if it's running."}
        });
    }

    // Submit to reconstruction service
    try {
        auto stream_id = std::string(req["X-MRD-Stream"]);
        if (stream_id.empty()) stream_id = "default";

        std::string request_id = state.recon_client->submit(
            stream_id,
            body.data(),
            body.size()
        );

        std::cout << "[marshal_http] Submitted k-space to reconstruction service "
                  << "(request_id=" << request_id << ", stream=" << stream_id << ")\n";

        return make_response(http::status::accepted, {
            {"status", "reconstruction_queued"},
            {"request_id", request_id},
            {"stream", stream_id},
            {"message", "Raw k-space data submitted to reconstruction service. "
                       "Reconstructed images will be stored automatically when ready."}
        });
    }
    catch (const std::exception& e) {
        std::cerr << "[marshal_http] ERROR: Failed to submit to reconstruction service: "
                  << e.what() << "\n";
        return make_response(http::status::internal_server_error, {
            {"error", "reconstruction submission failed"},
            {"what", e.what()}
        });
    }
```

### Step 5: Implement Reconstruction Callback (Week 2)

**File:** `src/marshal_main.cpp`

Setup callback when reconstruction completes:
```cpp
// After creating recon_client
if (state.recon_client) {
    state.recon_client->set_callback([&state](
        const std::string& request_id,
        const std::string& stream_id,
        const std::vector<uint8_t>& reconstructed_data
    ) {
        std::cout << "[reconstruction] Received reconstructed data "
                  << "(request_id=" << request_id
                  << ", stream=" << stream_id
                  << ", size=" << reconstructed_data.size() << " bytes)\n";

        try {
            // Validate it's an ImageHeader
            if (reconstructed_data.size() < sizeof(ISMRMRD::ImageHeader)) {
                std::cerr << "[reconstruction] ERROR: Invalid reconstructed data size\n";
                return;
            }

            // Parse ImageHeader
            const auto* img_header = reinterpret_cast<const ISMRMRD::ImageHeader*>(
                reconstructed_data.data()
            );

            mrd::ImageDimensions dims;
            dims.spatial = {
                static_cast<hsize_t>(img_header->matrix_size[0]),
                static_cast<hsize_t>(img_header->matrix_size[1]),
                static_cast<hsize_t>(img_header->matrix_size[2])
            };
            dims.channels = img_header->channels ? static_cast<hsize_t>(img_header->channels) : 1;

            auto element_type = mrd::element_type_from_ismrmrd(img_header->data_type);
            const void* payload = reconstructed_data.data() + sizeof(ISMRMRD::ImageHeader);
            size_t payload_bytes = reconstructed_data.size() - sizeof(ISMRMRD::ImageHeader);

            std::string header_xml = mrd::default_ismrmrd_header(dims, element_type, stream_id);

            // Store to SWMR (same as direct image ingestion)
            auto result = state.mrd_sink->append_frame(
                stream_id,
                dims,
                element_type,
                header_xml,
                payload,
                payload_bytes,
                "" // session_token
            );

            std::cout << "[reconstruction] Stored reconstructed frame "
                      << "(stream=" << stream_id
                      << ", frame=" << result.frame_index
                      << ", path=" << result.file_path.string() << ")\n";

            // Broadcast via WebSocket
            nlohmann::json event = {
                {"type", "reconstruction_complete"},
                {"request_id", request_id},
                {"stream", stream_id},
                {"frame_index", result.frame_index},
                {"path", result.file_path.string()},
                {"timestamp", mrd::iso8601_now_ms()}
            };

            state.ws_emit(event.dump());
            state.ws_emit_topic(event.dump(), "reconstruction");
        }
        catch (const std::exception& e) {
            std::cerr << "[reconstruction] ERROR: Failed to store reconstructed data: "
                      << e.what() << "\n";

            // Broadcast error event
            nlohmann::json error_event = {
                {"type", "reconstruction_error"},
                {"request_id", request_id},
                {"stream", stream_id},
                {"error", e.what()},
                {"timestamp", mrd::iso8601_now_ms()}
            };

            state.ws_emit(error_event.dump());
        }
    });
}
```

### Step 6: Implement HTTP Client Logic (Week 3)

**File:** `src/reconstruction_client.cpp`

**Key Functions:**

1. **Submit k-space data:**
```cpp
std::string ReconstructionClient::submit(
    const std::string& stream_id,
    const void* data,
    size_t size
) {
    // Generate unique request ID
    std::string request_id = generate_uuid();

    // Prepare HTTP POST request
    // POST {endpoint}/reconstruct
    // Headers:
    //   X-Stream-ID: {stream_id}
    //   X-Request-ID: {request_id}
    //   Content-Type: application/octet-stream
    // Body: raw k-space data

    // Use Boost.Beast or libcurl
    http::request<http::string_body> req{http::verb::post, "/reconstruct", 11};
    req.set(http::field::host, config_.endpoint);
    req.set(http::field::user_agent, "MRI-Marshal/1.0");
    req.set("X-Stream-ID", stream_id);
    req.set("X-Request-ID", request_id);
    req.set(http::field::content_type, "application/octet-stream");
    req.body() = std::string(static_cast<const char*>(data), size);
    req.prepare_payload();

    // Send request
    // ... (HTTP client code) ...

    // Track pending request
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_[request_id] = stream_id;
    }

    return request_id;
}
```

2. **Poll for results:**
```cpp
void ReconstructionClient::poll_results_loop() {
    while (running_) {
        std::vector<std::string> to_check;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (const auto& [request_id, stream_id] : pending_requests_) {
                to_check.push_back(request_id);
            }
        }

        for (const auto& request_id : to_check) {
            auto result = poll_result(request_id);
            if (result) {
                // Result ready! Invoke callback
                std::string stream_id;
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    stream_id = pending_requests_[request_id];
                    pending_requests_.erase(request_id);
                }

                if (callback_) {
                    callback_(request_id, stream_id, *result);
                }
            }
        }

        // Wait before next poll
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.poll_interval_ms)
        );
    }
}

std::optional<std::vector<uint8_t>> ReconstructionClient::poll_result(
    const std::string& request_id
) {
    // GET {endpoint}/reconstruct/result/{request_id}
    // Returns:
    //   HTTP 200 + reconstructed data (if ready)
    //   HTTP 202 (still processing)
    //   HTTP 404 (not found)
    //   HTTP 500 (failed)

    // ... (HTTP GET implementation) ...

    return std::nullopt; // Not ready yet
}
```

### Step 7: Add Health Check Endpoint (Week 3)

**File:** `src/marshal_http.hpp`

Add new endpoint:
```cpp
// GET /v1/reconstruction/status
if (req.method() == http::verb::get && req.target() == "/v1/reconstruction/status")
{
    if (!state.recon_enabled) {
        return make_response(http::status::ok, {
            {"enabled", false},
            {"message", "Reconstruction service not configured"}
        });
    }

    bool available = state.recon_client->is_available();

    return make_response(http::status::ok, {
        {"enabled", true},
        {"endpoint", state.recon_endpoint},
        {"available", available},
        {"timeout_sec", state.recon_timeout_sec}
    });
}
```

### Step 8: Testing (Week 4)

**Create mock reconstruction service:**

**File:** `tests/mock_recon_service.py`

```python
#!/usr/bin/env python3
from flask import Flask, request, jsonify, send_file
import uuid
import time
import os

app = Flask(__name__)
pending = {}  # request_id -> (stream_id, data, start_time)
results = {}  # request_id -> reconstructed_data

@app.route('/reconstruct', methods=['POST'])
def submit():
    stream_id = request.headers.get('X-Stream-ID', 'default')
    request_id = request.headers.get('X-Request-ID', str(uuid.uuid4()))
    data = request.data

    print(f"[mock-recon] Received k-space: request_id={request_id}, stream={stream_id}, size={len(data)} bytes")

    # Simulate async processing
    pending[request_id] = (stream_id, data, time.time())

    return jsonify({
        'request_id': request_id,
        'status': 'queued',
        'estimated_time_sec': 5
    }), 202

@app.route('/reconstruct/status/<request_id>')
def status(request_id):
    if request_id in results:
        return jsonify({
            'request_id': request_id,
            'status': 'complete',
            'result_url': f'/reconstruct/result/{request_id}'
        })
    elif request_id in pending:
        elapsed = time.time() - pending[request_id][2]
        if elapsed > 5:  # Simulate 5 second processing
            # "Reconstruct" by creating fake ImageHeader + data
            stream_id, raw_data, _ = pending.pop(request_id)
            reconstructed = create_fake_image(stream_id, len(raw_data))
            results[request_id] = reconstructed
            return jsonify({
                'request_id': request_id,
                'status': 'complete',
                'result_url': f'/reconstruct/result/{request_id}'
            })
        return jsonify({
            'request_id': request_id,
            'status': 'processing',
            'progress': min(1.0, elapsed / 5.0)
        })
    return jsonify({'error': 'not found'}), 404

@app.route('/reconstruct/result/<request_id>')
def result(request_id):
    if request_id in results:
        # Return reconstructed ImageHeader + pixel data
        return results[request_id], 200, {'Content-Type': 'application/octet-stream'}
    return jsonify({'error': 'not found'}), 404

def create_fake_image(stream_id, original_size):
    # Create minimal ISMRMRD ImageHeader + fake pixel data
    import struct
    header = struct.pack('HQ', 1, 0)  # version=1, flags=0
    header += b'\x00' * (340 - len(header))  # Pad to 340 bytes
    pixels = b'\x00' * 1024  # Fake pixel data
    return header + pixels

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=9002)
```

**Test script:**

```bash
#!/bin/bash
# tests/test_reconstruction_flow.sh

echo "1. Start mock reconstruction service..."
python3 tests/mock_recon_service.py &
MOCK_PID=$!
sleep 2

echo "2. Start MRI Marshal with reconstruction enabled..."
./build/marshal \
    --data ./test_data \
    --recon-endpoint http://localhost:9002 \
    --recon-timeout 30 &
MARSHAL_PID=$!
sleep 2

echo "3. Send raw k-space data..."
curl -X POST http://localhost:8080/v1/mrd/ingest \
    -H "X-MRD-Stream: test_scan" \
    -H "Content-Type: application/octet-stream" \
    --data-binary @tests/data/raw_kspace.bin

echo "4. Wait for reconstruction to complete..."
sleep 10

echo "5. Check if reconstructed data was stored..."
ls -lh test_data/mrd/*.mrd

echo "6. Cleanup..."
kill $MARSHAL_PID
kill $MOCK_PID
```

---

## External Reconstruction Service Requirements

The external service (Gadgetron, custom service, etc.) must implement:

### API Contract

**1. Submit Reconstruction**
```
POST /reconstruct
Headers:
  X-Stream-ID: {stream_name}
  X-Request-ID: {unique_id}
  Content-Type: application/octet-stream
Body: Raw k-space data (ISMRMRD acquisitions or HDF5 with /dataset/data)

Response:
{
  "request_id": "uuid",
  "status": "queued",
  "estimated_time_sec": 45
}
```

**2. Check Status**
```
GET /reconstruct/status/{request_id}

Response (Processing):
{
  "request_id": "uuid",
  "status": "processing",
  "progress": 0.65
}

Response (Complete):
{
  "request_id": "uuid",
  "status": "complete",
  "result_url": "/reconstruct/result/{request_id}"
}
```

**3. Get Result**
```
GET /reconstruct/result/{request_id}

Response: Binary data
  - ISMRMRD ImageHeader (340 bytes)
  - Pixel data (float32, complex64, etc.)
```

---

## Configuration Examples

### Start with reconstruction enabled:
```bash
./build/marshal \
    --data ./data \
    --http 0.0.0.0:8080 \
    --ws 0.0.0.0:8090 \
    --recon-endpoint http://localhost:9002 \
    --recon-timeout 300
```

### Start without reconstruction (existing behavior):
```bash
./build/marshal \
    --data ./data \
    --http 0.0.0.0:8080 \
    --ws 0.0.0.0:8090
```

---

## Testing Checklist

- [ ] Reconstruction service starts successfully
- [ ] MRI Marshal connects to reconstruction service
- [ ] Health check returns service status
- [ ] Raw k-space submission returns HTTP 202
- [ ] Polling detects completed reconstruction
- [ ] Callback stores reconstructed data to SWMR
- [ ] WebSocket events broadcast reconstruction status
- [ ] Existing reconstructed image flow still works
- [ ] Service unavailable fallback works
- [ ] Timeout handling works (300 sec default)

---

## Current Code Locations

### Detection (Already Implemented)
- `include/mrd_type_detector.hpp` - Type detection
- `src/marshal_http.hpp:345-460` - `/v1/mrd/frame` with detection
- `src/marshal_http.hpp:461-521` - `/v1/mrd/ingest` with detection

### Placeholders (Need Implementation)
- Line 480-489: Raw k-space case in `/v1/mrd/ingest` → **TODO: Call recon_client->submit()**
- Line 365-374: Raw k-space case in `/v1/mrd/frame` → **TODO: Call recon_client->submit()**

---

## Documentation References

- **Full Design:** [MRI_MARSHAL_RECONSTRUCTION_ROUTING.md](MRI_MARSHAL_RECONSTRUCTION_ROUTING.md)
- **Quick Overview:** [MRI_MARSHAL_QUICK_OVERVIEW.md](MRI_MARSHAL_QUICK_OVERVIEW.md)
- **Implementation Summary:** [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)
- **ISMRMRD Docs:** https://ismrmrd.readthedocs.io/

---

## Success Criteria

When done, this workflow should work:

```
1. Scanner sends raw k-space to /v1/mrd/ingest
   → Marshal detects: "ACQUISITION"
   → Marshal submits to reconstruction service
   → Returns HTTP 202 with request_id

2. Reconstruction service processes (30-90 seconds)
   → Marshal polls every 1 second
   → Detects completion

3. Marshal retrieves reconstructed images
   → Callback stores to SWMR
   → Broadcasts WebSocket event

4. Visualization clients see new frames
   → Read from SWMR file
   → Display images normally
```

**End result:** Scanner can send raw or reconstructed data - marshal handles both automatically!

---

## Git Branches

- **Main repo:** `feature/reconstruction-routing`
- **Worktree:** `feature/bio-memory-cache`

## Current Status

- ✅ Phase 1: Detection complete
- ⏳ Phase 2: Integration (THIS HANDOFF)
- ⏳ Phase 3: Testing & optimization

---

**Good luck! The framework is ready - just need to implement the HTTP client and wire up the callbacks.**
