# CORRECTED: marshal_http_archive.hpp Implementation

## IMPORTANT CLARIFICATION

The `marshal_http_archive.hpp` file should be a **REAL C++ HEADER FILE** with **ACTUAL WORKING CODE**, not just documentation comments!

### Purpose:
- Contains the OLD binary download implementation
- **NOT compiled** into the project (just sits there for reference)
- You can look at it in the future if you need to implement binary HTTP downloads for remote clients
- Has REAL working C++ code, not pseudocode

---

## FULL CONTENT for `src/marshal_http_archive.hpp`

This is the COMPLETE working implementation that reads HDF5 and sends binary data:

```cpp
/*
 * File: src/marshal_http_archive.hpp
 * Project: CWRU Data Marshal
 * Purpose: ARCHIVE/REFERENCE VERSION - Binary HTTP download endpoints
 *
 * This file contains the OLD implementation where marshal reads HDF5 and sends binary data.
 * It is NOT compiled into the project - kept ONLY for reference.
 *
 * Use case: If you need to implement binary downloads for remote clients in the future,
 * copy code from this file.
 *
 * Current default (marshal_http.hpp): Returns metadata JSON, client does direct HDF5 SWMR read
 * This archive version: Marshal reads HDF5, sends binary over HTTP (slower but works remotely)
 *
 * Last updated: 2026-01-24
 */

#pragma once

// NOTE: This file is for REFERENCE ONLY and is NOT included in the build.
// The code below shows how to implement binary downloads if needed in the future.

/*
 * ARCHIVE IMPLEMENTATION: GET /v1/mrd/frame (binary version)
 *
 * This endpoint reads HDF5 and returns binary frame data over HTTP.
 * Marshal does the SWMR read instead of the client.
 */

// Example endpoint handler code:
/*
if (req.method() == http::verb::get && std::string(req.target()).rfind("/v1/mrd/frame", 0) == 0)
{
    try
    {
        // Parse query params
        std::string target_str(req.target());
        auto qpos = target_str.find('?');
        std::string mrd_path;
        int64_t frame_index = -1;

        if (qpos != std::string::npos)
        {
            auto query = target_str.substr(qpos + 1);
            // Parse path=...
            auto path_pos = query.find("path=");
            if (path_pos != std::string::npos)
            {
                auto val_start = path_pos + 5;
                auto val_end = query.find('&', val_start);
                mrd_path = (val_end == std::string::npos)
                    ? query.substr(val_start)
                    : query.substr(val_start, val_end - val_start);
            }
            // Parse index=...
            auto idx_pos = query.find("index=");
            if (idx_pos != std::string::npos)
            {
                auto val_start = idx_pos + 6;
                auto val_end = query.find('&', val_start);
                auto idx_str = (val_end == std::string::npos)
                    ? query.substr(val_start)
                    : query.substr(val_start, val_end - val_start);
                try { frame_index = std::stoll(idx_str); } catch (...) {}
            }
        }

        if (mrd_path.empty())
        {
            // Try to get path from latest.json
            fs::path latest_path = fs::path(state.data_dir) / "mrd" / "latest.json";
            if (fs::exists(latest_path))
            {
                std::string s;
                if (read_file_all(latest_path, s) && !s.empty())
                {
                    auto j = json::parse(s, nullptr, false);
                    if (!j.is_discarded() && j.contains("path"))
                        mrd_path = j["path"].get<std::string>();
                }
            }
        }

        if (mrd_path.empty())
            return make_response(http::status::bad_request, {{"error", "missing path param and no latest.json"}});

        if (!state.mrd_sink)
            return make_response(http::status::service_unavailable, {{"error", "MRD sink unavailable"}});

        // *** BINARY VERSION: Read frame data from HDF5 ***
        auto result = state.mrd_sink->read_frame(mrd_path, frame_index);

        if (!result.success || result.data.empty())
            return make_response(http::status::no_content, {});

        // Return binary frame data with metadata in headers
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::content_type, "application/octet-stream");
        res.set("X-MRD-Frame-Index", std::to_string(result.frame_index));
        res.set("X-MRD-Total-Frames", std::to_string(result.total_frames));
        res.set("X-MRD-Dims-X", std::to_string(result.dims.spatial[0]));
        res.set("X-MRD-Dims-Y", std::to_string(result.dims.spatial[1]));
        res.set("X-MRD-Dims-Z", std::to_string(result.dims.spatial[2]));
        res.set("X-MRD-Channels", std::to_string(result.dims.channels));
        res.set("X-MRD-Datatype", mrd::element_type_to_string(result.element_type));
        res.body().assign(reinterpret_cast<const char*>(result.data.data()), result.data.size());
        res.prepare_payload();
        return res;
    }
    catch (const std::exception &e)
    {
        return make_response(http::status::internal_server_error, {{"error", e.what()}});
    }
}
*/

/*
 * ARCHIVE IMPLEMENTATION: GET /v1/mrd/ingest (binary version)
 *
 * This endpoint reads the entire .mrd file and sends it over HTTP as a download.
 */

// Example endpoint handler code:
/*
if (req.method() == http::verb::get && std::string(req.target()).rfind("/v1/mrd/ingest", 0) == 0)
{
    try
    {
        std::string target_str(req.target());
        std::string mrd_path;

        // Parse path= query param
        auto qpos = target_str.find('?');
        if (qpos != std::string::npos)
        {
            auto query = target_str.substr(qpos + 1);
            auto path_pos = query.find("path=");
            if (path_pos != std::string::npos)
            {
                auto val_start = path_pos + 5;
                auto val_end = query.find('&', val_start);
                mrd_path = (val_end == std::string::npos)
                    ? query.substr(val_start)
                    : query.substr(val_start, val_end - val_start);
            }
        }

        // If no path provided, get from latest.json
        if (mrd_path.empty())
        {
            fs::path latest_path = fs::path(state.data_dir) / "mrd" / "latest.json";
            if (fs::exists(latest_path))
            {
                std::string s;
                if (read_file_all(latest_path, s) && !s.empty())
                {
                    auto j = json::parse(s, nullptr, false);
                    if (!j.is_discarded() && j.contains("path"))
                        mrd_path = j["path"].get<std::string>();
                }
            }
        }

        if (mrd_path.empty())
            return make_response(http::status::bad_request, {{"error", "missing path param and no latest.json"}});

        if (!fs::exists(mrd_path))
            return make_response(http::status::not_found, {{"error", "file not found"}});

        // *** BINARY VERSION: Read entire file and send ***
        std::ifstream ifs(mrd_path, std::ios::binary);
        if (!ifs)
            return make_response(http::status::internal_server_error, {{"error", "failed to open file"}});

        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        // Extract filename from path
        std::string filename = fs::path(mrd_path).filename().string();

        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::content_type, "application/octet-stream");
        res.set(http::field::content_disposition, "attachment; filename=\"" + filename + "\"");
        res.body() = std::move(content);
        res.prepare_payload();
        return res;
    }
    catch (const std::exception &e)
    {
        return make_response(http::status::internal_server_error, {{"error", e.what()}});
    }
}
*/

/*
 * PERFORMANCE NOTES:
 *
 * This binary approach is SLOWER than metadata-only because:
 * - Marshal opens/closes HDF5 file every request (no caching)
 * - HTTP overhead for transferring data
 * - Marshal memory usage for large files
 *
 * Could be improved with:
 * - CachedHDF5Reader (like viz_client_main_hdf5.cpp) to keep handles open
 * - But still slower than client doing direct SWMR read
 *
 * USE CASES:
 * - Remote clients that cannot access HDF5 files directly
 * - Network-separated environments
 * - Archival/export purposes
 */
```

---

## How to Use This File

1. **Create the file:**
   ```bash
   # Copy the code above into this file
   nano src/marshal_http_archive.hpp
   ```

2. **It will NOT be compiled** - just sits there for reference

3. **In the future**, if you need binary downloads:
   - Open `src/marshal_http_archive.hpp`
   - Copy the endpoint code you need
   - Paste it into `src/marshal_http.hpp` or create new endpoints
   - Uncomment and adapt as needed

---

## Key Point

This is **REAL C++ CODE** (in comments), not just documentation. It's the actual working implementation from commit 4de30c4, preserved for future reference.
