/*
 * File: src/latest_image_writer.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: The viewer's copy of the current scan's images.
 *
 * Holds the newest image for each slice position (position + slice
 * orientation + image type, straight from the image header) and keeps
 * latest_image.h5 equal to that set. Nothing is inferred about volumes or
 * slice counts: a new image at a position replaces the previous one there,
 * a new scan clears the set.
 *
 * One background thread writes the file whenever the set changed. The file
 * mirrors a state, not a stream of events, so a write always takes the set
 * as it is at that moment and nothing can be lost or reordered.
 */

#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace mrd {

class LatestSnapshot {
public:
    explicit LatestSnapshot(std::filesystem::path dir);
    ~LatestSnapshot();

    LatestSnapshot(const LatestSnapshot&) = delete;
    LatestSnapshot& operator=(const LatestSnapshot&) = delete;

    // New scan: drop the previous scan's images.
    void begin_scan(std::string xml_header);
    // One IMAGE wire body (header + attr_len + attributes + pixels).
    void add_image(const uint8_t* body, size_t size);

    struct View {
        std::string path;        // empty until the current scan's first write
        uint64_t generation{0};  // bumps on every change the file reflects
        size_t images{0};        // images in the file at `path`
    };
    View view() const;

    struct Perf {
        uint64_t writes{0};
        uint64_t failed{0};
        uint64_t last_write_us{0};
        uint64_t max_write_us{0};
    };
    Perf perf() const;

private:
    // position (0.01 mm), slice normal (0.001), image_type
    using Key = std::tuple<std::array<int64_t, 3>, std::array<int32_t, 3>, uint16_t>;

    void run();

    const std::filesystem::path dest_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::map<Key, std::vector<uint8_t>> images_;
    std::string xml_;
    uint64_t scan_{0};       // bumps per begin_scan; a write for an old scan is discarded
    bool dirty_{false};
    bool stop_{false};
    View view_;
    Perf perf_;
    std::thread worker_;
};

// Write `images` (IMAGE wire bodies) to `dest` atomically (tmp + rename) as
// a canonical ISMRMRD file. Images are grouped by size: image_0, image_1, ...
void write_latest_image_h5_file(const std::filesystem::path& dest,
                                const std::string& xml,
                                const std::vector<std::vector<uint8_t>>& images);

} // namespace mrd
