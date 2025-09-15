#pragma once
#include <unordered_set>
#include <mutex>
#include <memory>
#include <string>
#include <chrono>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include "common/pose.hpp"


struct HubClient { std::shared_ptr<void> ws; }; // opaque holder


struct IndexEntry {
std::chrono::system_clock::time_point t;
std::string file;
uint64_t seq{0};
std::string type{"acq"};
};


struct MarshalState {
PoseStore poses;
std::string data_dir{"/data"};
std::mutex ws_mtx;
std::unordered_set<void*> ws_clients; // track raw ptr keys
boost::asio::io_context* io = nullptr;
std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};