# SWMR Concurrent Operations During Reconstruction

**Document Purpose:** Explains how SWMR (Single Writer Multiple Readers) enables concurrent operations when using reconstruction service.

**Last Updated:** 2026-01-29

---

## The Question

**When sending raw k-space to reconstruction client, can we:**
1. POST raw data to reconstruction service
2. Receive reconstructed data back
3. Write to SWMR file
4. Have visualization clients read from SWMR simultaneously

**Answer: YES, with proper understanding of the concurrency model.**

---

## Overview: What Happens Concurrently

```
┌─────────────────────────────────────────────────────────────────┐
│                  CONCURRENT OPERATIONS TIMELINE                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Thread 1: Marshal HTTP Handler (Reconstruction Flow)          │
│  ──────────────────────────────────────────────────────────     │
│                                                                 │
│  t=0.000s   Receive raw k-space from scanner                   │
│  t=0.001s   POST to reconstruction-service:9002/reconstruct    │
│  t=0.001s   [WAITING for reconstruction...]                    │
│             ↓                                                   │
│             [Marshal thread is BLOCKED, waiting for HTTP        │
│              response from reconstruction service]              │
│             ↓                                                   │
│  t=5.000s   Reconstruction completes                            │
│  t=5.001s   Receive reconstructed ImageHeader + pixels         │
│  t=5.002s   Append to SWMR file (exclusive write lock)         │
│  t=5.007s   Write complete, release lock                       │
│  t=5.008s   Return HTTP 201 to scanner                         │
│                                                                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Threads 2-N: Visualization Clients (Reading SWMR)             │
│  ─────────────────────────────────────────────────────────      │
│                                                                 │
│  t=0.000s   Viz Client 1: Reading frame 41 from SWMR   ✅      │
│  t=0.100s   Viz Client 2: Reading frame 40 from SWMR   ✅      │
│  t=1.000s   Viz Client 1: Reading frame 41 again       ✅      │
│  t=2.000s   Viz Client 3: Reading frame 39 from SWMR   ✅      │
│             ...                                                 │
│             [All reading while marshal waits for recon]         │
│             ...                                                 │
│  t=5.002s   Viz Client 1: Reading frame 41             ✅      │
│             [Marshal writing frame 42 - brief lock]             │
│  t=5.007s   Viz Client 1: Reading frame 42             ✅      │
│             [New frame now available]                           │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**Key Insight:** Most of the time (during reconstruction), the marshal is **NOT** writing to SWMR. It's waiting for HTTP response. During this time, viz clients read freely with zero contention.

---

## Detailed Flow Analysis

### Phase 1: Marshal Sends Raw K-Space (No SWMR Activity)

```
Scanner/Client                    Marshal                    Reconstruction Service
      │                              │                              │
      │ POST raw k-space             │                              │
      ├─────────────────────────────>│                              │
      │                              │ POST /reconstruct            │
      │                              │ (raw k-space data)           │
      │                              ├─────────────────────────────>│
      │                              │                              │
      │                              │ [BLOCKED - Waiting]          │
      │                              │                              │
```

**SWMR Status:** No activity
**Viz Clients:** Can read freely ✅

---

### Phase 2: Reconstruction Processing (No SWMR Activity)

```
Scanner/Client                    Marshal                    Reconstruction Service
      │                              │                              │
      │                              │                              │ [Processing]
      │                              │ [BLOCKED - Waiting]          │ • Parse k-space
      │                              │                              │ • 2D/3D FFT
      │                              │                              │ • Create ImageHeader
      │                              │                              │ • Generate pixels
      │                              │                              │
```

**Duration:** 1-60 seconds (depends on image size and algorithm)
**SWMR Status:** No activity
**Viz Clients:** Can read freely ✅

**Concurrent Operations During This Phase:**
```
Reconstruction Service: Processing frame 42
Viz Client 1:          Reading frame 41   ✅
Viz Client 2:          Reading frame 40   ✅
Viz Client 3:          Reading frame 39   ✅
```

---

### Phase 3: Reconstruction Returns (No SWMR Activity Yet)

```
Scanner/Client                    Marshal                    Reconstruction Service
      │                              │                              │
      │                              │ HTTP 200 OK                  │
      │                              │ ImageHeader + pixels         │
      │                              │<─────────────────────────────┤
      │                              │                              │
      │                              │ [Parse response]             │
      │                              │ [Validate ImageHeader]       │
      │                              │ [Extract pixel data]         │
