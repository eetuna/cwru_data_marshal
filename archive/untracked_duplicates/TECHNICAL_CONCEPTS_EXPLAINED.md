# Technical Concepts Explained - Deep Dive

This document explains every technical term and concept mentioned in the interview prep, so you truly understand what you're talking about.

---

## Table of Contents

1. [Communication Protocols](#1-communication-protocols)
2. [Distributed Systems Concepts](#2-distributed-systems-concepts)
3. [Concurrency & Performance](#3-concurrency--performance)
4. [Data Storage & Formats](#4-data-storage--formats)
5. [Software Architecture Patterns](#5-software-architecture-patterns)
6. [Container & Orchestration](#6-container--orchestration)
7. [Real-Time Systems](#7-real-time-systems)

---

## 1. Communication Protocols

### Server-Sent Events (SSE)

**What it is:**
A standard allowing servers to push data to web browsers over HTTP. The connection stays open, and the server can send updates whenever it wants.

**How it works:**
```
Client                          Server
  |                               |
  |--- HTTP GET /events -------->|
  |                               |
  |<-- data: event1 -------------|
  |<-- data: event2 -------------|
  |<-- data: event3 -------------|
  |     (connection stays open)  |
```

**Real-world analogy:**
Like a news ticker - you subscribe once, and updates keep coming without you asking again.

**In your project:**
You technically use **WebSocket** (similar but bidirectional), not SSE. Both solve the same problem: push notifications instead of polling.

**Why it matters for robotics:**
Instead of robots constantly asking "any new commands?" (polling), the control system pushes commands immediately. Lower latency, less network overhead.

---

### WebSocket

**What it is:**
A protocol that establishes a persistent, bidirectional connection between client and server. Either side can send messages at any time.

**How it works:**
```
Client                          Server
  |                               |
  |--- HTTP Upgrade Req -------->|
  |<-- Switching Protocols ------|
  |                               |
  |<=== Full-Duplex Channel ====>|
  |                               |
  |--- client message ---------->|
  |<-- server push --------------|
  |--- another message --------->|
```

**Difference from HTTP:**
- **HTTP:** Request/response pairs (client always initiates)
- **WebSocket:** Persistent connection (either side can send anytime)

**In your project:**
```cpp
// Client subscribes to "mrd" topic
ws://localhost:8090/ws
Send: {"subscribe": "mrd"}

// Server pushes notifications
Receive: {"type": "mrd", "frame_index": 42, "timestamp": "..."}
```

**Implementation:**
- Library: Boost.Beast
- File: `.worktrees/mri_data_marshal/src/marshal_ws.hpp`
- Port: 8090

**Why it matters for robotics:**
Real-time teleoperation requires instant bidirectional communication. Robot sends sensor data up, operator sends commands down, all over one persistent connection.

---

### HTTP/REST API

**What it is:**
Representational State Transfer - a design pattern for network APIs using standard HTTP methods.

**HTTP Methods:**
- **GET:** Retrieve data (read-only)
- **POST:** Submit data (create/update)
- **PUT:** Replace data (full update)
- **DELETE:** Remove data
- **PATCH:** Partial update

**In your project:**
```bash
# GET - retrieve latest frame metadata
GET http://localhost:8080/v1/mrd/latest
Response: {"data": {"path": "...", "frame_index": 42}}

# POST - submit new MRI frame
POST http://localhost:8080/v1/mrd/frame
Body: <binary MRI data>
Response: {"frame_index": 43, "timestamp": "..."}
```

**REST principles:**
1. **Stateless:** Each request contains all needed info (no server-side session)
2. **Resource-based:** URLs represent resources (/v1/mrd/frame)
3. **Standard methods:** Use HTTP verbs semantically
4. **JSON responses:** Structured data exchange

**Why it matters for robotics:**
Standard protocol means any language can communicate with your robot system. Python planner, C++ controller, JavaScript UI - all speak HTTP/REST.

---

### Headers (HTTP)

**What they are:**
Metadata sent with HTTP requests/responses. Not part of the body, but provide context.

**Common headers:**
```http
Content-Type: application/json           # What format is the body?
Content-Length: 1024                     # How big is the body?
Authorization: Bearer <token>            # Who is making the request?
X-Custom-Header: custom_value            # Custom metadata
```

**In your project:**
```http
POST /v1/mrd/frame HTTP/1.1
Host: localhost:8080
Content-Type: application/octet-stream
X-MRD-Stream: raw_scan                   # Custom: which stream?
X-MRD-Callback: http://marshal:8080      # Custom: callback URL
Content-Length: 10240

<binary data here>
```

**Why they matter:**
Headers let you send metadata without parsing the body. For binary data (like MRI frames), headers tell you what the bytes mean.

---

## 2. Distributed Systems Concepts

### Microservices Architecture

**What it is:**
Breaking a system into small, independent services that communicate over the network.

**Contrast with Monolith:**

**Monolithic:**
```
┌─────────────────────────┐
│  One Big Application    │
│  ┌─────────────────┐    │
│  │ Data Ingestion  │    │
│  │ Processing      │    │
│  │ Storage         │    │
│  │ API             │    │
│  └─────────────────┘    │
└─────────────────────────┘
All in one process
```

**Microservices:**
```
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│  Ingestion   │───▶│  Processing  │───▶│   Storage    │
│  Service     │    │  Service     │    │   Service    │
└──────────────┘    └──────────────┘    └──────────────┘
     │                                          │
     └──────────────▶ Message Queue ◀──────────┘
```

**In your project:**
- **mri-marshal:** Data coordination (HTTP/WebSocket server)
- **robot-marshal:** Robot state exchange
- **mock-recon:** Image reconstruction
- **kspace-streamer:** Data ingestion
- **viz-client:** Visualization

Each runs in its own Docker container, has its own process, communicates via HTTP.

**Advantages:**
- **Independent scaling:** Can run 10 viz-clients, 1 marshal
- **Independent deployment:** Update reconstruction without touching marshal
- **Technology diversity:** Marshal in C++, recon in Python, viz in whatever
- **Fault isolation:** If recon crashes, marshal keeps running

**Challenges:**
- Network latency between services
- Service discovery (how does streamer find marshal?)
- Distributed debugging (logs across services)

**Why it matters for robotics:**
Amazon's robot fleet has perception, planning, control, safety - all separate services. If one crashes, others keep running. Can deploy updates to planning without touching control.

---

### Service Discovery

**What it is:**
How services find each other's network addresses without hardcoding IPs.

**The Problem:**
```
# Hard-coded IP (bad)
recon_url = "http://192.168.1.45:8080"  # What if IP changes?
```

**The Solution (DNS-based):**
```
# Docker Compose provides DNS
recon_url = "http://mri-marshal:8080"   # Name resolves to current IP
```

**How Docker implements it:**
```yaml
services:
  mri-marshal:
    networks:
      - cwru-net
  kspace-streamer:
    environment:
      - MARSHAL_ENDPOINT=http://mri-marshal:8080  # DNS name
    networks:
      - cwru-net
```

Docker's internal DNS server maps `mri-marshal` → current container IP.

**In your project:**
Services reference each other by container name, Docker resolves to IP automatically.

**Why it matters for robotics:**
Robot fleet with 100 robots - you don't want to configure IPs manually. Service registry (like Kubernetes DNS, AWS Service Discovery) lets robots find the central planner, planner finds the robots, all dynamic.

---

### Health Checks

**What they are:**
Automated tests to verify a service is alive and ready to accept requests.

**In your project:**
```yaml
healthcheck:
  test: ["CMD", "curl", "-f", "http://localhost:8080/health"]
  interval: 5s        # Check every 5 seconds
  timeout: 3s         # Fail if takes >3 seconds
  start_period: 10s   # Wait 10s before first check
  retries: 3          # Fail after 3 consecutive failures
```

**What happens:**
1. Container starts
2. Wait 10 seconds (start_period)
3. Every 5 seconds, run `curl http://localhost:8080/health`
4. If 3 consecutive failures, mark container as unhealthy
5. Docker Compose can restart unhealthy containers

**Health endpoint implementation:**
```cpp
// In marshal_http.hpp
if (req.target() == "/health") {
    response.result(http::status::ok);
    response.body() = R"({"status":"ok"})";
    return response;
}
```

**Why it matters for robotics:**
Robots can't tolerate silent failures. Health checks detect when a service is stuck/crashed and trigger auto-restart or failover. Amazon's fleet needs this at scale - a hung planner could stall dozens of robots.

---

### Dependency Management

**What it is:**
Ensuring services start in the correct order (e.g., database before API server).

**In your project:**
```yaml
kspace-streamer:
  depends_on:
    mri-marshal:
      condition: service_healthy  # Wait for marshal to be healthy
```

**What this does:**
1. Start `mri-marshal`
2. Wait for health check to pass
3. Only then start `kspace-streamer`

**Why it's needed:**
```
# Without depends_on
kspace-streamer starts → tries to POST → marshal not ready → FAIL

# With depends_on
kspace-streamer starts → marshal already healthy → POST succeeds
```

**Why it matters for robotics:**
Robot control loop depends on sensor drivers. Sensor drivers depend on hardware init. Hardware init depends on config service. Dependency management ensures correct startup order.

---

### Asynchronous Processing

**What it is:**
Returning a response immediately, then doing work in the background.

**Synchronous (blocking):**
```
Client                          Server
  |                               |
  |--- POST request ------------>|
  |                               |---- Do work (2 seconds)
  |                               |
  |<-- Response (200 OK) --------|
  |    (2 second wait)            |
```

**Asynchronous (non-blocking):**
```
Client                          Server                   Worker Thread
  |                               |                            |
  |--- POST request ------------>|                            |
  |                               |-- Queue work ------------->|
  |<-- 202 Accepted -------------|                            |
  |    (instant)                  |                            |
  |                               |                            |---- Do work
  |                               |                            |
  |                               |<-- Callback when done -----|
```

**In your project:**
```python
# Mock recon service
@app.route('/reconstruct', methods=['POST'])
def reconstruct():
    raw_kspace = request.data
    callback_url = request.headers.get('X-MRD-Callback')

    # Start background thread
    thread = Thread(target=process_and_callback, args=(raw_kspace, callback_url))
    thread.start()

    # Return immediately
    return jsonify({"status": "processing"}), 202  # 202 = Accepted
```

**Callback pattern:**
```
1. kspace-streamer → POST k-space → mri-marshal
2. mri-marshal → POST k-space + callback URL → mock-recon
3. mock-recon → 202 Accepted (instant)
4. mock-recon → [background: reconstruct 3D volume]
5. mock-recon → POST result → callback URL (mri-marshal)
6. mri-marshal → store result + notify clients
```

**Why it matters for robotics:**
Motion planning takes time (100ms-1s). You can't block the control loop waiting. Instead: submit planning request, get "processing" response, continue control loop with current plan, switch to new plan when callback arrives.

---

## 3. Concurrency & Performance

### SWMR (Single-Write-Multiple-Read)

**What it is:**
A file access pattern where one process writes, many processes read simultaneously.

**The Problem (without SWMR):**
```
Writer                    Reader 1              Reader 2
  |                          |                     |
  |-- Open file (write) --->|                     |
  |                          X-- Open file (FAILS - locked)
  |                          |                     X-- Open file (FAILS)
```

Traditional file locking: only one process can access at a time.

**The Solution (SWMR):**
```
Writer                    Reader 1              Reader 2
  |                          |                     |
  |-- Open SWMR write ----->|                     |
  |                          |-- Open SWMR read ->|
  |                          |                     |-- Open SWMR read
  |-- Write frame 42 ------>|                     |
  |                          |-- Read frame 42 -->|
  |                          |                     |-- Read frame 42
```

All three processes access the file simultaneously. Readers see data the instant writer flushes.

**HDF5 SWMR Mode:**
```cpp
// Writer (mri-marshal)
H5::H5File file("demo.mrd", H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE);
dataset.write(new_frame, ...);
file.flush(H5F_SCOPE_GLOBAL);  // Make visible to readers

// Reader (viz-client)
H5::H5File file("demo.mrd", H5F_ACC_RDONLY | H5F_ACC_SWMR_READ);
dataset.refresh();  // See latest data
dataset.read(buffer, frame_index);
```

**Key Rules:**
1. Only ONE writer allowed
2. Unlimited readers allowed
3. Writer must `flush()` for readers to see new data
4. Readers must `refresh()` to see new data

**In your project:**
- **Writer:** mri-marshal (stores MRI frames to demo.mrd)
- **Readers:** viz-client, offline analysis tools, monitoring dashboards
- **Benefit:** 10 viz clients can read simultaneously while marshal writes

**Why it matters for robotics:**
Sensor data stream (LiDAR, cameras) → one process writes to shared memory → perception, planning, control, logging all read simultaneously. No copying, no pub-sub overhead, just direct memory/file access.

---

### Async I/O (Asynchronous Input/Output)

**What it is:**
Starting I/O operations (disk, network) without blocking the thread.

**Synchronous I/O (blocking):**
```cpp
// Thread blocks here until write completes (5ms)
file.write(data);
// Can't do anything else during write
return response;  // Latency: 5ms
```

**Asynchronous I/O (non-blocking):**
```cpp
// Queue write, continue immediately
async_writer_queue.push(data);
return response;  // Latency: 0.1ms

// Background thread handles writes
void writer_thread() {
    while (true) {
        data = async_writer_queue.pop();
        file.write(data);  // Slow, but doesn't block main thread
    }
}
```

**In your project:**
```cpp
// marshal_state.hpp
std::thread json_writer_thread;  // Background thread
std::queue<JsonEvent> json_queue;

// HTTP handler (fast path)
void handle_mrd_frame(data) {
    sink.write_to_hdf5(data);         // Required for correctness
    update_cache(data);                // Fast (in-memory)
    json_queue.push(metadata);         // Fast (queue)
    notify_websockets();               // Fast (network already async)
    return 200;                        // Total: ~1ms
}

// Background thread (slow path)
void json_writer_loop() {
    while (true) {
        event = json_queue.pop();
        file.write(json.dumps(event));  // Slow (~5ms), but doesn't block HTTP
    }
}
```

**Why it matters:**
HTTP response time: 1ms (with async I/O) vs 6ms (without). At 50 FPS, that's the difference between 20% and 120% CPU utilization.

**Why it matters for robotics:**
Control loop runs at 100Hz (10ms period). If disk logging blocks for 5ms, you've used half your time budget. Async I/O lets you log everything without impacting control loop timing.

---

### Threading Models

**What they are:**
How a program uses multiple CPU cores to do work in parallel.

**Single-threaded:**
```
Thread 1: HTTP request 1 → HTTP request 2 → HTTP request 3
One request at a time
```

**Multi-threaded (thread per request):**
```
Thread 1: HTTP request 1
Thread 2: HTTP request 2
Thread 3: HTTP request 3
All requests handled simultaneously
```

**Thread Pool:**
```
Pool: [Thread 1] [Thread 2] [Thread 3] [Thread 4]
Queue: [Request A] [Request B] [Request C] [Request D] [Request E]

Thread 1 takes A
Thread 2 takes B
Thread 3 takes C
Thread 4 takes D
Thread 1 finishes A, takes E
```

**In your project:**

**MRI Marshal:**
- **Main thread:** HTTP listener (Boost.Asio event loop)
- **WS thread:** WebSocket listener (separate event loop)
- **JSON writer thread:** Async JSONL disk I/O
- **Request threads:** Thread pool for handling HTTP/WS sessions

**Robot Marshal:**
- **Main thread:** HTTP server (cpp-httplib built-in thread pool)
- **Writer thread:** Background disk flush
- **Request threads:** 8 threads (configurable) handle concurrent requests

**Why it matters for robotics:**
Perception, planning, control all run in parallel threads. Perception computes at 30Hz, planning at 10Hz, control at 100Hz - all on different cores, all accessing shared state via SWMR or lock-free data structures.

---

### Latency vs Throughput

**Latency:**
How long ONE operation takes (milliseconds).

**Throughput:**
How many operations per second (ops/sec).

**Example:**
```
Scenario A: Latency 1ms, Throughput 1000 ops/sec
Scenario B: Latency 10ms, Throughput 1000 ops/sec (via parallelism)
```

Both handle 1000 requests/sec, but scenario A feels 10x faster to the user.

**In your project:**
```
Operation: POST /v1/mrd/frame (64KB)
Latency: ~1ms (time for ONE request)
Throughput: 20-50 FPS (frames per second)
```

Low latency is achieved through:
- Async I/O (don't wait for disk)
- In-memory caching (serve reads from RAM)
- Zero-copy (avoid memcpy)

High throughput is achieved through:
- Thread pool (handle 10 requests simultaneously)
- SWMR (10 readers don't block writer)

**Why it matters for robotics:**
- **Latency:** How fast robot reacts to human input (teleoperation needs <100ms)
- **Throughput:** How many sensor readings/sec the system can handle (LiDAR produces 1 million points/sec)

---

### Lock-Free Data Structures

**What they are:**
Data structures that don't use mutexes, instead use atomic operations.

**With Locks (mutex):**
```cpp
std::mutex mtx;
std::vector<int> data;

void write(int value) {
    std::lock_guard<std::mutex> lock(mtx);  // Block if someone else has lock
    data.push_back(value);
}

int read() {
    std::lock_guard<std::mutex> lock(mtx);  // Block if writer has lock
    return data.back();
}
```

**Without Locks (atomic):**
```cpp
std::atomic<int> data;

void write(int value) {
    data.store(value, std::memory_order_release);  // Never blocks
}

int read() {
    return data.load(std::memory_order_acquire);  // Never blocks
}
```

**In your project:**
```cpp
// circularBuffer.hpp (Robot Marshal)
template<typename T>
class CircularBuffer {
    std::vector<T> buffer_;
    std::atomic<size_t> write_index_;
    std::atomic<size_t> read_index_;

    // No mutexes - uses atomic operations
    void push(T value) {
        size_t idx = write_index_.fetch_add(1) % capacity_;
        buffer_[idx] = value;
    }
};
```

**Why it matters:**
Lock contention (threads waiting for locks) kills performance. Lock-free structures guarantee forward progress - no thread ever waits.

**Why it matters for robotics:**
Real-time control can't tolerate unpredictable lock waits. Lock-free data structures provide bounded latency guarantees.

---

## 4. Data Storage & Formats

### HDF5 (Hierarchical Data Format)

**What it is:**
A binary file format designed for storing large numerical arrays (scientific data).

**Structure:**
```
demo.mrd  (HDF5 file)
├── /images/
│   ├── /data              [Dataset: 1000 × 1 × 5 × 64 × 64, float32]
│   ├── /metadata          [Attributes: acquisition params]
├── /kspace/
│   ├── /raw               [Dataset: complex64]
└── /reconstruction/
    ├── /result            [Dataset: float32]
```

Like a filesystem inside a file. Groups = directories, Datasets = files.

**Why use HDF5 over raw binary?**

**Raw binary:**
```
frame_000.bin  (262,144 bytes)
frame_001.bin  (262,144 bytes)
...
frame_999.bin
```
- No metadata (what size? what type?)
- No indexing (reading frame 500 requires opening 500 files)
- No compression

**HDF5:**
```
data.h5  (containing all 1000 frames + metadata)
- Self-describing (includes dimensions, data types)
- Random access (read frame 500 directly)
- Compression (can be 10x smaller)
- SWMR mode (concurrent access)
```

**In your project:**
```python
import h5py

# Open file in SWMR mode
file = h5py.File('demo.mrd', 'r', swmr=True)
dataset = file['/images/data']

# Read specific frame
frame_42 = dataset[42]  # Shape: [1, 5, 64, 64]

# Dimensions: [frames, channels, z_slices, y, x]
```

**Why it matters for robotics:**
Autonomous vehicle collects 100GB/hour (cameras, LiDAR, IMU). HDF5 stores it all in one file with timestamps, sensor IDs, calibration data - all queryable. Analysis tools can jump to "frame at timestamp 12:34:56" without scanning entire file.

---

### ISMRMRD (Imaging Sequence Model for Raw Data)

**What it is:**
A standardized format for MRI scanner raw data (k-space).

**Structure:**
```cpp
struct AcquisitionHeader {
    uint16_t version;                    // Format version
    uint64_t flags;                      // Acquisition flags
    uint32_t measurement_uid;            // Scan ID
    uint32_t scan_counter;               // Acquisition number
    uint32_t acquisition_time_stamp;     // Time
    uint32_t number_of_samples;          // Readout samples (256)
    uint16_t active_channels;            // Coils (1-32)
    float position[3];                   // Patient position
    EncodingCounters idx;                // k-space indices
    // ... more fields (total 340 bytes)
};

struct Acquisition {
    AcquisitionHeader header;  // 340 bytes
    float data[];              // number_of_samples × channels × 2 (complex)
};
```

**Why standardized format?**
Different MRI scanners (Siemens, GE, Philips) produce different proprietary formats. ISMRMRD is vendor-neutral, so reconstruction software works with any scanner.

**In your project:**
```cpp
// K-space streamer generates ISMRMRD acquisitions
ISMRMRD::Acquisition acq;
acq.setNumberOfSamples(256);
acq.setActiveChannels(1);
acq.idx.slice = 0;

// Marshal parses ISMRMRD and stores to HDF5
mrd_sink.append_frame(stream_id, ismrmrd_data);
```

**Why it matters for robotics:**
Similar to ROS messages - standardized format means interoperability. Your LiDAR driver outputs PointCloud2 messages, any ROS node can consume them. ISMRMRD does same for MRI data.

---

### JSON (JavaScript Object Notation)

**What it is:**
A human-readable text format for structured data.

**Example:**
```json
{
  "frame_index": 42,
  "timestamp": "2026-01-27T12:34:56.789Z",
  "dims": {
    "x": 64,
    "y": 64,
    "z": 5
  },
  "path": "/session-data/demo.mrd"
}
```

**Why use JSON?**
- Human-readable (can inspect with text editor)
- Language-agnostic (Python, C++, JavaScript all have JSON libraries)
- Web-friendly (native format for HTTP APIs)

**In your project:**
- **API responses:** All HTTP endpoints return JSON
- **Configuration:** Docker Compose, environment variables
- **Logging:** JSONL (JSON Lines - one JSON object per line)

**JSONL format:**
```jsonl
{"frame_index": 0, "timestamp": "2026-01-27T12:00:00.000Z", "type": "mrd"}
{"frame_index": 1, "timestamp": "2026-01-27T12:00:00.100Z", "type": "mrd"}
{"frame_index": 2, "timestamp": "2026-01-27T12:00:00.200Z", "type": "mrd"}
```

Each line is valid JSON. Can append without parsing entire file.

---

### Binary vs Text Formats

**Binary:**
- **Pros:** Compact, fast to parse, efficient
- **Cons:** Not human-readable, requires schema
- **Examples:** HDF5, Protocol Buffers, ISMRMRD
- **Use for:** Large numerical data, images, sensor data

**Text:**
- **Pros:** Human-readable, debuggable, self-describing
- **Cons:** Larger size, slower to parse
- **Examples:** JSON, XML, YAML
- **Use for:** Configuration, API responses, logs

**In your project:**
- **Binary:** MRI frames (64×64×5 float32 = 80KB), k-space data
- **Text:** API responses (metadata), configuration files, logs

---

## 5. Software Architecture Patterns

### Publish-Subscribe (Pub-Sub)

**What it is:**
A messaging pattern where publishers send messages to topics, subscribers receive messages from topics they're interested in.

**How it works:**
```
Publisher 1 ──┐
Publisher 2 ──┼──▶ TOPIC: "mrd_frames" ──┬──▶ Subscriber A
Publisher 3 ──┘                           ├──▶ Subscriber B
                                          └──▶ Subscriber C
```

Publishers don't know who subscribers are. Subscribers don't know who publishers are.

**In your project (WebSocket):**
```
kspace-streamer ───▶ mri-marshal ───▶ WebSocket Topic: "mrd"
                                           │
                                           ├───▶ viz-client 1
                                           ├───▶ viz-client 2
                                           └───▶ monitoring-dashboard
```

**Subscribe:**
```json
{"subscribe": "mrd"}
```

**Publish (from marshal):**
```json
{"type": "mrd", "frame_index": 42}
```

**Why it matters for robotics:**
ROS (Robot Operating System) is built on pub-sub. Perception publishes to `/camera/image`, planning subscribes. Perception doesn't know/care who's listening. Planning doesn't know/care who's publishing. Decouples components.

---

### Blackboard Pattern

**What it is:**
Multiple agents communicate by reading/writing to a shared "blackboard" (shared state). No direct agent-to-agent communication.

**How it works:**
```
Agent A ─┐
Agent B ─┼──▶ BLACKBOARD ◀──┬── Agent D
Agent C ─┘    (shared state)  └── Agent E

Agent A writes "sensor_data"
Agent B reads "sensor_data", writes "object_detections"
Agent C reads "object_detections", writes "motion_plan"
Agent D reads "motion_plan", writes "motor_commands"
```

**In your project (Robot Marshal):**
```
Catheter Tracking:
  Reads: localization_data
  Writes: tip_position_orientation

Planning:
  Reads: tip_position_orientation, surface_model_parameters, user_input
  Writes: desired_planned_motion

Controller:
  Reads: desired_planned_motion
  Writes: forward_kinematics
```

All communication via Robot Marshal's shared files. Clients never talk directly.

**API:**
```bash
# Agent A writes
POST http://robot-marshal:8081/write/tip_position_orientation
Body: {"values": [1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 1.0]}

# Agent B reads
GET http://robot-marshal:8081/read/tip_position_orientation
Response: {"entries": [{"sent_at": 1234567890, "values": [1.0, 2.0, 3.0, ...]}]}
```

**Why it matters:**
Classic AI pattern for coordinating multiple reasoning agents. Used in robotic systems, game AI, expert systems. Agents are loosely coupled - can add/remove agents without changing others.

---

### API Gateway Pattern

**What it is:**
A single entry point that routes requests to multiple backend services.

**Without Gateway:**
```
Client ──▶ Service A (port 8001)
Client ──▶ Service B (port 8002)
Client ──▶ Service C (port 8003)
```
Client needs to know all service addresses.

**With Gateway:**
```
Client ──▶ API Gateway ──┬──▶ Service A
                         ├──▶ Service B
                         └──▶ Service C
```
Client only knows gateway address.

**In your project:**
Your marshals act as mini-gateways:
- **MRI Marshal:** Single endpoint (port 8080) routes to MRD sink, bio handler, pose handler
- **Robot Marshal:** Single endpoint (port 8081) routes to 13 data channels

**Why it matters for robotics:**
Robot exposes 50 internal services (motors, sensors, planning, etc.). External clients connect to API gateway, which routes to appropriate internal service. Simplifies client code, centralizes auth/logging.

---

### Callback Pattern

**What it is:**
Registering a function/URL to be called when an async operation completes.

**Synchronous:**
```python
result = expensive_function()  # Wait for result
process(result)
```

**Asynchronous with Callback:**
```python
expensive_function_async(callback=process)  # Return immediately
# Later, when done: callback(result)
```

**In your project:**
```python
# Marshal sends reconstruction request with callback URL
POST http://mock-recon:9002/reconstruct
Headers:
  X-MRD-Callback: http://mri-marshal:8080/v1/mrd/frame
Body: <k-space data>

# Mock recon returns immediately
Response: 202 Accepted

# Later, mock recon calls back with result
POST http://mri-marshal:8080/v1/mrd/frame
Headers:
  X-MRD-Stream: raw_scan
Body: <reconstructed image>
```

**Why it matters for robotics:**
Motion planning takes 100ms. Control loop can't wait. Instead:
1. Control loop: "Plan path to X" (with callback)
2. Control loop continues with current plan
3. When planning finishes, callback updates plan
4. Control loop switches to new plan

---

## 6. Container & Orchestration

### Docker Container

**What it is:**
A lightweight, standalone package containing an application and all its dependencies (libraries, runtime, system tools).

**Virtual Machine vs Container:**

**VM:**
```
┌─────────────────────┐
│   Application       │
│   ├─ Libraries      │
│   └─ Runtime        │
├─────────────────────┤
│   Guest OS          │  ← Full OS (1-2 GB)
├─────────────────────┤
│   Hypervisor        │
├─────────────────────┤
│   Host OS           │
└─────────────────────┘
```

**Container:**
```
┌─────────────────────┐
│   Application       │
│   ├─ Libraries      │
│   └─ Runtime        │
├─────────────────────┤
│   Docker Engine     │
├─────────────────────┤
│   Host OS           │
└─────────────────────┘
```

Containers share host OS kernel, much lighter (~100 MB vs 1-2 GB).

**Dockerfile:**
```dockerfile
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y python3
COPY app.py /app/
CMD ["python3", "/app/app.py"]
```

**Build & Run:**
```bash
docker build -t my-app .
docker run -p 8080:8080 my-app
```

**In your project:**
Each service has a Dockerfile:
- `docker/Dockerfile.kspace-streamer` (C++ app)
- `docker/Dockerfile.mock-recon` (Python app)
- `docker/mri-marshal.Dockerfile` (C++ app)

**Why it matters for robotics:**
Robotic software has complex dependencies (ROS, OpenCV, TensorFlow). Docker packages everything. Ship the container, guaranteed to work anywhere.

---

### Docker Compose

**What it is:**
A tool for defining and running multi-container Docker applications.

**Problem:**
Running 13 services manually:
```bash
docker run mri-marshal ...
docker run robot-marshal ...
docker run kspace-streamer ...
# ... 10 more commands
```

**Solution (docker-compose.yml):**
```yaml
services:
  mri-marshal:
    image: cwru/mri-marshal:latest
    ports: ["8080:8080"]
  robot-marshal:
    image: cwru/robot-marshal:latest
    ports: ["8081:8081"]
  kspace-streamer:
    image: cwru/kspace-streamer:latest
    depends_on: [mri-marshal]
```

**Run all:**
```bash
docker-compose up  # Starts all 13 services
```

**In your project:**
`docker-compose.demo.yml` defines:
- 13 services
- Network configuration
- Volume mounts
- Environment variables
- Health checks
- Dependencies

**Why it matters for robotics:**
Robot system has 20+ services (drivers, perception, planning, control, logging, monitoring). Docker Compose defines the entire stack, one command to deploy.

---

### Docker Networks

**What they are:**
Virtual networks that allow containers to communicate.

**Bridge Network (default):**
```
Host Machine
┌─────────────────────────────────────┐
│  Docker Bridge: cwru-demo-net       │
│  ┌──────────┐    ┌──────────┐      │
│  │ Marshal  │───▶│ Streamer │      │
│  │ :8080    │    │          │      │
│  └──────────┘    └──────────┘      │
└─────────────────────────────────────┘
```

Containers can reach each other by name (DNS).

**In your project:**
```yaml
networks:
  cwru-net:
    driver: bridge

services:
  mri-marshal:
    networks: [cwru-net]
  kspace-streamer:
    networks: [cwru-net]
    environment:
      - MARSHAL_ENDPOINT=http://mri-marshal:8080
```

**Communication:**
- Inside network: `http://mri-marshal:8080` (container name)
- From host: `http://localhost:8080` (exposed port)

**Why it matters for robotics:**
Isolates robot network from external network. Only expose what's needed (e.g., expose UI, keep internal sensors isolated).

---

### Volumes & Bind Mounts

**What they are:**
Ways to persist data outside containers.

**Problem:**
Container writes file → container stops → file is GONE.

**Solution (Volume):**
```yaml
services:
  mri-marshal:
    volumes:
      - session-data:/session-data  # Named volume

volumes:
  session-data:  # Managed by Docker
```

**Solution (Bind Mount):**
```yaml
services:
  mri-marshal:
    volumes:
      - ./my-data:/session-data  # Host directory
```

**Bind mount vs Volume:**
- **Bind mount:** Direct mapping to host directory (easy to access from host)
- **Volume:** Docker-managed storage (better performance, portable)

**In your project:**
```yaml
volumes:
  - ${SESSION_DATA_DIR:-./session-data}:/session-data
```

Host directory `./session-data` mounted into container at `/session-data`. MRI frames written to `/session-data/demo.mrd` appear in host's `./session-data/demo.mrd`.

**Why it matters:**
Training data for robot ML models (terabytes) needs to persist across container restarts. Mount host storage into container, train model, container crashes, data is safe.

---

## 7. Real-Time Systems

### Latency Budgets

**What they are:**
Time limits for each operation in a real-time pipeline.

**Example (100ms end-to-end budget):**
```
Operation              | Latency | Cumulative
-----------------------|---------|------------
Sensor read            |   5ms   |   5ms
Network transmission   |  10ms   |  15ms
Data processing        |  30ms   |  45ms
Planning algorithm     |  40ms   |  85ms
Command transmission   |  10ms   |  95ms
Actuator response      |   5ms   | 100ms ✓
```

**If any step exceeds budget:**
```
Planning algorithm     |  60ms   |  85ms → 105ms ✗ MISSED DEADLINE
```

**In your project:**
```
Operation              | Budget  | Actual
-----------------------|---------|--------
POST /v1/mrd/frame     |  10ms   |  1ms ✓
WebSocket broadcast    |   5ms   |  0.5ms ✓
HDF5 write             |  10ms   |  2ms ✓
Client notification    |   5ms   |  1ms ✓
Client HDF5 read       |  10ms   |  0.5ms ✓
Client render          |  60ms   |  40ms ✓
Total (end-to-end)     | 100ms   |  45ms ✓
```

**Why it matters for robotics:**
Teleoperation requires <100ms human-to-robot-to-human latency. Every millisecond counts. Need to profile each component, optimize hot paths, use async I/O strategically.

---

### Jitter

**What it is:**
Variation in timing. Even if average latency is good, high jitter causes problems.

**Low Jitter (good):**
```
Request 1: 10ms
Request 2: 10ms
Request 3: 10ms
Average: 10ms, Max: 10ms
```

**High Jitter (bad):**
```
Request 1:  5ms
Request 2: 20ms
Request 3:  8ms
Average: 11ms, Max: 20ms
```

**Why jitter matters:**
Control loop runs at 100Hz (10ms period). If one iteration takes 20ms, you've missed a deadline. Robot motion becomes jerky.

**How to reduce jitter:**
1. **Real-time OS:** Guarantees scheduling
2. **Lock-free data structures:** No unpredictable lock waits
3. **Async I/O:** Don't block on disk/network
4. **Memory pre-allocation:** No malloc in hot path (malloc can take 1ms)

**In your project:**
Async I/O keeps HTTP response time consistent (~1ms). Without it, occasional 5ms disk writes would cause jitter.

---

### Throughput vs Real-Time

**Throughput system:**
Goal: Process as much data as possible (GB/sec).
Okay if occasionally slow (batch processing).

**Real-time system:**
Goal: Meet every deadline (ms).
Okay if total throughput is lower.

**Example:**

**Throughput-optimized:**
```
Buffer 100 frames → process all at once (GPU batch)
Average: 100 frames in 1 second (100 FPS)
Latency: 1 second per frame
```

**Real-time-optimized:**
```
Process each frame immediately
Average: 50 frames in 1 second (50 FPS)
Latency: 20ms per frame ✓
```

Real-time system processes fewer frames total, but each frame has predictable low latency.

**In your project:**
You prioritize real-time (sub-100ms latency) over max throughput. Could batch frames for higher FPS, but would increase latency.

**Why it matters for robotics:**
Autonomous vehicle perception needs 100ms latency (for braking). Don't care if system could process 1000 FPS in batch mode - need low latency per frame.

---

### Event-Driven Architecture

**What it is:**
System reacts to events (messages, signals) rather than polling.

**Polling (bad):**
```cpp
while (true) {
    if (has_new_frame()) {
        process_frame();
    }
    sleep(10ms);  // Check every 10ms
}
```
Wastes CPU checking. Latency: 0-10ms (depends when frame arrives).

**Event-Driven (good):**
```cpp
register_callback(on_new_frame);

void on_new_frame(frame) {
    process_frame();  // Called immediately
}
```
CPU sleeps until event arrives. Latency: ~0ms (immediate notification).

**In your project:**
```cpp
// WebSocket event-driven
ws_client.on_message([](json msg) {
    if (msg["type"] == "mrd") {
        viz_client.display_frame(msg["frame_index"]);
    }
});
```

Marshal pushes notification → viz client wakes up → displays frame. No polling.

**Why it matters for robotics:**
Sensors generate events (new LiDAR scan). Event-driven system reacts immediately. Polling wastes CPU and adds latency.

---

## Summary: Key Concepts for Interview

### Communication Layer
- **WebSocket:** Bidirectional, persistent connection for real-time push notifications
- **REST API:** Stateless HTTP for service-to-service communication
- **Headers:** Metadata for requests (stream ID, callback URLs, session tracking)

### Distributed Systems
- **Microservices:** 13+ independent services, communicate via HTTP/WebSocket
- **Service Discovery:** Docker DNS (reference by name, not IP)
- **Health Checks:** Automated monitoring, auto-restart on failure
- **Async Processing:** Return immediately, do work in background, callback with result

### Concurrency
- **SWMR:** One writer, many readers, no blocking
- **Async I/O:** Background threads for slow operations (disk/network)
- **Lock-Free:** Atomic operations instead of mutexes (bounded latency)
- **Thread Pool:** Fixed threads handle concurrent requests

### Storage
- **HDF5:** Binary format for large numerical arrays, supports SWMR
- **ISMRMRD:** Standardized MRI data format (vendor-neutral)
- **JSONL:** Append-only logging (one JSON per line)

### Architecture
- **Pub-Sub:** Publishers → topics → subscribers (decoupled)
- **Blackboard:** Agents read/write shared state, no direct communication
- **Callback:** Async operations notify completion via callback URL

### DevOps
- **Docker:** Containerize services with dependencies
- **Docker Compose:** Orchestrate multi-container applications
- **Volumes:** Persist data outside containers

### Real-Time
- **Latency Budget:** Time limit per operation (~100ms total)
- **Jitter:** Minimize timing variation (use async I/O, lock-free)
- **Event-Driven:** React to events, don't poll

---

## How These Apply to Robotics

**Your MRI System → Amazon's Robot Fleet:**

| Your System | Robotics Equivalent |
|-------------|---------------------|
| MRI frames at 20 FPS | LiDAR scans at 10 Hz |
| WebSocket notifications | ROS pub-sub topics |
| SWMR HDF5 reads | Shared memory for sensor data |
| Async reconstruction | Async motion planning |
| Microservices (13 containers) | Robot services (perception, planning, control) |
| Docker Compose orchestration | Kubernetes for robot fleet |
| Sub-100ms latency | Teleoperation requirements |
| Health checks + auto-restart | Robot fault tolerance |

**Your transferable skills:**
- Low-latency distributed systems ✓
- Real-time data streaming ✓
- Concurrent access patterns ✓
- Service coordination ✓
- Async processing ✓
- Containerized deployment ✓

The domain is different (medical imaging vs robotics), but the technical challenges are identical.
