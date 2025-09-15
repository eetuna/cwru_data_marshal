/*
 * File: include/atomic_write.hpp
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
#include <string>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

// Atomic file writer: writes to <path>.tmp, fsyncs, then rename() to final.
inline void // Atomic write helper
write_atomic(const std::string& final_path, const std::string& data) {
    std::string tmp = final_path + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) throw std::runtime_error("Failed to open temp file: " + tmp);
        ofs.write(data.data(), (std::streamsize)data.size());
        ofs.flush();
        if (!ofs) throw std::runtime_error("Failed to write temp file: " + tmp);
    }
    int fd = ::open(tmp.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
    if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
        std::perror("rename");
        throw std::runtime_error("Failed to rename temp file");
    }
}
