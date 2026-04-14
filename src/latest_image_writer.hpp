/*
 * File: src/latest_image_writer.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Coalesced async writer for the live latest-image H5 file.
 */

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <cstdint>
#include <vector>

namespace mrd {

void write_latest_image_h5_file(const std::filesystem::path& dest,
                                const std::string& xml,
                                const std::vector<std::vector<uint8_t>>& images);

class LatestImageWriter {
public:
    using Completion = std::function<void(const std::filesystem::path&)>;

    LatestImageWriter();
    ~LatestImageWriter();

    LatestImageWriter(const LatestImageWriter&) = delete;
    LatestImageWriter& operator=(const LatestImageWriter&) = delete;

    void enqueue(std::filesystem::path dest,
                 std::string xml,
                 std::vector<std::vector<uint8_t>> images,
                 Completion completion);

private:
    struct Job;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mrd
