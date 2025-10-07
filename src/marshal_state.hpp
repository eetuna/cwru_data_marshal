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
#include <unordered_set>
#include <mutex>
#include <memory>
#include <string>
#include <chrono>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include "common/pose.hpp"

namespace mrd
{
class SwmrManager;
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

    PoseStore poses;
    std::string data_dir{"./data"};
    std::mutex ws_mtx;
    std::unordered_set<void *> ws_clients; // track raw ptr keys
    boost::asio::io_context *io = nullptr;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    // WS emit hook (set by WsServer on init). Safe to call from HTTP handlers.
    std::function<void(const std::string &)> ws_emit = [](const std::string &) {};

    // Optional topic-based emit hook (set by WsServer on init).
    std::function<void(const std::string &, const std::string &topic)> ws_emit_topic =
        [](const std::string &, const std::string &) {};

    std::shared_ptr<mrd::SwmrManager> swmr;
};