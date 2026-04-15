/*
 * File: src/latest_image_writer.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Async writer for the live latest-image H5 file.
 */

#include "latest_image_writer.hpp"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <ismrmrd/ismrmrd.h>

#include "logging.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"

#undef LOG_COMPONENT
#define LOG_COMPONENT "latest_image"

namespace mrd {
namespace {

bool append_latest_wire_image(MrdSink& sink, const uint8_t* data, size_t size)
{
    if (size < IMAGE_HEADER_BYTES + sizeof(uint64_t)) return false;

    const auto* hdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(data);
    uint64_t attr_len = 0;
    std::memcpy(&attr_len, data + IMAGE_HEADER_BYTES, sizeof(uint64_t));
    const size_t attr_off = IMAGE_HEADER_BYTES + sizeof(uint64_t);
    if (size < attr_off + attr_len) return false;
    const size_t pixel_off = attr_off + static_cast<size_t>(attr_len);

    sink.append_image("image_0", *hdr,
                      reinterpret_cast<const char*>(data + attr_off),
                      static_cast<size_t>(attr_len),
                      data + pixel_off,
                      size - pixel_off);
    return true;
}

} // namespace

void write_latest_image_h5_file(const std::filesystem::path& dest,
                                const std::string& xml,
                                const std::vector<std::vector<uint8_t>>& images)
{
    if (images.empty()) return;

    auto tmp = dest;
    tmp += ".tmp";

    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    ec.clear();
    std::filesystem::remove(tmp, ec);

    {
        MrdSink sink(tmp);
        if (!xml.empty()) sink.set_header(xml);
        for (const auto& image : images) {
            if (!append_latest_wire_image(sink, image.data(), image.size())) {
                LOG_WARN("Skipping malformed image while writing latest H5");
            }
        }
        sink.close();
    }

    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        std::filesystem::remove(dest, ec);
        ec.clear();
        std::filesystem::rename(tmp, dest, ec);
        if (ec) throw std::runtime_error("rename latest H5 failed: " + ec.message());
    }
}

struct LatestImageWriter::Job {
    std::filesystem::path dest;
    std::string xml;
    std::vector<std::vector<uint8_t>> images;
    Completion completion;
};

struct LatestImageWriter::Impl {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Job> jobs;
    bool stopping{false};
    std::thread worker;

    Impl()
        : worker([this] { run(); })
    {}

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            stopping = true;
        }
        cv.notify_one();
        if (worker.joinable()) worker.join();
    }

    void enqueue(Job job)
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
    }

    void run()
    {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [&] { return stopping || !jobs.empty(); });
                if (stopping && jobs.empty()) break;
                job = std::move(jobs.front());
                jobs.pop_front();
            }

            try {
                write_latest_image_h5_file(job.dest, job.xml, job.images);
                if (job.completion) job.completion(job.dest);
            } catch (const std::exception& e) {
                LOG_WARN("Latest H5 write failed: " << e.what());
            }
        }
    }
};

LatestImageWriter::LatestImageWriter()
    : impl_(std::make_unique<Impl>())
{}

LatestImageWriter::~LatestImageWriter() = default;

void LatestImageWriter::enqueue(std::filesystem::path dest,
                                std::string xml,
                                std::vector<std::vector<uint8_t>> images,
                                Completion completion)
{
    impl_->enqueue(Job{std::move(dest), std::move(xml), std::move(images),
                       std::move(completion)});
}

} // namespace mrd
