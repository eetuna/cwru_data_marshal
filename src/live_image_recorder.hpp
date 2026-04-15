/*
 * File: src/live_image_recorder.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Async per-lane live image history recorder.
 */

#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mrd {

class MrdSink;

class LiveImageRecorder {
public:
    explicit LiveImageRecorder(std::filesystem::path lane_dir);
    ~LiveImageRecorder();

    LiveImageRecorder(const LiveImageRecorder&) = delete;
    LiveImageRecorder& operator=(const LiveImageRecorder&) = delete;

    void append_image(std::string filename,
                      std::string xml,
                      std::vector<uint8_t> body);
    void close_scan();

private:
    struct Job {
        std::function<void()> fn;
    };

    std::filesystem::path lane_dir_;
    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    bool stopping_{false};

    std::string current_filename_;
    std::unique_ptr<MrdSink> sink_;

    void enqueue(std::function<void()> fn);
    void worker_loop();
    void close_scan_on_worker();
    void ensure_sink_on_worker(const std::string& filename, const std::string& xml);
};

} // namespace mrd
