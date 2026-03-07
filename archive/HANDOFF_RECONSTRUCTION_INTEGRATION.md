# Handoff: Reconstruction Service Integration (Simple HTTP Forwarding)

**For:** Next AI Agent
**Status:** Phase 1 Complete (Detection) - Ready for Phase 2 (Simple Forwarding)
**Priority:** Medium
**Estimated Effort:** 1-2 days (much simpler than originally planned)

---

## What's Already Done ✅

### Phase 1: Smart Detection (COMPLETE)

Both `/v1/mrd/frame` and `/v1/mrd/ingest` endpoints now automatically detect:
- ✅ Raw k-space data (AcquisitionHeader)
- ✅ Reconstructed images (ImageHeader)
- ✅ Complete HDF5 files

**Current behavior:**
- Raw k-space → Returns HTTP 501 "Not yet implemented" with TODO message
- Reconstructed → Stores normally (works perfectly)
- HDF5 file → Stores as-is (works perfectly)

---

## What Needs to Be Done 🔨

### Phase 2: Simple HTTP Forwarding to External Reconstruction Service

**Key Point:** The reconstruction client/service is EXTERNAL and already exists (e.g., Gadgetron, custom service).

The marshal just needs to:
1. Accept raw k-space data
2. Forward it to the external service via HTTP POST
3. Wait for reconstructed response
4. Store the reconstructed images

**No complex client implementation needed!** Just simple HTTP forwarding.

---

## Simple Implementation (2 Steps)

### Step 1: Add CLI Argument for Reconstruction Endpoint (30 minutes)

**File:** `src/marshal_main.cpp`

Add command-line parsing:
```cpp
// In main() function, add to argument parsing:
std::string recon_endpoint = "";

for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--recon-endpoint") {
        if (i + 1 < argc) {
            recon_endpoint = argv[++i];
        }
    }
    // ... other arguments ...
}

// Store in state
state.recon_endpoint = recon_endpoint;
state.recon_enabled = !recon_endpoint.empty();

if (state.recon_enabled) {
    std::cout << "[marshal] Reconstruction service enabled: " << recon_endpoint << "\n";
}
```

**Add to MarshalState:**

**File:** `src/marshal_state.hpp`

```cpp
struct MarshalState {
    // ... existing fields ...

    // Reconstruction service configuration
    std::string recon_endpoint;    // e.g., "http://localhost:9002"
    bool recon_enabled{false};     // True if --recon-endpoint provided
};
```

### Step 2: Update HTTP Handlers to Forward Raw K-Space (2 hours)

**File:** `src/marshal_http.hpp`

Replace the ACQUISITION case in `/v1/mrd/ingest`:

