/*
 * File: src/marshal_state.hpp
 * Project: CWRU Data Marshal
 * Purpose: Internal support module
 * Notes:
 *  - See docs/PURPOSE.md and docs/ARCHITECTURE.md
 *  - Atomic file writes via include/atomic_write.hpp
 *  - /health returns constant JSON; no shared state
 *  - WebSocket ping/pong keepalive recommended
 * Last updated: 2025-09-15
 */

#pragma once
#include <atomic>
#include <mutex>
#include <memory>
#include <string>
#include <chrono>
#include <queue>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include <filesystem>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include "common/pose.hpp"

namespace mrd
{
class MrdSink;

struct FlushPolicy
{
    std::size_t max_pending_frames{1};
    std::chrono::milliseconds max_pending_interval{0};
};
}
enum class SinkMode
{
    MRD,
    DUMPBOX
};

struct HubClient
{
    std::shared_ptr<void> ws;
}; // opaque holder

struct IndexEntry
{
    std::chrono::system_clock::time_point t;
    std::string file;
    uint64_t seq{0};
    std::string type{"acq"};
};

struct MarshalState
{
    SinkMode sink_mode{SinkMode::MRD};
    std::string dumpbox_root{"./data/dumpbox"};
    std::string dumpbox_session{};
    std::atomic<uint64_t> seq{0};
    mrd::FlushPolicy flush_policy{1, std::chrono::milliseconds{0}};

    PoseStore poses;
    std::string data_dir{"./data"};
    std::mutex session_mtx; // Protects dumpbox_session initialization
    std::size_t max_body_bytes{128ULL * 1024ULL * 1024ULL}; // Default 128 MiB
    boost::asio::io_context *io = nullptr;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    // WS emit hook (set by WsServer on init). Safe to call from HTTP handlers.
    std::function<void(const std::string &)> ws_emit = [](const std::string &) {};

    // Optional topic-based emit hook (set by WsServer on init).
    std::function<void(const std::string &, const std::string &topic)> ws_emit_topic =
        [](const std::string &, const std::string &) {};

    std::shared_ptr<mrd::MrdSink> mrd_sink;

    // JSON write queue (non-blocking) - for async disk I/O
    enum class WriteType { MRD, BIO, POSE };
    struct WriteRequest {
        WriteType type;
        std::string data;
    };
    std::queue<WriteRequest> json_write_queue;
    std::mutex json_queue_mutex;
    std::condition_variable json_queue_cv;
    std::atomic<bool> json_writer_running{true};
    std::thread json_writer_thread;
    std::filesystem::path json_index_path;   // MRD index.jsonl
    std::filesystem::path json_latest_path;  // MRD latest.json
    std::filesystem::path json_bio_path;     // bio.jsonl
    std::filesystem::path json_pose_path;    // poses.jsonl

    // In-memory latest (for fast endpoint reads)
    std::mutex latest_mrd_mutex;
    std::string latest_mrd_json;

    std::mutex latest_bio_mutex;
    std::string latest_bio_json;

    std::mutex latest_pose_mutex;
    std::string latest_pose_json;

    // Reconstruction service configuration (Phase 2)
    std::string recon_endpoint;    // e.g., "http://localhost:9002"
    bool recon_enabled{false};     // True if --recon-endpoint provided

    // Reply-to mapping: job_id -> URL to POST reconstructed images back to
    std::mutex reply_to_mutex;
    std::unordered_map<std::string, std::string> reply_to_urls;
};
