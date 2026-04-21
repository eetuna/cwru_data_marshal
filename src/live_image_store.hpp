/*
 * File: src/live_image_store.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Shared helpers for live per-scan image history and closed snapshots.
 */

#pragma once

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <ismrmrd/ismrmrd.h>

#include "logging.hpp"
#include "marshal_state.hpp"
#include "mrd_io.hpp"
#include "mrd_stream_tags.hpp"

namespace mrd {

enum class LiveLane {
    Scanner,
    Recon,
};

inline LiveImageLaneState& lane_state(MarshalState& state, LiveLane lane)
{
    return lane == LiveLane::Scanner ? state.scanner_live : state.recon_live;
}

inline const char* lane_name(LiveLane lane)
{
    return lane == LiveLane::Scanner ? "scanner" : "reconstruction";
}

inline std::filesystem::path lane_live_dir(const std::filesystem::path& dump_dir, LiveLane lane)
{
    return lane == LiveLane::Scanner ? live_scanner_dir(dump_dir) : live_recon_dir(dump_dir);
}

inline std::filesystem::path lane_latest_path(const std::filesystem::path& dump_dir, LiveLane lane)
{
    return lane_live_dir(dump_dir, lane) / "latest_image.h5";
}

struct ParsedWireImage {
    ISMRMRD::ImageHeader header{};
    const char* attr{nullptr};
    size_t attr_len{0};
    const uint8_t* pixels{nullptr};
    size_t pixel_bytes{0};
};

inline bool parse_wire_image(const uint8_t* data, size_t size, ParsedWireImage& out)
{
    if (size < IMAGE_HEADER_BYTES + sizeof(uint64_t)) return false;
    std::memcpy(&out.header, data, sizeof(ISMRMRD::ImageHeader));
    uint64_t attr_len = 0;
    std::memcpy(&attr_len, data + IMAGE_HEADER_BYTES, sizeof(uint64_t));
    const size_t attr_off = IMAGE_HEADER_BYTES + sizeof(uint64_t);
    // MEDIUM #13: `attr_off + attr_len` could wrap on malicious input. Use
    // the overflow-safe form: size must be >= attr_off, and attr_len must
    // not exceed the remaining bytes.
    if (size < attr_off) return false;
    if (attr_len > size - attr_off) return false;
    const size_t attr_len_s = static_cast<size_t>(attr_len);
    out.attr = reinterpret_cast<const char*>(data + attr_off);
    out.attr_len = attr_len_s;
    out.pixels = data + attr_off + attr_len_s;
    out.pixel_bytes = size - attr_off - attr_len_s;
    return true;
}

inline void publish_latest_snapshot(MarshalState& state,
                                    LiveLane lane,
                                    std::string xml,
                                    std::vector<std::vector<uint8_t>> images)
{
    if (images.empty()) return;

    auto dest = lane_latest_path(state.dump_dir, lane);
    const auto generation = state.latest_image_generation.load();
    auto on_complete = [&state, generation](const std::filesystem::path& path) {
        if (state.latest_image_generation.load() != generation) return;
        std::lock_guard<std::mutex> img_lk(state.latest_image_mtx);
        state.latest_image_path = path.string();
        state.latest_image_error = false;
    };

    if (state.latest_writer) {
        state.latest_writer->enqueue(dest, std::move(xml), std::move(images), std::move(on_complete));
        return;
    }

    try {
        write_latest_image_h5_file(dest, xml, images);
        on_complete(dest);
    } catch (const std::exception& e) {
        LOG_WARN("Latest H5 write failed for " << lane_name(lane) << ": " << e.what());
    }
}

inline bool recon_group_is_complete(const MarshalState& state,
                                    const ReconLatestGroupState& group)
{
    // HIGH #9: gate on header_received. If a recon image somehow arrives
    // before the XML header has been parsed (reconnect, malformed stream),
    // recon_expected_slices is still its default 0 and the old `<= 1` check
    // would publish a single slice as a "complete" multislice result.
    if (!state.header_received.load(std::memory_order_acquire)) {
        // No header yet: we cannot know the expected slice count. Do not
        // publish prematurely; treat as incomplete so the group buffers.
        return false;
    }
    if (state.recon_expected_slices <= 1) return true;
    return group.seen_slices.size() >= state.recon_expected_slices;
}

inline void append_live_image(MarshalState& state, LiveLane lane, const uint8_t* data, size_t size)
{
    ParsedWireImage parsed;
    if (!parse_wire_image(data, size, parsed)) return;

    std::string xml;
    std::string filename;
    std::vector<uint8_t> image_bytes(data, data + size);
    std::vector<std::vector<uint8_t>> publish_images;

    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        xml = state.current_xml_header;

        if (state.current_scan_filename.empty()) {
            state.current_scan_filename = scan_filename();
        }
        filename = state.current_scan_filename;

        auto& lane_store = lane_state(state, lane);
        if (!lane_store.recorder) {
            lane_store.recorder = std::make_unique<LiveImageRecorder>(
                lane_live_dir(state.dump_dir, lane));
        }

        lane_store.recorder->append_image(filename, xml, image_bytes);

        if (lane != LiveLane::Recon) {
            publish_images.push_back(std::move(image_bytes));
        } else if (parsed.header.matrix_size[2] > 1) {
            state.recon_latest_group.reset();
            publish_images.push_back(std::move(image_bytes));
        } else {
            auto& recon_group = state.recon_latest_group;
            if (recon_group.active
                && recon_group.image_series_index != parsed.header.image_series_index) {
                if (!recon_group.published && !recon_group.images.empty()) {
                    publish_images = recon_group.images;
                }
                recon_group.reset();
            }

            if (!recon_group.active) {
                recon_group.reset();
                recon_group.active = true;
                recon_group.image_series_index = parsed.header.image_series_index;
            }

            recon_group.images.push_back(std::move(image_bytes));
            if (std::find(recon_group.seen_slices.begin(),
                          recon_group.seen_slices.end(),
                          parsed.header.slice) == recon_group.seen_slices.end()) {
                recon_group.seen_slices.push_back(parsed.header.slice);
            }

            if (recon_group_is_complete(state, recon_group)) {
                publish_images = recon_group.images;
                recon_group.published = true;
            }
        }
    }

