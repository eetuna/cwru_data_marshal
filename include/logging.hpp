#pragma once
// Structured logging for MRI Data Marshal
//
// Usage:
//   #define LOG_COMPONENT "marshal_http"
//   #include "logging.hpp"
//
//   LOG_INFO("Server started port=" << port);
//   LOG_ERROR("req=" << req_id << " Failed: " << e.what());
//
// Output:
//   2026-03-06T14:30:45.123Z [INFO] [marshal_http] Server started port=8080
//   2026-03-06T14:30:45.124Z [ERROR] [marshal_http] req=a1b2c3d4 Failed: timeout

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>

#ifndef LOG_COMPONENT
#define LOG_COMPONENT "unknown"
#endif

namespace marshal_log {

inline std::string timestamp()
{
    using namespace std::chrono;
    auto now = time_point_cast<milliseconds>(system_clock::now());
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t tt = system_clock::to_time_t(now);
    std::tm tm;
    gmtime_r(&tt, &tm);

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);

    std::ostringstream oss;
    oss << buf << '.' << std::setw(3) << std::setfill('0') << ms.count() << 'Z';
    return oss.str();
}

inline std::string generate_request_id()
{
    static thread_local std::mt19937 rng(
        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<uint32_t> dist;
    uint32_t val = dist(rng);
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", val);
    return std::string(buf);
}

// Thread-safe single-line log output
inline void log_msg(const char *level, const char *component, const std::string &msg)
{
    static std::mutex log_mutex;
    std::string ts = timestamp();
    std::lock_guard<std::mutex> lock(log_mutex);
    std::fprintf(stderr, "%s [%s] [%s] %s\n", ts.c_str(), level, component, msg.c_str());
}

} // namespace marshal_log

#define LOG_INFO(msg) do { \
    std::ostringstream _log_oss; \
    _log_oss << msg; \
    marshal_log::log_msg("INFO", LOG_COMPONENT, _log_oss.str()); \
} while (0)

#define LOG_WARN(msg) do { \
    std::ostringstream _log_oss; \
    _log_oss << msg; \
    marshal_log::log_msg("WARN", LOG_COMPONENT, _log_oss.str()); \
} while (0)

#define LOG_ERROR(msg) do { \
    std::ostringstream _log_oss; \
    _log_oss << msg; \
    marshal_log::log_msg("ERROR", LOG_COMPONENT, _log_oss.str()); \
} while (0)

#ifdef MARSHAL_DEBUG
#define LOG_DEBUG(msg) do { \
    std::ostringstream _log_oss; \
    _log_oss << msg; \
    marshal_log::log_msg("DEBUG", LOG_COMPONENT, _log_oss.str()); \
} while (0)
#else
#define LOG_DEBUG(msg) do {} while (0)
#endif
