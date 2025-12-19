# Roadmap: Improvements & Next Steps

This document outlines a task-based plan to further harden the `cwru_data_marshal` repository, building upon the recent testability refactoring.

## Task 1: Automate Quality Gate (CI/CD)
**Objective:** Ensure that the 100% test pass rate is maintained automatically on every code change.

- [ ] **1.1 Create GitHub Actions Workflow:** Add `.github/workflows/ci.yml`.
- [ ] **1.2 Environment Setup:** Configure the runner to install dependencies (`libhdf5-dev`, `libboost-all-dev`, `libopencv-dev`).
- [ ] **1.3 Build & Test Command:** Execute `cmake -B build -D BUILD_TESTING=ON && cmake --build build && ctest`.
- [ ] **1.4 Artifact Collection:** (Optional) Upload the `Testing/Temporary/LastTest.log` on failure for easier debugging.

## Task 2: Modernize Observability
**Objective:** Transition from raw console output to structured logging suitable for production environments.

- [ ] **2.1 Integrate `spdlog`:** Add `spdlog` as a `FetchContent` dependency in `CMakeLists.txt`.
- [ ] **2.2 Refactor `marshal_main.cpp`:** Replace `std::cout` with `spdlog::info` and `spdlog::error`.
- [ ] **2.3 Implement Log Levels:** Use `--verbose` or `--log-level` CLI flags to toggle between `DEBUG` and `INFO`.
- [ ] **2.4 Structured JSON Logging:** Configure a secondary logger to output JSON lines to a file for ingestion by log aggregators.

## Task 3: Python Client SDK
**Objective:** Provide a high-level Python interface to lower the barrier to entry for researchers.

- [ ] **3.1 Create `marshal_client` Package:** Start a `clients/python` directory with a `setup.py`.
- [ ] **3.2 HTTP Wrapper:** Implement a class to handle `POST /v1/mrd/ingest` and `GET /v1/mrd/latest` using the `requests` library.
- [ ] **3.3 WebSocket Subscriber:** Use `websockets` or `websocket-client` to provide an async iterator for real-time metadata/pose updates.
- [ ] **3.4 Usage Examples:** Port `tools/stream_image_series.py` to use this new SDK.

## Task 4: API Formalization
**Objective:** Standardize the API contract for third-party integrations.

- [ ] **4.1 OpenAPI Specification:** Create `docs/openapi.yaml` documenting all HTTP endpoints, request bodies, and JSON schemas.
- [ ] **4.2 Swagger UI Integration:** (Optional) Add a simple task/script to serve a local Swagger UI for live API exploration.
- [ ] **4.3 Versioning Strategy:** Ensure all endpoints remain under `/v1/` and plan for `/v2/` if breaking changes are needed.

## Task 5: Operational Hardening
**Objective:** Improve configuration and resilience for long-running deployments.

- [ ] **5.1 Configuration Files:** Support loading settings from a `.json` or `.yaml` file via a `--config` flag.
- [ ] **5.2 Graceful Shutdown:** Update `marshal_main.cpp` to catch `SIGINT` and `SIGTERM`, ensuring `H5Fclose` is called on all open sinks before exiting.
- [ ] **5.3 Health Check Enhancement:** Add more detail to `/health`, such as memory usage, active WebSocket client count, and disk space availability in `data_dir`.

## Task 6: Security & Encryption
**Objective:** Prepare the marshal for environments requiring data privacy (HIPAA compliance).

- [ ] **6.1 Reverse Proxy Documentation:** Add a guide on using Nginx or Traefik as an SSL terminator for the marshal.
- [ ] **6.2 Basic Authentication:** Implement optional token-based authentication for ingestion endpoints to prevent unauthorized data injection.

## Task 7: Client Optimizations (`viz_client`)
**Objective:** Clean up the redundant data fetching logic in the reference visualizer client.

- [ ] **7.1 Architecture Review:** The current `viz_client` runs both a WebSocket listener and a Polling thread simultaneously, causing redundant work (though functionally safe).
- [ ] **7.2 Implement Priority Mode:** Modify logic to prioritize WebSocket notifications. Only fall back to polling `latest.json` if no WS heartbeat/message is received for >2 seconds.
- [ ] **7.3 Exclusive Mode Flag:** Add a `--no-poll` or `--ws-only` CLI flag to disable the file-system watcher entirely for cleaner performance in network-stable environments.

