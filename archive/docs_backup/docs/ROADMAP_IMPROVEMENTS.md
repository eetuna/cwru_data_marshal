# Roadmap: Improvements & Next Steps

This document outlines a task-based plan to further harden the `cwru_data_marshal` repository, building upon the recent testability refactoring.

## Task 1: Automate Quality Gate (CI/CD)
**[PRIORITY: HIGH] [COMPLEXITY: EASY]**  
**Objective:** Ensure that the 100% test pass rate is maintained automatically on every code change.

- [ ] **1.1 Create GitHub Actions Workflow:** Add `.github/workflows/ci.yml`.
- [ ] **1.2 Environment Setup:** Configure the runner to install dependencies (`libhdf5-dev`, `libboost-all-dev`, `libopencv-dev`).
- [ ] **1.3 Build & Test Command:** Execute `cmake -B build -D BUILD_TESTING=ON && cmake --build build && cd build && ctest`.
- [ ] **1.4 Artifact Collection:** (Optional) Upload the `Testing/Temporary/LastTest.log` on failure for easier debugging.

## Task 2: Modernize Observability
**[PRIORITY: MED] [COMPLEXITY: EASY]**  
**Objective:** Transition from raw console output to structured logging suitable for production environments.

- [ ] **2.1 Integrate `spdlog`:** Add `spdlog` as a `FetchContent` dependency in `CMakeLists.txt`.
- [ ] **2.2 Refactor `marshal_main.cpp`:** Replace `std::cout` with `spdlog::info` and `spdlog::error`.
- [ ] **2.3 Implement Log Levels:** Use `--verbose` or `--log-level` CLI flags to toggle between `DEBUG` and `INFO`.
- [ ] **2.4 Structured JSON Logging:** Configure a secondary logger to output JSON lines to a file for ingestion by log aggregators.

## Task 3: Python Client SDK
**[PRIORITY: LOW] [COMPLEXITY: MED]**  
**Objective:** Provide a high-level Python interface to lower the barrier to entry for researchers.

- [ ] **3.1 Create `marshal_client` Package:** Start a `clients/python` directory with a `setup.py`.
- [ ] **3.2 HTTP Wrapper:** Implement a class to handle `POST /v1/mrd/ingest` and `GET /v1/mrd/latest` using the `requests` library.
- [ ] **3.3 WebSocket Subscriber:** Use `websockets` or `websocket-client` to provide an async iterator for real-time metadata/pose updates.
- [ ] **3.4 Usage Examples:** Port `tools/stream_image_series.py` to use this new SDK.

## Task 4: API Formalization (COMPLETED)
**[PRIORITY: HIGH] [COMPLEXITY: MED]**  
**Objective:** Standardize the API contract for third-party integrations.

- [x] **4.1 Response Envelope:** All successful responses now follow the `{"status": "ok", "data": {...}}` pattern.
- [x] **4.2 Standardized Errors:** All errors now return `{"status": "error", "error": "..."}` with correct HTTP codes.
- [x] **4.3 OpenAPI Specification:** Formally documented all schemas in `docs/technical/openapi.yaml`.

## Task 5: Operational Hardening
**[PRIORITY: MED] [COMPLEXITY: EASY]**  
**Objective:** Improve configuration and resilience for long-running deployments.

- [ ] **5.1 Configuration Files:** Support loading settings from a `.json` or `.yaml` file via a `--config` flag.
- [x] **5.2 Configurable Limits:** (Task 9.3) Added `--max-body-size` flag.
- [ ] **5.3 Health Check Enhancement:** Add more detail to `/health`, such as memory usage and active client counts.

## Task 6: Security & Encryption
**[PRIORITY: HIGH] [COMPLEXITY: MED]**  
**Objective:** Prepare the marshal for environments requiring data privacy (HIPAA compliance).

- [ ] **6.1 Reverse Proxy Documentation:** Add a guide on using Nginx or Traefik as an SSL terminator for the marshal.
- [ ] **6.2 Basic Authentication:** Implement optional token-based authentication for ingestion endpoints.

## Task 7: Client Optimizations (`viz_client`)
**[PRIORITY: LOW] [COMPLEXITY: EASY]**  
**Objective:** Clean up the redundant data fetching logic in the reference visualizer client.

- [ ] **7.1 Architecture Review:** The current `viz_client` runs both a WebSocket listener and a Polling thread simultaneously.
- [ ] **7.2 Implement Priority Mode:** Modify logic to prioritize WebSocket notifications.
- [ ] **7.3 Exclusive Mode Flag:** Add a `--no-poll` or `--ws-only` CLI flag.

## Task 8: Implement Missing Diagram Features (COMPLETED)
**[PRIORITY: HIGH] [COMPLEXITY: HARD]**  
**Objective:** Bridge the functionality gaps between `DataFlow.drawio` and the codebase.

- [x] **8.1 Biological Signals Ingest:** Implemented `POST /v1/bio/signal` with `bio.jsonl` persistence and topic-based WS broadcast.
- [x] **8.2 Robust Topic Routing:** Refactored `marshal_ws.hpp` to strictly enforce subscriptions and remove the default broadcast leak.
- [x] **8.3 API Standardization:** Renamed `/v1/ismrmrd/frame` to `/v1/mrd/frame` and updated all clients/tools.
- [x] **8.4 Module Interface Verification:** Created mock tracker and planner clients in `clients/mocks/`.
- [x] **8.5 Inter-Marshal Bridge:** Created `coordinator.py` for safety-critical synchronization between MRI and Robot marshals.

## Task 9: Security & Reliability Fixes (COMPLETED)
**[PRIORITY: HIGH] [COMPLEXITY: EASY]**  
**Objective:** Address high-priority bugs and security gaps identified during deep inspection.

- [x] **9.1 Fix Race Condition in Dumpbox:** Added `session_mtx` to protect lazy session initialization.
- [x] **9.2 Fix Swallowed Exceptions:** Standardized `try/catch` logic to log errors to `stderr`.
- [x] **9.3 Configurable Body Limit:** Added `--max-body-size` CLI flag.
- [x] **9.4 Input Sanitization:** Implemented `sanitize_session` to prevent path traversal.

## Task 10: High-Availability & Maintenance (PLANNED)
**[PRIORITY: MED] [COMPLEXITY: HARD]**  
**Objective:** Prepare the system for 24/7 mission-critical clinical deployments.

- [ ] **10.1 Graceful Shutdown:** Implement a signal handler for `SIGINT/SIGTERM` to explicitly close all HDF5 file handles before exiting.
- [ ] **10.2 Automatic Handle Cleanup:** Implement an idle-timeout for `MrdSink` streams.
- [ ] **10.3 Structured Logging:** Replace `std::cerr` with `spdlog` for color-coded, timestamped log levels.
- [ ] **10.4 Disk Space Watchdog:** Monitor `data_dir` capacity and broadcast faults if storage is low.
- [ ] **10.5 Log Rotation:** Implement automatic rotation for `.jsonl` files.

---

## Completed Verification Tools
The following scripts have been added to ensure ongoing system quality:
- `scripts/benchmarks/chaos_test.sh`: Stress test for concurrency and limits.
- `scripts/tools/verify_system_integration.sh`: Functional test for Bio signals and the Bridge.
- `scripts/run_all_tests.sh`: Master suite executing all checks (Unit, Integration, Stress).
