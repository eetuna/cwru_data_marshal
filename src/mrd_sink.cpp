/*
 * File: src/mrd_sink.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Implementation of canonical ISMRMRD HDF5 sink + standalone-file writer
 */

#undef LOG_COMPONENT
#define LOG_COMPONENT "mrd_sink"
#include "logging.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace mrd {

// ---------------------------------------------------------------------------
// MrdSink
// ---------------------------------------------------------------------------

MrdSink::MrdSink(const std::filesystem::path& path, const std::string& groupname)
    : path_(path), groupname_(groupname)
{
    namespace fs = std::filesystem;
    fs::create_directories(path.parent_path());
    dataset_ = std::make_unique<ISMRMRD::Dataset>(path.c_str(), groupname.c_str(), true);
    LOG_INFO("Opened HDF5 sink: " << path.string());
}

MrdSink::~MrdSink()
{
    close();
}

void MrdSink::set_header(const std::string& xml)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!dataset_) return;
    dataset_->writeHeader(xml);
}

void MrdSink::append_acquisition(const ISMRMRD::Acquisition& acq)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!dataset_) return;
    dataset_->appendAcquisition(acq);
    ++acq_count_;
}

void MrdSink::append_image(const std::string& varname, const ISMRMRD::ImageHeader& hdr,
                           const char* attr_str, size_t attr_len,
                           const void* pixel_data, size_t pixel_bytes)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!dataset_) return;

    // Dispatch on data_type to construct the correctly-typed Image<T>
    switch (hdr.data_type) {
    case ISMRMRD::ISMRMRD_USHORT: {
        ISMRMRD::Image<uint16_t> img;
        img.setHead(hdr);
        if (attr_len > 0) img.setAttributeString(std::string(attr_str, attr_len));
        std::memcpy(img.getDataPtr(), pixel_data, pixel_bytes);
        dataset_->appendImage(varname, img);
        break;
    }
    case ISMRMRD::ISMRMRD_SHORT: {
        ISMRMRD::Image<int16_t> img;
        img.setHead(hdr);
        if (attr_len > 0) img.setAttributeString(std::string(attr_str, attr_len));
        std::memcpy(img.getDataPtr(), pixel_data, pixel_bytes);
        dataset_->appendImage(varname, img);
        break;
    }
    case ISMRMRD::ISMRMRD_UINT: {
        ISMRMRD::Image<uint32_t> img;
        img.setHead(hdr);
        if (attr_len > 0) img.setAttributeString(std::string(attr_str, attr_len));
        std::memcpy(img.getDataPtr(), pixel_data, pixel_bytes);
        dataset_->appendImage(varname, img);
        break;
    }
    case ISMRMRD::ISMRMRD_INT: {
        ISMRMRD::Image<int32_t> img;
        img.setHead(hdr);
        if (attr_len > 0) img.setAttributeString(std::string(attr_str, attr_len));
        std::memcpy(img.getDataPtr(), pixel_data, pixel_bytes);
        dataset_->appendImage(varname, img);
        break;
    }
    case ISMRMRD::ISMRMRD_FLOAT: {
        ISMRMRD::Image<float> img;
        img.setHead(hdr);
        if (attr_len > 0) img.setAttributeString(std::string(attr_str, attr_len));
        std::memcpy(img.getDataPtr(), pixel_data, pixel_bytes);
        dataset_->appendImage(varname, img);
        break;
    }
    case ISMRMRD::ISMRMRD_DOUBLE: {
        ISMRMRD::Image<double> img;
        img.setHead(hdr);
        if (attr_len > 0) img.setAttributeString(std::string(attr_str, attr_len));
        std::memcpy(img.getDataPtr(), pixel_data, pixel_bytes);
        dataset_->appendImage(varname, img);
        break;
    }
    case ISMRMRD::ISMRMRD_CXFLOAT: {
        ISMRMRD::Image<complex_float_t> img;
        img.setHead(hdr);
        if (attr_len > 0) img.setAttributeString(std::string(attr_str, attr_len));
        std::memcpy(img.getDataPtr(), pixel_data, pixel_bytes);
        dataset_->appendImage(varname, img);
        break;
    }
    case ISMRMRD::ISMRMRD_CXDOUBLE: {
        ISMRMRD::Image<complex_double_t> img;
        img.setHead(hdr);
        if (attr_len > 0) img.setAttributeString(std::string(attr_str, attr_len));
        std::memcpy(img.getDataPtr(), pixel_data, pixel_bytes);
        dataset_->appendImage(varname, img);
        break;
    }
    default:
        LOG_WARN("Unknown image data_type " << hdr.data_type << ", storing as float");
        ISMRMRD::Image<float> img;
        img.setHead(hdr);
        if (attr_len > 0) img.setAttributeString(std::string(attr_str, attr_len));
        std::memcpy(img.getDataPtr(), pixel_data, pixel_bytes);
        dataset_->appendImage(varname, img);
        break;
    }

    ++img_count_;
}

void MrdSink::append_waveform(const ISMRMRD::Waveform& wf)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!dataset_) return;
    dataset_->appendWaveform(wf);
    ++wf_count_;
}

void MrdSink::append_unknown_bytes(const void* data, size_t len)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!dataset_) return;

    // Store unknown bytes as a 1D float NDArray (reinterpret, pad to float boundary)
    // This is a last resort — UNKNOWN data should be rare.
    size_t nfloats = (len + sizeof(float) - 1) / sizeof(float);
    ISMRMRD::NDArray<float> arr;
    std::vector<size_t> dims = {nfloats};
    arr.resize(dims);
    std::memset(arr.getDataPtr(), 0, nfloats * sizeof(float));
    std::memcpy(arr.getDataPtr(), data, len);
    dataset_->appendNDArray("unknown_data", arr);
}

void MrdSink::close()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (dataset_) {
        dataset_.reset(); // ISMRMRD::Dataset destructor closes HDF5
        LOG_INFO("Closed HDF5 sink: " << path_.string()
                 << " (acq=" << acq_count_
                 << " img=" << img_count_
                 << " wf=" << wf_count_ << ")");
    }
}

// ---------------------------------------------------------------------------
// Standalone file writer (atomic rename)
// ---------------------------------------------------------------------------

void write_standalone_file(const std::filesystem::path& dest,
                           const void* data, size_t len)
{
    namespace fs = std::filesystem;
    fs::create_directories(dest.parent_path());

    fs::path tmp = dest;
    tmp += ".tmp";

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
        throw std::runtime_error("open tmp failed: " + tmp.string() + ": " + std::strerror(errno));

    auto* ptr = static_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t written = ::write(fd, ptr, remaining);
        if (written == -1) {
            int err = errno;
            ::close(fd);
            ::unlink(tmp.c_str());
            throw std::runtime_error("write tmp failed: " + tmp.string() + ": " + std::strerror(err));
        }
        ptr += static_cast<size_t>(written);
        remaining -= static_cast<size_t>(written);
    }

    if (::fsync(fd) == -1) {
        int err = errno;
        ::close(fd);
        ::unlink(tmp.c_str());
        throw std::runtime_error("fsync tmp failed: " + tmp.string() + ": " + std::strerror(err));
    }

    ::close(fd);

    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec)
        throw std::runtime_error("rename tmp->dst failed: " + ec.message());
}

} // namespace mrd