## Task 8: Implement Missing Diagram Features
**Objective:** Bridge the functionality gaps between `DataFlow.drawio` and the codebase.

- [ ] **8.1 Biological Signals Ingest:**
    -   **Endpoint:** Implement `POST /v1/bio/signal`.
    -   **Schema:** Body must be JSON: `{"ts": <iso8601>, "source": "ecg"|"resp", "data": [float, float, ...], "rate_hz": <float>}`.
    -   **Persistence:** Append to `data/mrd/bio.jsonl`.
    -   **Broadcast:** Emit WS message `{"type": "bio", ...}`. Ensure this is only sent to the `bio` topic.

- [ ] **8.2 Robust Topic Routing (Fix Leaks):**
    -   **Current Flaw:** `marshal_ws.hpp` sends all broadcasts to clients with empty/default topics.
    -   **Refactor:** Change `Session::topic` default from `""` to `"_system_"`.
    -   **Strict Logic:** `broadcast_to(msg, "pose")` must iterate clients and send *only* if `client.topic == "pose"`.
    -   **Feature:** Implement `{"unsubscribe": "topic"}` handling in `handle_ws_message`.

- [ ] **8.3 API Standardization (Clean Break):**
    -   **Rename:** Change the primary route registration in `marshal_http.hpp` from `/v1/ismrmrd/frame` to `/v1/mrd/frame`.
    -   **Remove Legacy:** Do **not** keep an alias. Remove the old route handling logic entirely to avoid technical debt.
    -   **Update Clients:** Update `tools/stream_image_series.py` and `tools/make_image_message.cpp` to use the new route.
    -   **Update Tests:** Rename tests in `unit_http_handlers` and `it_http` to verify `/v1/mrd/frame`.

- [ ] **8.4 Module Interface Verification:**
    -   **Mock Surface Tracking:** Create `clients/mocks/surface_tracker.py`. It must subscribe to `bio` and `pose` topics via WS and print latency stats (time received - time sent).
    -   **Mock Planning:** Create `clients/mocks/planner.py`. It must subscribe to `mrd` and verify that `frame_index` increases monotonically without gaps.

- [ ] **8.5 Inter-Marshal Bridge (The "Coordinator"):**
    -   **Concept:** A dedicated client acting as the "brain" connecting the "nervous system" (Robot Marshal) and "eyes" (MRI Marshal). Since the marshals are architecturally decoupled, this bridge enforces system-level logic.
    -   **Artifact:** Create `clients/bridge/coordinator.py`.
    -   **Requirement 1 (Safety Stop):** Listen to MRI Marshal (WS) for "Scanner Stopped/Error" events. Immediately send `POST /write/robot_status` `{"state": "HALT"}` to Robot Marshal.
    -   **Requirement 2 (Scan Sync):** Listen to Robot Marshal (Polling/GET) for "Robot at Isocenter". Trigger "Start Scan" on MRI (mock trigger or API call).
    -   **Requirement 3 (Data Tagging):** Listen to Robot Marshal for tool ID changes. Forward this to MRI Marshal to update session metadata/logs.
    -   **Architecture Note:** This must be an external script, keeping the core C++ servers decoupled.

## Task 9: Security & Reliability Fixes
**Objective:** Address high-priority bugs and security gaps identified during deep inspection.

- [ ] **9.1 Fix Race Condition in Dumpbox:**
    -   **Issue:** `resolve_sink_paths` lazy-initializes `state.dumpbox_session` without a mutex. Simultaneous ingests can cause split sessions.
    -   **Fix:** Add `std::mutex` protection around session initialization in `mrd_io.hpp`.
- [ ] **9.2 Fix Swallowed Exceptions:**
    -   **Issue:** `POST /v1/pose/update` and `ws_emit` catch `...` without logging, causing silent data loss.
    -   **Fix:** Log all caught exceptions to `std::cerr` (or `spdlog` if Task 2 is done).
- [ ] **9.3 Configurable Body Limit:**
    -   **Issue:** `kMaxHttpBodyBytes` is hardcoded to 128MB.
    -   **Fix:** Add `--max-body-size` CLI flag to support large 3D volume uploads.
- [ ] **9.4 Input Sanitization:**
    -   **Issue:** `dumpbox_session` CLI argument allows path traversal (`../`).
    -   **Fix:** Sanitize the session string to ensure it contains no slashes or parent directories.