    if (!publish_images.empty()) {
        publish_latest_snapshot(state, lane, std::move(xml), std::move(publish_images));
    }
}

inline void append_live_waveform(MarshalState& state, LiveLane lane,
                                 const uint8_t* data, size_t size)
{
    if (size < WAVEFORM_HEADER_BYTES) return;

    std::string xml;
    std::string filename;
    std::vector<uint8_t> wf_bytes(data, data + size);

    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        xml = state.current_xml_header;

        if (state.current_scan_filename.empty()) {
            state.current_scan_filename = scan_filename();
        }
        filename = state.current_scan_filename;

        auto& lane_store = lane_state(state, lane);
        if (!lane_store.recorder) {
            lane_store.recorder = std::make_unique<LiveImageRecorder>(
                lane_live_dir(state.dump_dir, lane));
        }

        lane_store.recorder->append_waveform(filename, xml, std::move(wf_bytes));
    }
}

inline void flush_live_lane(MarshalState& state, LiveLane lane)
{
    std::lock_guard<std::mutex> lk(state.scan_mtx);
    auto& lane_store = lane_state(state, lane);
    if (lane_store.recorder) {
        lane_store.recorder->close_scan();
    }
    if (lane == LiveLane::Recon) {
        state.recon_latest_group.reset();
    }
}

inline void flush_all_live_lanes(MarshalState& state)
{
    flush_live_lane(state, LiveLane::Scanner);
    flush_live_lane(state, LiveLane::Recon);
}

inline void mark_lane_finalized_after_eof(MarshalState& state, LiveLane lane)
{
    std::lock_guard<std::mutex> lk(state.scan_mtx);
    if (lane == LiveLane::Scanner) {
        state.scanner_lane_finalized = true;
    } else {
        state.recon_lane_finalized = true;
        state.recon_latest_group.reset();
    }

    if (state.scanner_lane_finalized && state.recon_lane_finalized) {
        state.current_xml_header.clear();
        state.current_config.clear();
        state.current_scan_filename.clear();
        state.recon_expected_slices = 0;
        state.header_received.store(false);
        state.config_received.store(false);
        state.recon_failure_reported.store(false);
        state.scanner_lane_finalized = false;
        state.recon_lane_finalized = false;
    }
}

inline void reset_live_outputs_for_new_scan(MarshalState& state)
{
    state.latest_image_generation.fetch_add(1);

    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        state.scanner_live.close();
        state.recon_live.close();
        state.scanner_lane_finalized = false;
        state.recon_lane_finalized = false;
        state.recon_failure_reported.store(false);
        state.recon_latest_group.reset();
    }

    std::error_code ec;
    std::filesystem::remove(lane_latest_path(state.dump_dir, LiveLane::Scanner), ec);
    ec.clear();
    std::filesystem::remove(lane_latest_path(state.dump_dir, LiveLane::Recon), ec);
    ec.clear();
    std::filesystem::remove(live_recon_dir(state.dump_dir) / "latest_error.png", ec);

    std::lock_guard<std::mutex> img_lk(state.latest_image_mtx);
    state.latest_image_path.clear();
    state.latest_image_error = false;
}

} // namespace mrd
