/*
 * File: src/latest_image_writer.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Per-scan async appender for the live ISMRMRD H5 file.
 */

#include "latest_image_writer.hpp"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

#include <ismrmrd/ismrmrd.h>

#include "logging.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"

#undef LOG_COMPONENT
#define LOG_COMPONENT "latest_image"

namespace mrd {

namespace {

bool append_wire_image(MrdSink& sink, const uint8_t* data, size_t size)
{
    if (size < IMAGE_HEADER_BYTES + sizeof(uint64_t)) return false;

    const auto* hdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(data);
    uint64_t attr_len = 0;
    std::memcpy(&attr_len, data + IMAGE_HEADER_BYTES, sizeof(uint64_t));
    const size_t attr_off = IMAGE_HEADER_BYTES + sizeof(uint64_t);
    if (size < attr_off + attr_len) return false;
    const size_t pixel_off = attr_off + static_cast<size_t>(attr_len);

    // Group by image_series_index — same convention as python-ismrmrd-server
    // (connection.py:390) and dump_recorder.cpp:217. One HDF5 group per volume.
    const std::string varname = "image_" + std::to_string(hdr->image_series_index);
    sink.append_image(varname, *hdr,
                      reinterpret_cast<const char*>(data + attr_off),
                      static_cast<size_t>(attr_len),
                      data + pixel_off,
                      size - pixel_off);
    return true;
}

} // namespace

struct LatestImageWriter::Impl {
    struct Job {
        enum Kind { Open, Append, Close, Stop } kind{Append};
        std::filesystem::path dest;
        std::string xml;
        std::vector<uint8_t> body;
        AppendCompletion on_complete;
        std::shared_ptr<std::promise<void>> done;
    };

    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::thread worker;

    std::unique_ptr<MrdSink> sink;
    std::filesystem::path current_path;

    Impl()
        : worker([this] { run(); })
    {}

    ~Impl()
    {
        Job stop;
        stop.kind = Job::Stop;
        {
            std::lock_guard<std::mutex> lk(mtx);
            jobs.push_back(std::move(stop));
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
                cv.wait(lk, [&] { return !jobs.empty(); });
                job = std::move(jobs.front());
                jobs.pop_front();
            }

            try {
                switch (job.kind) {
                case Job::Open: {
                    close_sink();
                    std::error_code ec;
                    std::filesystem::create_directories(job.dest.parent_path(), ec);
                    sink = std::make_unique<MrdSink>(job.dest);
                    if (!job.xml.empty()) sink->set_header(job.xml);
                    current_path = job.dest;
                    break;
                }
                case Job::Append:
                    if (sink && append_wire_image(*sink, job.body.data(),
                                                  job.body.size())) {
                        if (job.on_complete) job.on_complete(current_path);
                    }
                    break;
                case Job::Close:
                    close_sink();
                    break;
                case Job::Stop:
                    close_sink();
                    return;
                }
            } catch (const std::exception& e) {
                LOG_WARN("Live H5 write failed: " << e.what());
            }

            if (job.done) job.done->set_value();
        }
    }

    void close_sink()
    {
        if (sink) {
            sink->close();
            sink.reset();
        }
        current_path.clear();
    }
};

LatestImageWriter::LatestImageWriter()
    : impl_(std::make_unique<Impl>())
{}

LatestImageWriter::~LatestImageWriter() = default;

void LatestImageWriter::open_scan(std::filesystem::path dest, std::string xml)
{
    Impl::Job job;
    job.kind = Impl::Job::Open;
    job.dest = std::move(dest);
    job.xml = std::move(xml);
    job.done = std::make_shared<std::promise<void>>();
    auto fut = job.done->get_future();
    impl_->enqueue(std::move(job));
    fut.wait();
}

void LatestImageWriter::append_image(std::vector<uint8_t> body,
                                     AppendCompletion on_complete)
{
    Impl::Job job;
    job.kind = Impl::Job::Append;
    job.body = std::move(body);
    job.on_complete = std::move(on_complete);
    impl_->enqueue(std::move(job));
}

void LatestImageWriter::close_scan()
{
    Impl::Job job;
    job.kind = Impl::Job::Close;
    job.done = std::make_shared<std::promise<void>>();
    auto fut = job.done->get_future();
    impl_->enqueue(std::move(job));
    fut.wait();
}

} // namespace mrd
