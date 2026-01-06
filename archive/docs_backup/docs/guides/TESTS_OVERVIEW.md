# Test Suite Overview

This project uses [Catch2](https://github.com/catchorg/Catch2) unit tests wired into the
CMake build. The tests exercise the HTTP ingestion pipeline, the MRD sink, pose
state handling, and the WebSocket broadcast helpers. This page summarizes the
coverage and shows how to run each group locally.

## Running the entire suite

1. Configure the build directory with testing enabled:
   ```bash
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
   ```
2. Compile the marshal and test binaries:
   ```bash
   cmake --build build -j"$(nproc)"
   ```
3. Execute all Catch2 tests through CTest:
   ```bash
   ctest --test-dir build
   ```

CTest forwards its output directly from Catch2, so individual assertion
failures point at the corresponding source line. Append `-V` for verbose
logging or `-R <pattern>` to run only a subset of cases.

## Focused runs

Because each executable bundles all Catch2 cases, targeted invocations go
through CTest filters. Examples:

```bash
# Only MRD sink coverage
ctest --test-dir build -R test_mrd_sink

# Only HTTP endpoint parser limit regression
ctest --test-dir build -R test_http_endpoints
```

## Test modules

### `tests/test_mrd_sink.cpp`
* Confirms SWMR readers see appended frames immediately after writes.
* Validates complex64 and int16 payloads to ensure type handling stays
  lossless.
* Verifies the geometry-aware rollover by pushing frames whose `nx`, `ny`, or
  slice counts change mid-stream and asserting the sink stamps the new shape
  into a fresh filename.
* Exercises the adaptive chunk-sizing algorithm so chunk extents shrink when a
  proposed chunk would exceed the 8 MiB budget.

### `tests/test_http_endpoints.cpp`
* Reproduces Boost.Beast’s default 1 MiB parser ceiling and asserts the server
  now raises the allowance to `kMaxHttpBodyBytes` (128 MiB) so 2 MiB payloads
  parse successfully.

### `tests/test_pose_store.cpp`
* Round-trips pose updates through the in-memory pose store to guarantee
  `POST /v1/pose/update` and `GET /v1/pose/current` share the same serialization
  logic.

### `tests/test_ws_broadcast.cpp`
* Smoke-tests the WebSocket broadcast helper; it currently asserts the helper
  compiles and links, providing a placeholder for deeper coverage as more WS
  features are added.

Refer back to this guide whenever you need to confirm test coverage or explain
how to run specific checks to a teammate.
