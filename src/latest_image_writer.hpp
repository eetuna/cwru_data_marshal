/*
 * File: src/latest_image_writer.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Per-scan async appender for the live ISMRMRD H5 file.
 *
 * One instance per live side (scanner, recon). On scan start, open_scan()
 * creates the file. Each image is appended off the calling thread via
 * append_image(). On scan end, close_scan() flushes and closes.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mrd {

class LatestImageWriter {
public:
    using AppendCompletion = std::function<void(const std::filesystem::path&)>;

    LatestImageWriter();
    ~LatestImageWriter();

    LatestImageWriter(const LatestImageWriter&) = delete;
    LatestImageWriter& operator=(const LatestImageWriter&) = delete;

    // Open a new scan file. Blocks on the worker to ensure file exists before
    // any append_image. Any previous scan file is closed first.
    void open_scan(std::filesystem::path dest, std::string xml);

    // Enqueue an appendImage onto the worker. `on_complete` runs after the
    // append lands; receives the scan file path.
    void append_image(std::vector<uint8_t> body, AppendCompletion on_complete);

    // Close the current scan file (sync, waits for pending appends).
    void close_scan();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mrd