```

**SWMR Status:** No activity
**Viz Clients:** Can read freely ✅

---

### Phase 4: Marshal Writes to SWMR (Brief Exclusive Lock)

```
Scanner/Client                    Marshal                    SWMR File
      │                              │                              │
      │                              │ Open SWMR file               │
      │                              ├─────────────────────────────>│
      │                              │                              │
      │                              │ EXCLUSIVE WRITE LOCK         │
      │                              │ Append frame 42              │
      │                              │ (ImageHeader + pixels)       │
      │                              │ Flush to disk                │
      │                              │ Release lock                 │
      │                              │<─────────────────────────────┤
```

**Duration:** ~1-10ms (very brief)
**SWMR Status:** WRITE LOCK (exclusive)
**Viz Clients:**
- Reading old frames (0-41): ✅ No wait
- Trying to read frame 42: ⏸️ Brief wait (~5ms) until write completes

**What Happens to Viz Clients:**
```
During marshal write (5ms):
  Viz Client 1 reading frame 41: ✅ Continues normally
  Viz Client 2 reading frame 42: ⏸️ Waits for lock release
  Viz Client 3 reading frame 40: ✅ Continues normally
```

---

### Phase 5: Marshal Responds to Scanner (No SWMR Activity)

```
Scanner/Client                    Marshal                    SWMR File
      │                              │                              │
      │ HTTP 201 Created             │                              │
      │<─────────────────────────────┤                              │
      │ {                            │                              │
      │   "frame_index": 42,         │                              │
      │   "reconstructed": true      │                              │
      │ }                            │                              │
```

**SWMR Status:** No activity
**Viz Clients:** Can read frame 42 now ✅

---

## Concurrency Summary

### What Can Happen Simultaneously

| Marshal Activity | Viz Client Activity | Possible? |
|------------------|---------------------|-----------|
| Sending to recon service | Reading SWMR | ✅ YES (no conflict) |
| Waiting for recon response | Reading SWMR | ✅ YES (no conflict) |
| Parsing recon response | Reading SWMR | ✅ YES (no conflict) |
| **Writing to SWMR** | **Reading different frame** | ✅ YES (SWMR allows) |
| **Writing to SWMR** | **Reading same frame being written** | ⏸️ BRIEF WAIT (~5ms) |
| Responding to scanner | Reading SWMR | ✅ YES (no conflict) |

### Latency Breakdown

**Total time for raw k-space → stored reconstructed frame:**
```
Marshal receives raw:         0ms    (t=0)
POST to recon service:        1ms    (network)
Reconstruction processing:    5000ms (algorithm - most time spent here)
Receive response:             1ms    (network)
Parse response:               1ms    (CPU)
Write to SWMR:               5ms    (disk I/O with lock)
Respond to scanner:          1ms    (network)
─────────────────────────────────
Total:                       5009ms ≈ 5 seconds
```

**Of this 5 seconds:**
- **4999ms (99.8%):** Marshal NOT touching SWMR (viz clients read freely)
- **5ms (0.1%):** Marshal writing to SWMR (brief lock)
- **5ms (0.1%):** Other processing

---

## SWMR Guarantees

### What SWMR Enables

```cpp
// HDF5 SWMR mode allows:

Single Writer (Marshal):
  - Can append new frames
  - Exclusive write lock (brief)
  - Must flush after write

Multiple Readers (Viz Clients):
  - Can read existing frames while writer appends
  - See new frames after writer flushes
  - No lock needed for reading old frames
  - Brief wait only if reading frame being written
```

### Example: 3 Viz Clients + 1 Marshal

```
t=0: Marshal writing frame 42
     Viz Client A: Reading frame 41  ✅ No wait
     Viz Client B: Reading frame 40  ✅ No wait
     Viz Client C: Reading frame 42  ⏸️  Waits ~5ms

t=5ms: Marshal finished writing frame 42
       Viz Client A: Reading frame 41  ✅ Still works
       Viz Client B: Reading frame 42  ✅ Now available
       Viz Client C: Reading frame 42  ✅ Lock released, reads now
