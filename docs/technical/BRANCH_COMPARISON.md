# Branch Comparison: `robot-data-marshal` vs. `swmr` (Hardened)

## Executive Summary

This document provides a comprehensive technical comparison between two divergent branches of the "Data Marshal" project. 

- **`robot-data-marshal`**: A legacy or alternative implementation focused on **generic, low-frequency state synchronization** for robotics systems. It acts like a "mailbox" where robots leave brief status notes (JSON) for each other, prioritizing simplicity and strict validation.
- **`swmr` (Single Writer Multiple Reader)**: The modern, high-performance evolution used in this hardened branch. It acts like a "firehose" for massive amounts of medical imaging data, prioritizing raw speed, zero-locking, and durable history.

## Feature Comparison Matrix

| Feature | `robot-data-marshal` | Hardened SWMR |
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
1.  **Validation:** It parses the request body as JSON. It strictly checks for three required fields: `sent_at`, `values`, and `client_id`.
2.  **Timestamp:** It injects a `received_at` timestamp (**nanoseconds since epoch**) into the JSON.
3.  **Cache Update:** It acquires a unique lock on the buffer and pushes the JSON string.
4.  **Disk Queueing:** It notifies a background worker thread via a condition variable.

### 3. GET Requests (Reading Data)
**Endpoint:** `GET /read/<filename>?last=<k>`
1.  **Locking:** It acquires a shared lock on the buffer.
2.  **Fetch:** Returns the latest `k` updates from the circular buffer.

---

## Technical Deep Dive: Hardened SWMR

### 1. HTTP API & Data Ingest
The hardened branch provides specialized endpoints for high-throughput binary and metadata ingest:
*   **`POST /v1/mrd/frame`**: Ingests raw binary MRI frames (SWMR mode).
*   **`POST /v1/mrd/ingest`**: Ingests complete `.mrd` files.
*   **`POST /v1/bio/signal`**: Ingests high-frequency physiological waveforms.
*   **`POST /v1/pose/update`**: Ingests JSON scanner geometry.

### 2. Synchronization & Persistence
Unlike the RAM-cached model, this branch uses the filesystem as the primary synchronization layer.
*   **HDF5 SWMR:** The server opens files in Single Writer mode. Readers can read growing datasets without ever acquiring a lock or blocking the server.
*   **Journaling:** All updates are logged to `index.jsonl`, `latest.json`, `poses.jsonl`, and `bio.jsonl`.

### 3. Real-Time Notification (WebSockets)
*   **Fan-out:** Immediately after an ingest operation, the server emits a JSON broadcast to all connected WebSocket clients.
*   **Topics:** Clients can subscribe to specific topics (e.g., `pose`, `bio`, `mrd`) to filter high-frequency traffic.
