/*
 * File: src/latest_image_writer.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: The viewer's copy of the current scan's images (see header).
 */

#include "latest_image_writer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <ismrmrd/ismrmrd.h>

#include "logging.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"

#undef LOG_COMPONENT
#define LOG_COMPONENT "latest_image"

namespace mrd {
namespace {

struct WireImage {
    ISMRMRD::ImageHeader header{};
    const char* attr{nullptr};
    size_t attr_len{0};
    const uint8_t* pixels{nullptr};
    size_t pixel_bytes{0};
};

bool parse(const uint8_t* data, size_t size, WireImage& out)
{
    if (size < IMAGE_HEADER_BYTES + sizeof(uint64_t)) return false;
    std::memcpy(&out.header, data, sizeof(out.header));
    uint64_t attr_len = 0;
    std::memcpy(&attr_len, data + IMAGE_HEADER_BYTES, sizeof(uint64_t));
    const size_t attr_off = IMAGE_HEADER_BYTES + sizeof(uint64_t);
    if (attr_len > size - attr_off) return false;
    if (attr_len > std::numeric_limits<uint32_t>::max()) return false;
    out.attr = reinterpret_cast<const char*>(data + attr_off);
    out.attr_len = static_cast<size_t>(attr_len);
    out.pixels = data + attr_off + out.attr_len;
    out.pixel_bytes = size - attr_off - out.attr_len;
    out.header.attribute_string_len = static_cast<uint32_t>(out.attr_len);
    return true;
}

// Position along the slice normal: orders a stack of parallel slices.
double depth(const ISMRMRD::ImageHeader& h)
{
    return double(h.position[0]) * h.slice_dir[0]
         + double(h.position[1]) * h.slice_dir[1]
         + double(h.position[2]) * h.slice_dir[2];
}

using SizeKey = std::tuple<uint16_t, uint16_t, uint16_t, uint16_t, uint16_t>;

SizeKey size_key(const ISMRMRD::ImageHeader& h)
{
    return {h.matrix_size[0], h.matrix_size[1], h.matrix_size[2], h.channels, h.data_type};
}

} // namespace

void write_latest_image_h5_file(const std::filesystem::path& dest,
                                const std::string& xml,
                                const std::vector<std::vector<uint8_t>>& images)
{
    std::vector<WireImage> parsed;
    for (const auto& img : images) {
        WireImage w;
        if (parse(img.data(), img.size(), w)) parsed.push_back(w);
        else LOG_WARN("Skipping malformed image while writing latest H5");
    }
    if (parsed.empty()) return;

    // Stable, meaningful order: by image type, then along the slice normal.
    std::stable_sort(parsed.begin(), parsed.end(), [](const WireImage& a, const WireImage& b) {
        if (a.header.image_type != b.header.image_type)
            return a.header.image_type < b.header.image_type;
        return depth(a.header) < depth(b.header);
    });

    // An ISMRMRD image variable holds images of one size; one variable per size.
    std::map<SizeKey, std::string> var_for_size;

    auto tmp = dest;
    tmp += ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    std::filesystem::remove(tmp, ec);
    {
        MrdSink sink(tmp);
        if (!xml.empty()) sink.set_header(xml);
        for (const auto& w : parsed) {
            auto [it, inserted] = var_for_size.try_emplace(
                size_key(w.header), "image_" + std::to_string(var_for_size.size()));
            sink.append_image(it->second, w.header, w.attr, w.attr_len, w.pixels, w.pixel_bytes);
        }
        sink.close();
    }
    std::filesystem::rename(tmp, dest, ec);
    if (ec) throw std::runtime_error("rename latest H5 failed: " + ec.message());
}

LatestSnapshot::LatestSnapshot(std::filesystem::path dir)
    : dest_(std::move(dir) / "latest_image.h5")
{
    std::error_code ec;
    std::filesystem::remove(dest_, ec);
    worker_ = std::thread(&LatestSnapshot::run, this);
}

LatestSnapshot::~LatestSnapshot()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void LatestSnapshot::begin_scan(std::string xml_header)
{
    std::lock_guard<std::mutex> lk(mtx_);
    images_.clear();
    xml_ = std::move(xml_header);
    ++scan_;
    dirty_ = false;
    view_.path.clear();
    view_.images = 0;
    ++view_.generation;
}

void LatestSnapshot::add_image(const uint8_t* body, size_t size)
{
    WireImage w;
    if (!parse(body, size, w)) return;
    const auto& h = w.header;
    Key key{{std::llround(h.position[0] * 100.0), std::llround(h.position[1] * 100.0),
             std::llround(h.position[2] * 100.0)},
            {int32_t(std::lround(h.slice_dir[0] * 1000.0)),
             int32_t(std::lround(h.slice_dir[1] * 1000.0)),
             int32_t(std::lround(h.slice_dir[2] * 1000.0))},
            h.image_type};
    {
        std::lock_guard<std::mutex> lk(mtx_);
        images_[key].assign(body, body + size);
        dirty_ = true;
    }
    cv_.notify_one();
}

LatestSnapshot::View LatestSnapshot::view() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return view_;
}

LatestSnapshot::Perf LatestSnapshot::perf() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return perf_;
}

void LatestSnapshot::run()
{
    std::unique_lock<std::mutex> lk(mtx_);
    for (;;) {
        cv_.wait(lk, [this] { return stop_ || dirty_; });
        if (stop_) return;
        dirty_ = false;
        const uint64_t scan = scan_;
        const std::string xml = xml_;
        std::vector<std::vector<uint8_t>> images;
        images.reserve(images_.size());
        for (const auto& [k, v] : images_) images.push_back(v);
        lk.unlock();

        bool ok = true;
        const auto t0 = std::chrono::steady_clock::now();
        try {
            write_latest_image_h5_file(dest_, xml, images);
        } catch (const std::exception& e) {
            ok = false;
            LOG_WARN("Latest H5 write failed: " << e.what());
        }
        const auto us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());

        lk.lock();
        if (!ok) {
            ++perf_.failed;
            continue;   // the next change writes again
        }
        ++perf_.writes;
        perf_.last_write_us = us;
        perf_.max_write_us = std::max(perf_.max_write_us, us);
        if (scan != scan_) continue;   // a new scan started while writing
        view_.path = dest_.string();
        view_.images = images.size();
        ++view_.generation;
    }
}

} // namespace mrd
