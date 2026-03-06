# Quick Reference - SWMR Implementation Changes

## Files to Modify

### 1. `src/marshal_http.hpp`

**Change GET /v1/mrd/frame (around line 491):**
```cpp
// REPLACE the binary return with:
return make_response(http::status::ok, {
    {"path", mrd_path},
    {"frame_index", result.frame_index},
    {"total_frames", result.total_frames},
    {"dims", {
        {"x", result.dims.spatial[0]},
        {"y", result.dims.spatial[1]},
        {"z", result.dims.spatial[2]}
    }},
    {"channels", result.dims.channels},
    {"datatype", mrd::element_type_to_string(result.element_type)}
});
```

**Change GET /v1/mrd/ingest (around line 561):**
```cpp
// REPLACE the file download with:
auto file_size = fs::file_size(mrd_path);
std::string filename = fs::path(mrd_path).filename().string();

return make_response(http::status::ok, {
    {"path", mrd_path},
    {"filename", filename},
    {"size_bytes", file_size}
});
```

### 2. `src/marshal_http_archive.hpp` (NEW FILE)
See HANDOVER_SWMR_IMPLEMENTATION.md for full content.

### 3. `clients/viz_client/viz_client_main.cpp`
```bash
cp viz_client_main.cpp viz_client_main_http.cpp
cp viz_client_main_hdf5.cpp viz_client_main.cpp
```

### 4. `CMakeLists.txt` (around line 84-91)
Add to viz_client linking:
```cmake
nlohmann_json::nlohmann_json ${hdf5_target}
```

## Build & Test
```bash
mkdir -p build && cd build
cmake .. -DBUILD_TESTING=ON
cmake --build . --parallel
ctest --output-on-failure  # Should pass 9/9 tests
```

## Docker Commands
All with `--env-file .env.demo`:
```bash
# Terminal 1
docker compose -f docker-compose.demo.yml --env-file .env.demo up mri-marshal

# Terminal 2
docker compose -f docker-compose.demo.yml --env-file .env.demo up robot-marshal

# Terminal 3
docker compose -f docker-compose.demo.yml --env-file .env.demo up image-streamer

# Terminal 4
docker compose -f docker-compose.demo.yml --env-file .env.demo up pose-client

# Terminal 5
docker compose -f docker-compose.demo.yml --env-file .env.demo up ecg-client

# Terminal 6
docker compose -f docker-compose.demo.yml --env-file .env.demo up viz-client
```

## Key Changes Summary
- GET /v1/mrd/frame → returns JSON metadata (not binary)
- GET /v1/mrd/ingest → returns JSON metadata (not file)
- viz_client → uses direct HDF5 SWMR reads (not HTTP GET binary)
- Result: 2-3x faster, simpler architecture

See HANDOVER_SWMR_IMPLEMENTATION.md for complete details.