```cpp
case mrd::MrdDataType::ACQUISITION:
    // RAW K-SPACE DATA - Forward to external reconstruction service
    {
        if (!state.recon_enabled) {
            return make_response(http::status::not_implemented, {
                {"error", "reconstruction service not configured"},
                {"detected_type", "ACQUISITION"},
                {"message", "Raw k-space detected but no reconstruction service available."},
                {"hint", "Start marshal with --recon-endpoint http://localhost:9002"}
            });
        }

        std::cout << "[marshal_http] Forwarding raw k-space to reconstruction service: "
                  << state.recon_endpoint << "\n";

        try {
            // Simple HTTP POST to external service
            // POST {recon_endpoint}/reconstruct
            // Body: raw k-space data (as-is)

            namespace beast = boost::beast;
            namespace http = beast::http;
            namespace net = boost::asio;
            using tcp = net::ip::tcp;

            // Parse endpoint URL
            std::string host = state.recon_endpoint;  // Simplified: assume "host:port"
            std::string port = "9002";  // Default port

            // Better: parse URL properly
            // For now, assume format: http://host:port
            size_t protocol_pos = host.find("://");
            if (protocol_pos != std::string::npos) {
                host = host.substr(protocol_pos + 3);
            }
            size_t port_pos = host.find(":");
            if (port_pos != std::string::npos) {
                port = host.substr(port_pos + 1);
                host = host.substr(0, port_pos);
            }

            // Connect to reconstruction service
            net::io_context ioc;
            tcp::resolver resolver(ioc);
            beast::tcp_stream stream(ioc);

            auto const results = resolver.resolve(host, port);
            stream.connect(results);

            // Prepare HTTP POST request
            http::request<http::string_body> req{http::verb::post, "/reconstruct", 11};
            req.set(http::field::host, host);
            req.set(http::field::user_agent, "MRI-Marshal/1.0");
            req.set(http::field::content_type, "application/octet-stream");
            req.body() = body;  // Forward raw k-space data as-is
            req.prepare_payload();

            // Send request
            http::write(stream, req);

            // Read response from reconstruction service
            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(stream, buffer, res);

            // Close connection
            beast::error_code ec;
            stream.socket().shutdown(tcp::socket::shutdown_both, ec);

            std::cout << "[marshal_http] Reconstruction service responded: HTTP "
                      << res.result_int() << "\n";

            // Check if reconstruction succeeded
            if (res.result() != http::status::ok && res.result() != http::status::created) {
                return make_response(http::status::bad_gateway, {
                    {"error", "reconstruction service failed"},
                    {"service_status", res.result_int()},
                    {"service_response", res.body()}
                });
            }

            // Reconstructed data is in res.body()
            // It should be ImageHeader + pixel data
            const std::string& reconstructed = res.body();

            if (reconstructed.size() < sizeof(ISMRMRD::ImageHeader)) {
                return make_response(http::status::bad_gateway, {
                    {"error", "invalid response from reconstruction service"},
                    {"message", "Response too small to contain ImageHeader"}
                });
            }

            // Parse reconstructed ImageHeader
            const auto* img_header = reinterpret_cast<const ISMRMRD::ImageHeader*>(
                reconstructed.data()
            );

            mrd::ImageDimensions dims;
            dims.spatial = {
                static_cast<hsize_t>(img_header->matrix_size[0]),
                static_cast<hsize_t>(img_header->matrix_size[1]),
                static_cast<hsize_t>(img_header->matrix_size[2])
            };
            dims.channels = img_header->channels ? static_cast<hsize_t>(img_header->channels) : 1;

            auto element_type = mrd::element_type_from_ismrmrd(img_header->data_type);
            const void* payload = reconstructed.data() + sizeof(ISMRMRD::ImageHeader);
            size_t payload_bytes = reconstructed.size() - sizeof(ISMRMRD::ImageHeader);

            auto stream_id = std::string(req["X-MRD-Stream"]);
            if (stream_id.empty()) stream_id = "reconstructed";

            std::string header_xml = mrd::default_ismrmrd_header(dims, element_type, stream_id);

            // Store reconstructed image to SWMR (same as normal image path)
            auto result = state.mrd_sink->append_frame(
                stream_id,
                dims,
                element_type,
                header_xml,
                payload,
                payload_bytes,
                "" // session_token
            );

            std::cout << "[marshal_http] Stored reconstructed image: "
                      << "stream=" << stream_id
                      << ", frame=" << result.frame_index
                      << ", path=" << result.file_path.string() << "\n";

            // Return success response
            return make_response(http::status::created, {
                {"status", "reconstructed_and_stored"},
                {"path", result.file_path.string()},
                {"frame_index", result.frame_index},
                {"stream", stream_id},
                {"message", "Raw k-space was reconstructed and stored successfully"}
            });
        }
        catch (const std::exception& e) {
            std::cerr << "[marshal_http] ERROR: Reconstruction forwarding failed: "
                      << e.what() << "\n";
            return make_response(http::status::bad_gateway, {
                {"error", "reconstruction forwarding failed"},
                {"what", e.what()},
                {"endpoint", state.recon_endpoint}
            });
        }
    }
```

---

## That's It!

The external reconstruction service is responsible for:
- Receiving raw k-space data (HTTP POST)
- Performing reconstruction
- Returning reconstructed ImageHeader + pixel data (HTTP response body)

The marshal just forwards and stores the result.

---

## External Reconstruction Service API (What It Must Provide)

### Endpoint: POST /reconstruct

**Request:**
```
POST /reconstruct HTTP/1.1
Content-Type: application/octet-stream
Body: [raw k-space data - ISMRMRD acquisitions or HDF5]
```

**Response:**
```
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Body: [ISMRMRD ImageHeader (198 bytes) + pixel data]
```

**That's all the external service needs to implement!**

---

## Usage Example

### Start Marshal with Reconstruction Service

```bash
# Reconstruction service running on localhost:9002
./build/marshal \
    --data ./data \
    --http 0.0.0.0:8080 \
    --ws 0.0.0.0:8090 \
    --recon-endpoint http://localhost:9002
```

### Send Raw K-Space Data

```bash
# Marshal will automatically detect it's raw k-space,
# forward to http://localhost:9002/reconstruct,
# receive reconstructed image,
# and store to SWMR
curl -X POST http://localhost:8080/v1/mrd/ingest \
    -H "X-MRD-Stream: cardiac_scan" \
    -H "Content-Type: application/octet-stream" \
    --data-binary @raw_kspace.bin

# Response:
{
  "status": "reconstructed_and_stored",
  "path": "/data/mrd/cardiac_scan.mrd",
  "frame_index": 0,
  "stream": "cardiac_scan"
}
```