```

---

## Practical Example: Real-Time Cardiac Imaging

### Scenario
- Scanner sends 20 frames/second of raw k-space
- Each reconstruction takes 3 seconds
- 5 viz clients are viewing live

### Timeline

```
t=0.00s  Frame 0: Scanner sends raw k-space
         → Marshal POSTs to recon service
         → Viz clients reading previous frames

t=0.05s  Frame 1: Scanner sends raw k-space
         → Marshal POSTs to recon service (new thread)
         → Viz clients reading previous frames

t=0.10s  Frame 2: Scanner sends raw k-space
         → Marshal POSTs to recon service (new thread)
         → Viz clients reading previous frames

... (frames keep coming at 20 fps) ...

t=3.00s  Frame 0 reconstruction completes
         → Marshal writes to SWMR (5ms)
         → Viz clients can now see frame 0

t=3.05s  Frame 1 reconstruction completes
         → Marshal writes to SWMR (5ms)
         → Viz clients can now see frame 1

t=3.10s  Frame 2 reconstruction completes
         → Marshal writes to SWMR (5ms)
         → Viz clients can now see frame 2
```

**Key Points:**
1. Frames arrive at 20 fps
2. Reconstruction takes 3 seconds per frame
3. But multiple reconstructions happen in parallel (different threads/services)
4. SWMR writes happen ~20 times/second (5ms each)
5. Viz clients read freely 99.9% of the time

---

## Thread Safety Model

### Marshal Side (Writer)

```cpp
// Simplified marshal flow
void handle_raw_kspace(const std::vector<uint8_t>& raw_data) {
    // Phase 1: Send to reconstruction (no SWMR activity)
    auto reconstructed = send_to_recon_service(raw_data);
    // Time: ~5 seconds, SWMR readers have zero contention

    // Phase 2: Parse response (no SWMR activity)
    auto image_header = parse_image_header(reconstructed);
    auto pixels = extract_pixels(reconstructed);
    // Time: ~1ms, SWMR readers have zero contention

    // Phase 3: Write to SWMR (exclusive lock)
    {
        std::lock_guard<std::mutex> lock(swmr_write_mutex);
        swmr_file.append_frame(image_header, pixels);
        swmr_file.flush();
    }
    // Time: ~5ms, SWMR readers may briefly wait

    // Phase 4: Respond to client (no SWMR activity)
    return http_201_created(frame_index);
}
```

### Viz Client Side (Readers)

```cpp
// Simplified viz client flow
void read_latest_frame() {
    // No lock needed for reading
    auto frame_index = swmr_file.get_latest_frame_index();

    if (frame_index > last_displayed_frame) {
        // Read new frame
        auto frame = swmr_file.read_frame(frame_index);
        // May briefly wait (~5ms) if marshal is writing THIS frame
        // No wait if reading old frames

        display(frame);
        last_displayed_frame = frame_index;
    }
}
```

---

## Performance Characteristics

### Contention Analysis

**For a typical frame:**
- Total time frame exists: 50ms (at 20 fps)
- SWMR write lock duration: 5ms
- Lock-free time: 45ms
- **Contention ratio: 5/50 = 10%**

**For reconstruction scenario:**
- Total time from raw → stored: 5000ms
- SWMR write lock duration: 5ms
- Lock-free time: 4995ms
- **Contention ratio: 5/5000 = 0.1%**

**Conclusion:** With reconstruction, SWMR contention is negligible.

### Throughput

**Without reconstruction:**
- Frame ingestion: ~1ms per frame
- SWMR write: ~5ms per frame
- Max throughput: ~200 frames/sec

**With reconstruction:**
- Frame ingestion: ~5000ms per frame (but parallel)
- SWMR write: ~5ms per frame
- Max throughput: Limited by reconstruction service, not SWMR

---

## Common Scenarios

### Scenario 1: Real-Time Streaming (No Reconstruction)

```
Scanner sends IMAGE (reconstructed) at 20 fps:

t=0.000s  Frame 0: POST → SWMR write (5ms) → Available
t=0.050s  Frame 1: POST → SWMR write (5ms) → Available
t=0.100s  Frame 2: POST → SWMR write (5ms) → Available

Viz clients see frames with ~5-10ms latency
SWMR contention: 10% (5ms write / 50ms frame time)
```

### Scenario 2: Reconstruction (Async)

```
Scanner sends RAW K-SPACE at 20 fps:

