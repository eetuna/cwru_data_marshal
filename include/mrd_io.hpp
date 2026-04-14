/*
 * File: include/mrd_io.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Utility functions for I/O, timestamps, directory creation
 *
 * Split paths: from_scanner/ and from_reconstruction/ under --dump-dir.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace mrd {

inline std::string iso8601_now_ms()
{
    using namespace std::chrono;
    auto now = time_point_cast<milliseconds>(system_clock::now());
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t tt = system_clock::to_time_t(now);
    std::tm tm;
    gmtime_r(&tt, &tm);

    char base[32];
    std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);

    std::ostringstream oss;
    oss << base << '.' << std::setw(3) << std::setfill('0') << ms.count() << 'Z';
    return oss.str();
}

inline uint64_t now_ms_epoch()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

inline void ensure_dir(const std::filesystem::path& p)
{
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        std::filesystem::create_directories(p, ec);
        if (ec)
            throw std::runtime_error("create_directories failed: " + ec.message());
    }
}

// Generate a scan filename with timestamp, e.g. "scan_2026-04-10T12:34:56.789Z.h5"
inline std::string scan_filename()
{
    return "scan_" + iso8601_now_ms() + ".h5";
}

// Live subtree (always populated): one file per scan, images appended.
inline std::filesystem::path live_root(const std::filesystem::path& dump_dir)
{
    auto p = dump_dir / "live";
    ensure_dir(p);
    return p;
}

inline std::filesystem::path live_scanner_dir(const std::filesystem::path& dump_dir)
{
    auto p = live_root(dump_dir) / "from_scanner";
    ensure_dir(p);
    return p;
}

inline std::filesystem::path live_recon_dir(const std::filesystem::path& dump_dir)
{
    auto p = live_root(dump_dir) / "from_reconstruction";
    ensure_dir(p);
    return p;
}

// Dump subtree (only populated when --dump is on): canonical archival per scan.
inline std::filesystem::path dump_root(const std::filesystem::path& dump_dir)
{
    auto p = dump_dir / "dump";
    ensure_dir(p);
    return p;
}

inline std::filesystem::path dump_scanner_dir(const std::filesystem::path& dump_dir)
{
    auto p = dump_root(dump_dir) / "from_scanner";
    ensure_dir(p);
    return p;
}

inline std::filesystem::path dump_recon_dir(const std::filesystem::path& dump_dir)
{
    auto p = dump_root(dump_dir) / "from_reconstruction";
    ensure_dir(p);
    return p;
}

} // namespace mrd