---

## Testing with Mock Service

**Simple Python Mock (tests/mock_recon_service.py):**

```python
#!/usr/bin/env python3
from flask import Flask, request, Response
import struct

app = Flask(__name__)

@app.route('/reconstruct', methods=['POST'])
def reconstruct():
    # Receive raw k-space
    raw_kspace = request.data
    print(f"[mock-recon] Received {len(raw_kspace)} bytes of k-space")

    # "Reconstruct" by creating fake ImageHeader + pixels
    # In reality, this would be Gadgetron doing real reconstruction

    # Create minimal ISMRMRD ImageHeader
    header = bytearray(198)
    struct.pack_into('H', header, 0, 1)  # version = 1
    struct.pack_into('H', header, 48, 64)  # matrix_size[0] = 64
    struct.pack_into('H', header, 50, 64)  # matrix_size[1] = 64
    struct.pack_into('H', header, 52, 1)   # matrix_size[2] = 1
    struct.pack_into('H', header, 54, 1)   # channels = 1
    struct.pack_into('H', header, 56, 5)   # data_type = 5 (FLOAT)

    # Create fake pixel data (64x64x1x1 = 4096 floats = 16384 bytes)
    pixels = b'\x00' * (64 * 64 * 4)  # All zeros (black image)

    reconstructed = bytes(header) + pixels

    print(f"[mock-recon] Returning {len(reconstructed)} bytes reconstructed data")

    return Response(reconstructed, mimetype='application/octet-stream')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=9002)
```

**Run test:**
```bash
# Terminal 1: Start mock reconstruction service
python3 tests/mock_recon_service.py

# Terminal 2: Start marshal with reconstruction enabled
./build/marshal --data ./data --recon-endpoint http://localhost:9002

# Terminal 3: Send raw k-space
curl -X POST http://localhost:8080/v1/mrd/ingest \
    -H "X-MRD-Stream: test" \
    --data-binary @tests/data/sample_kspace.bin
```

---

## Configuration

### Enable Reconstruction
```bash
./build/marshal \
    --data ./data \
    --recon-endpoint http://reconstruction-server:9002
```

### Disable Reconstruction (Default)
```bash
./build/marshal --data ./data
# Raw k-space will return HTTP 501 "Not configured"
```

---

## Error Handling

| Scenario | HTTP Status | Message |
|----------|-------------|---------|
| No `--recon-endpoint` | 501 Not Implemented | "Start marshal with --recon-endpoint" |
| Service unreachable | 502 Bad Gateway | "Could not connect to service" |
| Service returns error | 502 Bad Gateway | "Reconstruction service failed" |
| Invalid response | 502 Bad Gateway | "Invalid response from service" |
| Success | 201 Created | "Reconstructed and stored" |

---

## Success Criteria

When done, this workflow should work:

```
1. Start external reconstruction service (Gadgetron, custom, etc.)
   → Listening on port 9002
   → Implements POST /reconstruct endpoint

2. Start marshal with --recon-endpoint
   → Marshal knows where to forward raw k-space

3. Scanner sends raw k-space to marshal
   → Marshal detects: "ACQUISITION"
   → Marshal forwards to reconstruction service
   → Service reconstructs and returns ImageHeader + pixels
   → Marshal stores to SWMR
   → Returns HTTP 201 "Reconstructed and stored"

4. Visualization clients see new frames
   → Read from SWMR file normally
   → Everything works as if images were sent directly
```

---

## What the Implementation Does NOT Need

- ❌ **NO** complex ReconstructionClient class
- ❌ **NO** background polling threads
- ❌ **NO** async queuing or buffering
- ❌ **NO** k-space accumulation logic
- ❌ **NO** callback mechanisms

**Just simple synchronous HTTP POST and response!**

---

## Summary

**Phase 1 (Complete):** ✅ Detection and routing framework
**Phase 2 (This handoff):** Simple HTTP forwarding to external service

**Implementation time:** 1-2 days (not 3-4 weeks!)

**Key insight:** The reconstruction service is external and already handles all complexity. The marshal just needs to forward data and store the result.

---

## Files to Modify

1. ✅ `src/marshal_state.hpp` - Add `recon_endpoint` field (5 lines)
2. ✅ `src/marshal_main.cpp` - Add CLI parsing (10 lines)
3. ✅ `src/marshal_http.hpp` - Add forwarding logic (80 lines)

**Total: ~100 lines of code!**

---

## Current Status

- ✅ Phase 1: Detection complete
- ⏳ Phase 2: Simple forwarding (THIS HANDOFF - 1-2 days)
- ⏳ Phase 3: Testing with real Gadgetron

**Much simpler than originally thought - the external service does all the heavy lifting!**