t=0.000s  Frame 0: POST → Recon (3s) → SWMR write (5ms)
t=0.050s  Frame 1: POST → Recon (3s) → SWMR write (5ms)
t=0.100s  Frame 2: POST → Recon (3s) → SWMR write (5ms)

t=3.000s  Frame 0 appears in SWMR (reconstructed)
t=3.050s  Frame 1 appears in SWMR (reconstructed)
t=3.100s  Frame 2 appears in SWMR (reconstructed)

Viz clients see frames with ~3 second latency
SWMR contention: 0.1% (5ms write / 3000ms recon time)
```

### Scenario 3: Mixed Load

```
Multiple operations happening:

Thread 1: Frame 42 - Waiting for reconstruction
Thread 2: Frame 43 - Writing to SWMR (5ms lock)
Thread 3: Frame 44 - Sending to recon service
Viz Client 1: Reading frame 41
Viz Client 2: Reading frame 40
Viz Client 3: Trying to read frame 43 (waits ~5ms)

All operations concurrent, minimal contention
```

---

## Error Handling

### Reconstruction Timeout

```cpp
// If reconstruction takes too long
auto reconstructed = send_to_recon_service(raw_data, timeout=60s);
if (timeout_occurred) {
    // No SWMR write happens
    // Viz clients not affected
    // Frame gap in sequence
    return http_502_bad_gateway("Reconstruction timeout");
}
```

### SWMR Write Failure

```cpp
try {
    swmr_file.append_frame(image_header, pixels);
    swmr_file.flush();
} catch (const hdf5_error& e) {
    // Reconstruction succeeded but storage failed
    // Data lost for this frame
    // Viz clients not affected
    return http_500_internal_server_error("SWMR write failed");
}
```

---

## Optimization Strategies

### 1. Parallel Reconstruction

```
Run multiple reconstruction service instances:
- Frame 0 → Recon Instance 1
- Frame 1 → Recon Instance 2
- Frame 2 → Recon Instance 3

All reconstruct in parallel
SWMR writes happen sequentially but quickly (5ms each)
```

### 2. Asynchronous Write Queue

```cpp
// Instead of blocking marshal thread on SWMR write
auto reconstructed = send_to_recon_service(raw_data);
write_queue.enqueue(reconstructed);
// Marshal returns immediately

// Separate writer thread drains queue
while (true) {
    auto frame = write_queue.dequeue();
    swmr_file.append_frame(frame);
}
```

### 3. Batch Writes

```cpp
// Accumulate multiple frames
std::vector<Frame> batch;
batch.push_back(frame_42);
batch.push_back(frame_43);
batch.push_back(frame_44);

// Write all at once (single lock)
{
    std::lock_guard lock(swmr_write_mutex);
    for (const auto& frame : batch) {
        swmr_file.append_frame(frame);
    }
    swmr_file.flush();
}
// Time: ~15ms (3 frames), but only one lock
```

---

## Summary

### Question: Can We Do All This Concurrently?

**YES:**

| Operation | Concurrent with SWMR Reads? | Explanation |
|-----------|----------------------------|-------------|
| POST raw k-space to recon service | ✅ YES | No SWMR activity |
| Wait for reconstruction response | ✅ YES | No SWMR activity |
| Parse reconstructed response | ✅ YES | No SWMR activity |
| Write reconstructed frame to SWMR | ⏸️ BRIEF WAIT | 5ms exclusive lock |
| Respond to scanner | ✅ YES | No SWMR activity |

**The Key Insight:**
- **99.9% of the time:** Marshal is NOT writing to SWMR → Viz clients read freely
- **0.1% of the time:** Marshal is writing to SWMR → Viz clients wait ~5ms (barely noticeable)

**SWMR enables true concurrent read/write:**
- Multiple viz clients can read while marshal writes
- Only brief contention when reading the exact frame being written
- Perfect for real-time visualization during reconstruction

---

## References

- **HDF5 SWMR Documentation:** https://portal.hdfgroup.org/display/HDF5/Single+Writer+Multiple+Reader
- **Marshal SWMR Implementation:** `.worktrees/mri_data_marshal/include/mrd_io.hpp`
- **Reconstruction Flow:** [HTTP_ROUTING_EXAMPLES.md](HTTP_ROUTING_EXAMPLES.md)
- **System Diagram:** [SYSTEM_DIAGRAM_COMPLETE.md](SYSTEM_DIAGRAM_COMPLETE.md)
