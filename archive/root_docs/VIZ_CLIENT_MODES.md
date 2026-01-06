# viz_client - Mode Selection Guide

The enhanced viz_client now supports **two separate modes** for frame updates:

## Mode 1: HTTP Polling (Default)

**Command:**
```bash
./build/viz_client --http http://127.0.0.1:8080/v1/mrd/latest \
                   --data ./data_demo_mri/mrd
```

**Or with defaults:**
```bash
./build/viz_client
```

**How it works:**
- Polls HTTP endpoint every 500ms
- Fetches JSON with latest frame metadata
- Reads HDF5 file directly via SWMR
- Simple, reliable, firewall-friendly

**Best for:**
- Demonstrations
- Unreliable networks
- Debugging
- Production with HTTP infrastructure

---

## Mode 2: WebSocket (Optional)

**Command:**
```bash
./build/viz_client --ws ws://127.0.0.1:8090/ws \
                   --data ./data_demo_mri/mrd
```

**How it works:**
- Connects to WebSocket server
- Receives frame notifications in real-time
- Lower latency (no polling delay)
- Requires persistent connection

**Best for:**
- Production with WebSocket infrastructure
- Ultra-low latency needed
- High-frequency updates

---

## Command-Line Options

```bash
./build/viz_client [OPTIONS]

Options:
  --http URL          Use HTTP polling mode with endpoint URL
                      (default: http://localhost:8080/v1/mrd/latest)

  --ws URL            Use WebSocket mode with endpoint URL
                      (default: ws://localhost:8090/ws)

  --data PATH         Path to HDF5 data directory
                      (default: ./data/mrd)
```

---

## Examples

### Default (HTTP polling)
```bash
./build/viz_client
# Uses: HTTP polling from http://localhost:8080/v1/mrd/latest
# Reads: ./data/mrd
```

### Custom HTTP endpoint
```bash
./build/viz_client --http http://192.168.1.100:8080/v1/mrd/latest \
                   --data /mnt/storage/mri_data
```

### WebSocket mode
```bash
./build/viz_client --ws ws://localhost:8090/ws \
                   --data ./data_demo_mri/mrd
```

### Custom WebSocket endpoint
```bash
./build/viz_client --ws ws://remote-server:9090/ws \
                   --data ./data_demo_mri/mrd
```

---

## Mode Behavior Comparison

| Feature | HTTP | WebSocket |
|---------|------|-----------|
| **Polling Interval** | 500ms | Real-time |
| **Connection** | Stateless | Persistent |
| **Latency** | ~500-1000ms | <100ms |
| **Network** | Simple HTTP | Bidirectional |
| **Firewall** | Friendly | Complex |
| **Reliability** | High | Medium |
| **CPU** | Low | Low |

---

## What Happens in Each Mode

### HTTP Polling Mode
```
1. Every 500ms: GET http://localhost:8080/v1/mrd/latest
2. Response: {"frame_index": 4, "path": "./data/mrd/scan_001.h5", ...}
3. Process: Enqueue task to read frame 4
4. Read:  Direct HDF5 SWMR read from scan_001.h5
5. Render: OpenCV displays frame
6. Repeat: Step 1
```

### WebSocket Mode
```
1. Connect: ws://localhost:8090/ws
2. Receive: {"frame_index": 4, "path": "./data/mrd/scan_001.h5", ...}
3. Process: Enqueue task to read frame 4
4. Read: Direct HDF5 SWMR read from scan_001.h5
5. Render: OpenCV displays frame
6. Receive: Next frame notification (repeat)
```

---

## For Tomorrow's Presentation

**Recommendation: Use HTTP mode (default)**

```bash
# In automated demo:
./build/viz_client --http http://127.0.0.1:8080/v1/mrd/latest \
                   --data ./data_demo_mri/mrd
```

**Why:**
- More reliable (no connection state)
- Simpler to explain
- Works even if network is flaky
- Demonstrates REST API usage

---

## Troubleshooting

### Window appears but no frames
- **HTTP mode:** Check endpoint responds: `curl http://localhost:8080/v1/mrd/latest`
- **WebSocket mode:** Check connection: `wscat -c ws://localhost:8090/ws`

### Slow updates
- **HTTP mode:** Normal (500ms polling interval)
- **WebSocket mode:** Check WebSocket connection stability

### Connection refused
- **HTTP mode:** Verify MRI Marshal running on port 8080
- **WebSocket mode:** Verify WebSocket on port 8090

### HDF5 file not found
- Both modes: Check `--data` path exists and has HDF5 files

---

## Implementation Details

### HTTP Polling Implementation
- Uses libcurl for HTTP requests
- Timeout: 5 seconds, connect timeout: 2 seconds
- Gracefully handles network errors
- Lightweight (single GET per polling interval)

### Direct HDF5 SWMR Reading
- Both modes use same HDF5 reading mechanism
- SWMR (Single-Writer-Multiple-Reader) mode
- Lock-free concurrent access
- Latency: 1-5ms for SWMR read

---

## You Can Switch Modes Without Rebuilding!

The binary supports both modes with runtime selection via command-line:

```bash
# Start in HTTP mode
./build/viz_client --http ...

# Switch to WebSocket for next run
./build/viz_client --ws ...
```

**No recompilation needed!**

---

## Default Configuration

If no arguments provided, uses:
```
Mode:   HTTP polling
HTTP:   http://localhost:8080/v1/mrd/latest
Data:   ./data/mrd
Polling: Every 500ms
```

This is optimized for typical demo/development scenarios.

---

Good luck with your presentation! Use HTTP mode for maximum reliability. 🎉
