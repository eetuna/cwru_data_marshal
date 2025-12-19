# Branch Comparison: `robot-data-marshal` vs. `swmr` (Main)

## Executive Summary

This document provides a comprehensive technical comparison between two divergent branches of the "Data Marshal" project. 

- **`robot-data-marshal`**: A legacy or alternative implementation focused on **generic, low-frequency state synchronization** for robotics systems. It acts like a "mailbox" where robots leave brief status notes (JSON) for each other, prioritizing simplicity and strict validation.
- **`swmr` (Single Writer Multiple Reader)**: The modern, high-performance evolution used in **main**. It acts like a "firehose" for massive amounts of medical imaging data, prioritizing raw speed, zero-locking, and durable history.

## Feature Comparison Matrix

| Feature | `robot-data-marshal` | `swmr` (Main) |
| :--- | :--- | :--- |
| **Primary Domain** | Robotics / Generic State Sync | Medical Imaging (MRI) / Streaming |
| **Data Format** | JSON (Text) | ISMRMRD (Binary), HDF5 |
| **Network Stack** | `httplib.h` (Synchronous) | `Boost.Asio/Beast` (Asynchronous) |
| **Concurrency** | Mutex-based (Blocking) | SWMR-based (Lock-free) |
| **History Model** | RAM (Circular Buffer, last 1000) | Disk (HDF5 / JSONL, unlimited) |
| **Persistence** | Snapshot (Latest value only) | Journal (Full append-only log) |
| **Interaction** | Pull-only (Polling `GET`) | Push (WebSocket) + Pull (HTTP) |
| **Validation** | Strict Schema (Field checks) | Transparent (Pass-through) |
| **Dependencies** | Minimal (`httplib`, `json.hpp`) | Heavy (`HDF5`, `Boost`, `OpenCV`) |

---

## Technical Deep Dive: `robot-data-marshal`

### 1. HTTP Server & Configuration
The server starts by loading a static configuration file (`files.json`) which defines a list of valid filenames (e.g., `["robot_a.json", "robot_b.json"]`).
*   **Initialization:** For each file in this list, it creates:
    *   A **Circular Buffer** in memory (RAM cache) to store the last **1,000** updates.
    *   A **Write Queue** (Circular Buffer) to handle disk I/O asynchronously.
    *   **Mutexes:** A `std::shared_mutex` for reading/writing the cache and a `std::mutex` for the write queue.

### 2. POST Requests (Writing Data)
**Endpoint:** `POST /write/<filename>`
1.  **Validation:** It parses the request body as JSON. It strictly checks for three required fields: `sent_at` (integer), `values` (array), and `client_id` (string). If any are missing, it returns `400 Bad Request`.
2.  **Timestamp:** It injects a `received_at` timestamp (**nanoseconds since epoch**) into the JSON.
3.  **Cache Update (RAM):** It acquires a **unique lock** on the `file_caches` mutex for that specific filename and pushes the JSON string into the circular buffer. This makes the data immediately available to readers.
4.  **Disk Queueing:** It acquires a lock on the `write_queues` mutex and pushes the same JSON string into the write queue.
5.  **Notification:** It notifies a background worker thread via a condition variable.

### 3. GET Requests (Reading Data)
**Endpoint:** `GET /read/<filename>?last=<k>`
1.  **Parameter:** It checks for an optional query parameter `last=k`. The default is `k=1` (get only the absolute latest entry).
2.  **Locking:** It acquires a **shared lock** on the `file_caches` mutex. This allows multiple clients to read simultaneously but blocks if a write (`POST`) is currently updating the buffer.
3.  **Fetch:**
    *   **If k=1:** It peeks at the latest entry in the circular buffer and returns it directly as a JSON object.
    *   **If k > 1:** It iterates backward `k` times through the circular buffer, collecting entries into a JSON array under the key `"entries"`.
4.  **Response:** Returns the JSON payload with `200 OK`, or `404` if the file/buffer is empty.

### 4. Background Persistence (Disk I/O)
A dedicated `background_worker` thread runs constantly:
*   **Atomic Write:** Pops the oldest pending write, writes to `<filename>.tmp`, and performs `std::filesystem::rename` to atomically overwrite the actual file on disk.
*   **Note:** The disk file is a **snapshot** (latest value only); the memory buffer holds the history.

---

## Technical Deep Dive: `swmr` (Main)

### 1. HTTP API & Data Ingest
The `swmr` branch provides specialized endpoints for high-throughput binary and metadata ingest:
*   **`POST /v1/ismrmrd/frame`**: Ingests raw binary MRI frames.
    *   **Headers:** Requires `X-MRD-Stream` (ID of the scan). Optional `X-MRD-Session`.
    *   **Logic:** Interprets the body as an ISMRMRD `ImageHeader` followed by pixel data. It appends this frame to an active HDF5 file.
    *   **Response:** JSON containing `path`, `frame_index`, `flushed` (boolean), and `ts`.
*   **`POST /v1/mrd/ingest`**: Ingests complete `.mrd` files.
*   **`POST /v1/pose/update`**: Ingests JSON scanner geometry (`p` and `R`).
    *   **Logic:** Updates the global `PoseStore` and appends to `poses.jsonl`.

### 2. Synchronization & Persistence (The breakthrough)
Unlike the RAM-cached model, `swmr` uses the filesystem as the primary synchronization layer.
*   **HDF5 SWMR:** The server opens files in Single Writer mode. It appends data and flushes the HDF5 metadata.
*   **Non-Blocking Readers:** Clients open the *same* file in SWMR Read mode. They can read growing datasets without ever acquiring a lock or blocking the server.
*   **Journaling:** All updates are logged to:
    *   `index.jsonl`: A newline-delimited JSON log of every ingested item (The session "database").
    *   `latest.json`: An atomically-updated snapshot of the most recent ingest (for polling clients).
    *   `poses.jsonl`: A permanent log of all geometry changes.

### 3. Modes of Operation
*   **Mode A (Live):** Optimizes for immediate visualization. Writes directly to the `data/mrd` directory.
*   **Mode B (Record/Replay):** Optimizes for data integrity. Creates a timestamped "Dumpbox" session folder. This ensures that a new scan never overwrites previous data and allows for pixel-perfect replay.

### 4. Real-Time Notification (WebSockets)
*   **Fan-out:** Every time an HTTP ingest completes and the disk index is updated, the server broadcasts the metadata JSON to all connected WebSocket clients.
*   **Topics:** Clients can subscribe to specific topics (e.g., `{"subscribe": "pose"}`) to filter high-frequency traffic.
*   **Benefit:** This removes the need for clients to poll HTTP endpoints, as the server "pushes" the update the moment it becomes durable.

---

## Summary of Architectural Differences

1.  **State Model:** `robot-data-marshal` is a **Blackboard** (transient RAM history); `swmr` is a **Journal** (durable Disk history).
2.  **Concurrency:** `robot-data-marshal` uses **Mutexes** (Synchronous/Blocking); `swmr` uses **SWMR** (Asynchronous/Non-blocking).
3.  **Interaction:** `robot-data-marshal` is **Pull-only**; `swmr` is **Push/Pull Hybrid**.
4.  **Reliability:** `swmr` preserves the session against crashes via append-only logs; `robot-data-marshal` loses history on restart.