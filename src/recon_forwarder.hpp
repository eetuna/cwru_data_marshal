/*
 * File: src/recon_forwarder.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Background thread that forwards scanner data to recon service
 *
 * Typed methods: post_header, post_config, post_frame, post_close.
 * No X-headers. Drops on failure (marshal keeps running per R11).
 * Uses libcurl for HTTP POST.
 */

#pragma once

#undef LOG_COMPONENT
#define LOG_COMPONENT "recon_fwd"
#include "logging.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <curl/curl.h>

namespace mrd {

class ReconForwarder {
public:
    using FailureCallback = std::function<void()>;

    explicit ReconForwarder(const std::string& recon_url,
                            FailureCallback on_failure = nullptr)
        : recon_url_(recon_url), on_failure_(std::move(on_failure))
    {
        worker_ = std::thread(&ReconForwarder::run, this);
    }

    ~ReconForwarder() { stop(); }

    ReconForwarder(const ReconForwarder&) = delete;
    ReconForwarder& operator=(const ReconForwarder&) = delete;

    void post_header(const std::string& body)  { enqueue(recon_url_ + "/header",  body); }
    void post_config(const std::string& body)  { enqueue(recon_url_ + "/config",  body); }
    void post_frame(const std::string& body)   { enqueue(recon_url_ + "/frame",   body); }
    void post_close()                          { enqueue(recon_url_ + "/close",   "");   }

    void stop() {
        running_.store(false);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    bool is_running() const { return running_.load(); }

private:
    struct Job {
        std::string url;
        std::string body;
    };

    std::string recon_url_;
    FailureCallback on_failure_;
    std::atomic<bool> running_{true};
    std::queue<Job> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;

    void enqueue(const std::string& url, const std::string& body) {
        std::lock_guard<std::mutex> lk(mtx_);
        // Cap queue to prevent unbounded growth if recon is slow
        if (queue_.size() > 10000) {
            LOG_WARN("Recon queue full (>10000), dropping message to " << url);
            return;
        }
        queue_.push({url, body});
        cv_.notify_one();
    }

    void run() {
        CURL* curl = curl_easy_init();
        if (!curl) {
            LOG_ERROR("Failed to init curl for recon forwarder");
            return;
        }

        while (running_.load()) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait_for(lk, std::chrono::milliseconds(100),
                             [this] { return !queue_.empty() || !running_.load(); });
                if (queue_.empty()) continue;
                job = std::move(queue_.front());
                queue_.pop();
            }

            if (!do_post(curl, job.url, job.body)) {
                LOG_WARN("Recon POST failed: " << job.url << " (dropping)");
                if (on_failure_) {
                    try { on_failure_(); } catch (...) {}
                }
            }
        }

        // Drain remaining
        std::lock_guard<std::mutex> lk(mtx_);
        while (!queue_.empty()) {
            auto& job = queue_.front();
            do_post(curl, job.url, job.body);
            queue_.pop();
        }

        curl_easy_cleanup(curl);
    }

    // Suppress response body
    static size_t discard_cb(void*, size_t size, size_t nmemb, void*) {
        return size * nmemb;
    }

    bool do_post(CURL* curl, const std::string& url, const std::string& body) {
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
        // No custom headers — no X-MRD-*, no X-Recon-*
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) return false;

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        return (http_code >= 200 && http_code < 300);
    }
};

} // namespace mrd
