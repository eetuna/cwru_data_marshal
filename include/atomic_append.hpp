#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif

namespace fs = std::filesystem;

// Append one line to a file atomically and durably by rewriting the file:
//  - read current file (if exists)
//  - write tmp with previous content + new line
//  - fsync tmp
//  - rename tmp -> dst
//  - fsync parent dir
inline void atomic_append_line(const fs::path& dst, const std::string& line) {
    fs::create_directories(dst.parent_path());
    const fs::path tmp = dst.string() + ".tmp";

    // Load existing content if present
    std::string previous;
    {
        std::error_code ec;
        std::ifstream in(dst, std::ios::in | std::ios::binary);
        if (in.good()) {
            previous.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
    }

    // Write tmp with previous + new line
    {
        std::ofstream out(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("atomic_append: open tmp failed: " + tmp.string());
        out.write(previous.data(), (std::streamsize)previous.size());
        out.write(line.data(), (std::streamsize)line.size());
        out.put('\n');
        out.flush();
#ifndef _WIN32
        int fd = ::open(tmp.c_str(), O_RDONLY);
        if (fd >= 0) { ::fsync(fd); ::close(fd); }
#endif
        if (!out) throw std::runtime_error("atomic_append: write tmp failed: " + tmp.string());
    }

    // Atomic rename
    std::error_code ec;
    fs::rename(tmp, dst, ec);
    if (ec) throw std::runtime_error("atomic_append: rename failed: " + ec.message());

#ifndef _WIN32
    // fsync parent dir for metadata durability
    int dfd = ::open(dst.parent_path().c_str(), O_RDONLY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
#endif
}
