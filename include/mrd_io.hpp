#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <system_error>
#include <stdexcept>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "marshal_state.hpp"

// mrd_io.hpp centralizes the logic for writing MRD payloads to disk in a way
// that guarantees downstream readers only observe complete artifacts.  It is
// shared by both the HTTP and WebSocket ingestion paths, so any new ingest
// surface can call into the same helpers without reimplementing the
// filesystem choreography.
//
// The helpers provide a few building blocks:
//   * iso8601_now_ms: generate timestamps for filenames and metadata.
//   * ensure_dir: create sink directories on demand.
//   * write_atomic: durably write a blob via fsync + rename semantics.
//   * append_line: append to the index log with the same durability
//     guarantees.
//   * ingest_payload: given a payload and a MarshalState, create the target
//     MRD file, update index/metadata files, and emit WebSocket notifications.
//
// The ingest helper handles both live MRD sinks and dumpbox sessions.  It also
// keeps a process-wide sequence counter so files are unique even when multiple
// sources ingest concurrently.
namespace mrd
{
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

inline void ensure_dir(const std::filesystem::path &p)
{
    std::error_code ec;
    if (!std::filesystem::exists(p, ec))
    {
        std::filesystem::create_directories(p, ec);
        if (ec)
            throw std::runtime_error("create_directories failed: " + ec.message());
    }
}

struct SinkPaths
{
    std::filesystem::path sink_root;
    std::filesystem::path index_root;
};

inline SinkPaths resolve_sink_paths(MarshalState &state)
{
    namespace fs = std::filesystem;
    SinkPaths paths;

    fs::path mrd_root = fs::path(state.data_dir) / "mrd";
    ensure_dir(mrd_root);

    if (state.sink_mode == SinkMode::MRD)
    {
        paths.sink_root = mrd_root;
        paths.index_root = mrd_root;
    }
    else
    {
        std::string session = state.dumpbox_session.empty() ? iso8601_now_ms() : state.dumpbox_session;
        fs::path session_dir = fs::path(state.dumpbox_root) / session;
        paths.index_root = session_dir;
        paths.sink_root = session_dir / "files";
        std::error_code ec_mk;
        std::filesystem::create_directories(paths.sink_root, ec_mk);
        if (ec_mk)
            throw std::runtime_error("ensure dumpbox sink failed: " + ec_mk.message());
        state.dumpbox_session = session;
    }

    return paths;
}

inline void write_atomic(const std::filesystem::path &dst, const void *data, size_t n)
{
    namespace fs = std::filesystem;
    fs::path tmp = dst;
    tmp += ".tmp";

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
        throw std::runtime_error("open tmp failed: " + tmp.string() + ": " + std::strerror(errno));

    const char *ptr = static_cast<const char *>(data);
    size_t remaining = n;
    while (remaining > 0)
    {
        ssize_t written = ::write(fd, ptr, remaining);
        if (written == -1)
        {
            int err = errno;
            ::close(fd);
            ::unlink(tmp.c_str());
            throw std::runtime_error("write tmp failed: " + tmp.string() + ": " + std::strerror(err));
        }
        ptr += static_cast<size_t>(written);
        remaining -= static_cast<size_t>(written);
    }

    if (::fsync(fd) == -1)
    {
        int err = errno;
        ::close(fd);
        ::unlink(tmp.c_str());
        throw std::runtime_error("fsync tmp failed: " + tmp.string() + ": " + std::strerror(err));
    }

    if (::close(fd) == -1)
    {
        int err = errno;
        ::unlink(tmp.c_str());
        throw std::runtime_error("close tmp failed: " + tmp.string() + ": " + std::strerror(err));
    }

    std::error_code ec;
    fs::rename(tmp, dst, ec);
    if (ec)
        throw std::runtime_error("rename tmp->dst failed: " + ec.message());
}

inline void append_line(const std::filesystem::path &dst, const std::string &line)
{
    std::string payload = line;
    payload.push_back('\n');

    int fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1)
        throw std::runtime_error("open index for append failed: " + dst.string() + ": " + std::strerror(errno));

    const char *ptr = payload.data();
    size_t remaining = payload.size();
    while (remaining > 0)
    {
        ssize_t written = ::write(fd, ptr, remaining);
        if (written == -1)
        {
            int err = errno;
            ::close(fd);
            throw std::runtime_error("append index failed: " + dst.string() + ": " + std::strerror(err));
        }
        ptr += static_cast<size_t>(written);
        remaining -= static_cast<size_t>(written);
    }

    if (::fsync(fd) == -1)
    {
        int err = errno;
        ::close(fd);
        throw std::runtime_error("fsync index failed: " + dst.string() + ": " + std::strerror(err));
    }

    if (::close(fd) == -1)
        throw std::runtime_error("close index failed: " + dst.string() + ": " + std::strerror(errno));
}

inline std::atomic<uint64_t> &ingest_sequence()
{
    static std::atomic<uint64_t> seq{1};
    return seq;
}

inline nlohmann::json ingest_payload(MarshalState &state,
                                     const void *data,
                                     size_t size,
                                     const std::string &source)
{
    namespace fs = std::filesystem;

    const std::string ts = iso8601_now_ms();
    const uint64_t seq = ingest_sequence().fetch_add(1);

    std::ostringstream name;
    name << ts << '_' << std::setw(6) << std::setfill('0') << seq << ".mrd";

    SinkPaths paths = resolve_sink_paths(state);

    fs::path sink_root = paths.sink_root;
    fs::path index_root = paths.index_root;

    fs::path out_path = sink_root / name.str();
    write_atomic(out_path, data, size);

    std::error_code ec2;
    auto size_bytes = fs::file_size(out_path, ec2);
    if (ec2)
        size_bytes = size;

    nlohmann::json entry = {
        {"path", out_path.string()},
        {"ts", ts},
        {"size_bytes", size_bytes},
        {"type", "mrd"},
        {"seq", seq},
        {"source", source}};

    append_line(index_root / "index.jsonl", entry.dump());
    const std::string latest_dump = entry.dump();
    write_atomic(index_root / "latest.json", latest_dump.data(), latest_dump.size());

    try
    {
        state.ws_emit(entry.dump());
        state.ws_emit_topic(entry.dump(), "mrd.ingest");
    }
    catch (...)
    {
    }

    try
    {
        nlohmann::json evt = {
            {"type", "acq"},
            {"path", out_path.string()},
            {"seq", seq},
            {"size_bytes", size_bytes},
            {"ts", ts},
            {"source", source}};
        state.ws_emit(evt.dump());
    }
    catch (...)
    {
    }

    return entry;
}
} // namespace mrd

