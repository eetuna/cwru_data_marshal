/*
 * File: include/common/pose.hpp
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
#include <array>
#include <string>
#include <mutex>
#include <chrono>
#include <nlohmann/json.hpp>


struct Pose {
std::chrono::system_clock::time_point t;
std::array<double,3> p{0,0,0};
std::array<double,9> R{1,0,0, 0,1,0, 0,0,1};
std::string frame = "scanner";
std::string source = "fk";
};


inline nlohmann::json pose_to_json(const Pose& pose){
using nlohmann::json; using namespace std::chrono;
auto ms = time_point_cast<milliseconds>(pose.t).time_since_epoch().count();
return json{
{"t_ms", ms},
{"frame", pose.frame},
{"p", pose.p},
{"R", pose.R},
{"source", pose.source}
};
}


class PoseStore {
std::mutex m_;
Pose latest_{};
public:
void set(Pose p){ std::scoped_lock lk(m_); latest_ = std::move(p); }
Pose get(){ std::scoped_lock lk(m_); return latest_; }
};