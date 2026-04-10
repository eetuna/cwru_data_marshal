/*
 * File: include/mrd_sink.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Canonical ISMRMRD HDF5 archival + standalone-file writer for live viz
 *
 * Uses libismrmrd Dataset (appendAcquisition, appendImage, appendWaveform).
 * No SWMR. No custom HDF5 layout.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <variant>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/dataset.h>
#include <ismrmrd/waveform.h>

namespace mrd {

// Wraps one ISMRMRD::Dataset file. Thread-safe via internal mutex.
class MrdSink {
public:
    // Opens (or creates) an HDF5 file at `path` with group `groupname`.
    MrdSink(const std::filesystem::path& path, const std::string& groupname = "/dataset");
    ~MrdSink();

    MrdSink(const MrdSink&) = delete;
    MrdSink& operator=(const MrdSink&) = delete;

    // Write the XML header string. Call once before appending data.
    void set_header(const std::string& xml);

    // Typed append methods — delegate to ISMRMRD::Dataset
    void append_acquisition(const ISMRMRD::Acquisition& acq);

    // appendImage dispatches on data_type via std::visit
    void append_image(const std::string& varname, const ISMRMRD::ImageHeader& hdr,
                      const char* attr_str, size_t attr_len,
                      const void* pixel_data, size_t pixel_bytes);

    void append_waveform(const ISMRMRD::Waveform& wf);

    // Fallback: store raw bytes as an opaque NDArray (for UNKNOWN type).
    void append_unknown_bytes(const void* data, size_t len);

    // Close the HDF5 file. Idempotent.
    void close();

    bool is_open() const noexcept { return dataset_ != nullptr; }
    const std::filesystem::path& path() const noexcept { return path_; }

    uint32_t acquisition_count() const noexcept { return acq_count_; }
    uint32_t image_count() const noexcept { return img_count_; }
    uint32_t waveform_count() const noexcept { return wf_count_; }

private:
    std::filesystem::path path_;
    std::string groupname_;
    std::unique_ptr<ISMRMRD::Dataset> dataset_;
    mutable std::mutex mtx_;
    uint32_t acq_count_{0};
    uint32_t img_count_{0};
    uint32_t wf_count_{0};
};

// Write raw bytes to a standalone file via atomic rename (write tmp, fsync, rename).
// Used for latest_image.bin and latest_error.png.
void write_standalone_file(const std::filesystem::path& dest,
                           const void* data, size_t len);

} // namespace mrd
