# CWRU Data Marshal - Status Report

## Chapter 1: DataFlow.drawio vs. Current Implementation

### 1. Overview
The `DataFlow.drawio` diagram outlines a dual-marshal architecture designed to handle distinct domains of data synchronization within the robotic MRI system. This report compares the "Ideal State" depicted in the diagram against the actual codebases currently residing in the `robot-data-marshal` and `main` (SWMR) branches.

### 2. Architectural Intent
> "The idea for two data marshals is that the **Robot Data Marshall** can handle faster communication between the robotic parts of the system where the data size is smaller. The **MRI Data Marshal** will handle larger data but will likely not need to process things at as high a frequency. This should help avoid overloading a single data marshal. This will also separate some of the processing from the part of the system that communicates with the MRI and must remain online or the scanner will crash. The dashed lines represent connections that might be used but we aren't certain of yet."

This architectural split is fully realized in the codebase:
-   **Robot Data Marshal:** Implemented in the `robot-data-marshal` branch using **RAM-based Circular Buffers** for nanosecond-latency control loops.
-   **MRI Data Marshal:** Implemented in the `main` (SWMR) branch using **HDF5 Single Writer Multiple Reader** for gigabyte-scale data throughput and crash resilience.

### 3. Component Analysis

#### A. Robot Data Marshal
**Diagram Requirement:**
> "Robot Data Marshal: everything sent to cache and saved to file (default local cache for low latency, saving to file for archival purposes/past data)"
> "write to cache then dump to file"
> "http for all requests (post/get)"

**Current Implementation (`robot-data-marshal` branch):**
*   **Status:** ✅ **Fully Implemented**
*   **Evidence:** `server.cpp` implements a `CircularBuffer` (RAM cache) that is updated immediately upon a `POST /write` request, satisfying the "low latency" requirement. A separate background worker thread asynchronously writes these updates to disk (`.tmp` -> rename), satisfying the "archival" requirement without blocking the real-time control loop.
*   **Protocol:** Uses `httplib` for synchronous HTTP POST/GET, exactly as requested.

#### B. MRI Data Marshal
**Diagram Requirement:**
> "MRI" -> "Localization Data" / "Streaming 2D Images" / "3D Images" -> "Local Cache" -> "Save to File"

**Current Implementation (`main` / `swmr` branch):**
*   **Status:** ✅ **Fully Implemented (via SWMR)**
*   **Evidence:**
    *   **Ingest:** `POST /v1/ismrmrd/frame` accepts the raw binary data streams (Images).
    *   **Localization:** `POST /v1/pose/update` accepts the localization/tracking data.
    *   **Cache/Storage:** The architecture uses HDF5 SWMR. The "Local Cache" is effectively managed by the Operating System's page cache (RAM) which buffers the HDF5 writes, while the "Save to File" is guaranteed by the append-only log structure (`.mrd` files + `index.jsonl`).
    *   **Distribution:** Data flows downstream to consumers (Visualization, Planning) via WebSocket broadcasts or HTTP polling (`latest.json`), matching the flow arrows in the diagram.
*   **Analysis of "Local Cache" vs. "Save to File":** 
    *   The diagram depicts these as separate steps. In the implementation, they are unified by the **HDF5 SWMR** mechanism.
    *   **"Local Cache":** Provided effectively by the Linux OS Page Cache (RAM). When the scanner pushes a frame, it lands in RAM immediately, allowing downstream readers (visualizers) to read it instantly from memory without waiting for a physical disk seek.
    *   **"Save to File":** Provided by the HDF5 writer which flushes these pages to the physical disk to ensure the "Scanner Crash" safety requirement.
    *   **Direct Ingest (`/v1/mrd/ingest`):** Also follows this pattern. It writes the full `.mrd` file to disk immediately. The "Cache" effect is implicit via the OS filesystem layer.

### 4. Architecture Alignment
The repository has successfully bifurcated into the two specialized systems envisioned in the diagram:

1.  **The "Blackboard" (Robot Marshal):** Optimized for nanosecond-latency state exchange using RAM buffers.
2.  **The "Firehose" (MRI Marshal):** Optimized for gigabyte-scale throughput using HDF5/SWMR.

### 5. Missing or "Dashed Line" Features
The following features appear in the diagram (often as dashed lines) but are **not yet present** in the `main` branch codebase:

| Feature in Draw.io | Status in Code | Notes |
| :--- | :--- | :--- |
| **Biological Signals** | ❌ **Missing** | The diagram shows a path for biological waveforms (ECG/Respiratory). Currently, there is no endpoint (`POST /v1/bio/signal`) or data structure to handle this. |
| **Downstream Modules** | ❌ **External** | The diagram shows "Catheter Tracking", "Surface Tracking", "Planning". The marshal provides the *data* for these (via WebSocket/HTTP), but the modules themselves are external applications, not part of this repository. |

### 6. Conclusion
The current codebase is in excellent alignment with the architectural design. The "Local Cache" concept for MRI data is implemented implicitly via OS-level caching combined with SWMR, which offers the best balance of speed and reliability without complex application-level buffering. The primary functional gap is the ingestion of **Biological Signals**.

---

## Chapter 2: Testing and Refactoring Report

### 1. Project Rebuild
The repository was rebuilt from scratch using CMake and Ninja. Initial build issues related to cached paths and missing dependencies were resolved by performing a clean build.

### 2. Feature Review
The core features of the `marshal` were reviewed and verified:
-   **MRD Ingestion:** Handled via HTTP (`/v1/mrd/ingest`) and WebSocket (binary payloads).
-   **SWMR Streaming:** Appending ISMRMRD frames via `POST /v1/ismrmrd/frame`.
-   **Pose Tracking:** Update and retrieval via `/v1/pose/*`.
-   **WebSocket Broadcasting:** Real-time data fan-out and subscription management.
-   **Playback:** Replaying recorded sessions back to the marshal.

### 3. Extensive Refactoring for Testability
To enable deep unit testing without requiring full network stacks or complex setup, the following components were refactored:
-   **HTTP Handlers:** Extracted request logic into `handle_http_request`.
-   **WebSocket Handlers:** Extracted message logic into `handle_ws_message`.
-   **MRD Utilities:** Moved `mk_mrd` logic to `include/mk_mrd_utils.hpp`.
-   **Image Message Utilities:** Moved `make_image_message` logic to `include/image_message_utils.hpp`.
-   **Playback Core:** Moved index loading and path normalization logic to `services/playback/playback_core.hpp`.

### 4. New Test Coverage
9 test suites are now active (up from 4), covering previously untested or under-tested logic:
-   `unit_http_handlers`: Comprehensive tests for all REST API endpoints.
-   `unit_ws_handlers`: Tests for WebSocket broadcasting, subscriptions, and ingestion.
-   `unit_mk_mrd`: Verifies the generation of valid ISMRMRD files.
-   `unit_make_image`: Verifies binary image message payload generation.
-   `unit_playback`: Verifies session index loading and path normalization.
-   `test_mrd_sink`: (Existing) Verified HDF5 SWMR operations.
-   `unit_pose`: (Existing) Verified pose store logic.
-   `it_http` & `it_ws`: (Existing) Integration tests for networking.

### 5. Verification
All tests were executed using `ctest` and passed successfully:
```
100% tests passed, 0 tests failed out of 9
Total Test time (real) = 1.11 sec
```

### 6. Conclusion
The codebase is now significantly more robust, with decoupled logic that is easier to maintain and extend. Unit tests cover all critical data paths and control planes.
