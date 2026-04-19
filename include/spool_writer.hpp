/*
 * File: include/spool_writer.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Append-only raw MRD spool file. Dump mode writes wire frames
 *          here during the scan; a converter replays the spool into an
 *          ISMRMRD HDF5 file after CLOSE.
 *
 * Spool record format:
 *   [uint16 tag][uint32 body_length][body bytes]
 *
 * body bytes are the *exact* wire body the scanner/recon sent, as
 * already assembled by mrd_tcp_listener / recon_forwarder. The converter
 * is responsible for parsing each tag's inner framing (e.g. the inner
 * [uint32][text+NUL] prefix for TEXT / CONFIG_TEXT / METADATA_XML).
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

namespace mrd {

class SpoolWriter {
public:
    explicit SpoolWriter(const std::filesystem::path& path)
        : path_(path)
    {
        std::filesystem::create_directories(path.parent_path());
        fp_ = std::fopen(path.c_str(), "wb");
        if (!fp_) {
            last_error_ = std::string("fopen failed: ") + std::strerror(errno);
            return;
        }
        // 1 MiB userspace buffer. Dump worker batches are tiny (one
        // fwrite per MRD record) so this keeps sys-call traffic low.
        static constexpr size_t kBufSize = 1 << 20;
        std::setvbuf(fp_, nullptr, _IOFBF, kBufSize);
    }

    ~SpoolWriter() { close(); }

    SpoolWriter(const SpoolWriter&) = delete;
    SpoolWriter& operator=(const SpoolWriter&) = delete;

    // Append one MRD record. Returns true on success.
    // On failure, stores the error in last_error_ and returns false.
    bool append(uint16_t tag, const void* body, uint32_t length)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!fp_) return false;
        // Record prefix: tag (LE) + length (LE). The wire protocol is
        // already little-endian on x86-64 (our target) so a raw write
        // is correct; if we ever build on big-endian we would need to
        // byteswap here.
        if (std::fwrite(&tag, sizeof(tag), 1, fp_) != 1) return fail_(ferror(fp_));
        if (std::fwrite(&length, sizeof(length), 1, fp_) != 1) return fail_(ferror(fp_));
        if (length > 0) {
            if (std::fwrite(body, 1, length, fp_) != length) return fail_(ferror(fp_));
        }
        records_++;
        bytes_ += sizeof(tag) + sizeof(length) + length;
        return true;
    }

    // Flush userspace buffer + fsync.
    bool flush()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!fp_) return false;
        if (std::fflush(fp_) != 0) return fail_(ferror(fp_));
        return true;
    }

    void close()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (fp_) {
            std::fflush(fp_);
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

    uint64_t records() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return records_;
    }
    uint64_t bytes() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return bytes_;
    }
    bool healthy() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return fp_ != nullptr && last_error_.empty();
    }
    std::string last_error() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return last_error_;
    }
    const std::filesystem::path& path() const { return path_; }

private:
    bool fail_(int err)
    {
        last_error_ = std::string("spool write failed: ") +
            (err ? std::strerror(err) : "unknown error");
        if (fp_) { std::fclose(fp_); fp_ = nullptr; }
        return false;
    }

    std::filesystem::path path_;
    mutable std::mutex mtx_;
    std::FILE* fp_{nullptr};
    uint64_t records_{0};
    uint64_t bytes_{0};
    std::string last_error_;
};

} // namespace mrd
