/*
 * File: src/latest_image_writer.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Async writer for the live latest-image H5 file.
 */

#include "latest_image_writer.hpp"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <hdf5.h>
#include <ismrmrd/ismrmrd.h>

#include "logging.hpp"
#include "wire_guards.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"

#undef LOG_COMPONENT
#define LOG_COMPONENT "latest_image"

namespace mrd {
namespace {

constexpr const char* kDatasetGroup = "/dataset";
constexpr const char* kImageVarName = "image_0";

struct ParsedLatestWireImage {
    ISMRMRD::ImageHeader header{};
    std::string attributes;
    const uint8_t* pixel_data{nullptr};
    size_t pixel_bytes{0};
};

struct ScopedH5Handle {
    using Closer = herr_t (*)(hid_t);

    hid_t id{-1};
    Closer closer{nullptr};

    ScopedH5Handle() = default;
    ScopedH5Handle(hid_t handle, Closer close_fn)
        : id(handle), closer(close_fn)
    {}

    ScopedH5Handle(const ScopedH5Handle&) = delete;
    ScopedH5Handle& operator=(const ScopedH5Handle&) = delete;

    ScopedH5Handle(ScopedH5Handle&& other) noexcept
        : id(other.id), closer(other.closer)
    {
        other.id = -1;
        other.closer = nullptr;
    }

    ScopedH5Handle& operator=(ScopedH5Handle&& other) noexcept
    {
        if (this != &other) {
            reset();
            id = other.id;
            closer = other.closer;
            other.id = -1;
            other.closer = nullptr;
        }
        return *this;
    }

    ~ScopedH5Handle()
    {
        reset();
    }

    void reset(hid_t new_id = -1, Closer new_closer = nullptr)
    {
        if (id >= 0 && closer) closer(id);
        id = new_id;
        closer = new_closer;
    }

    explicit operator bool() const noexcept { return id >= 0; }
};

bool parse_latest_wire_image(const std::vector<uint8_t>& image,
                             ParsedLatestWireImage& parsed)
{
    if (image.size() < IMAGE_HEADER_BYTES + sizeof(uint64_t)) return false;

    std::memcpy(&parsed.header, image.data(), sizeof(parsed.header));

    uint64_t attr_len = 0;
    std::memcpy(&attr_len, image.data() + IMAGE_HEADER_BYTES, sizeof(uint64_t));
    const size_t attr_off = IMAGE_HEADER_BYTES + sizeof(uint64_t);
    // MEDIUM #13: overflow-safe bound check.
    if (image.size() < attr_off) return false;
    if (attr_len > image.size() - attr_off) return false;

    const size_t pixel_off = attr_off + static_cast<size_t>(attr_len);
    parsed.attributes.assign(reinterpret_cast<const char*>(image.data() + attr_off),
                             static_cast<size_t>(attr_len));
    parsed.header.attribute_string_len = static_cast<uint32_t>(parsed.attributes.size());
    parsed.pixel_data = image.data() + pixel_off;
    parsed.pixel_bytes = image.size() - pixel_off;
    return true;
}

bool append_latest_wire_image(MrdSink& sink, const ParsedLatestWireImage& image)
{
    sink.append_image(kImageVarName, image.header,
                      image.attributes.data(), image.attributes.size(),
                      image.pixel_data, image.pixel_bytes);
    return true;
}

size_t image_data_type_bytes(uint16_t data_type)
{
    switch (data_type) {
    case ISMRMRD::ISMRMRD_USHORT: return sizeof(uint16_t);
    case ISMRMRD::ISMRMRD_SHORT: return sizeof(int16_t);
    case ISMRMRD::ISMRMRD_UINT: return sizeof(uint32_t);
    case ISMRMRD::ISMRMRD_INT: return sizeof(int32_t);
    case ISMRMRD::ISMRMRD_FLOAT: return sizeof(float);
    case ISMRMRD::ISMRMRD_DOUBLE: return sizeof(double);
    case ISMRMRD::ISMRMRD_CXFLOAT: return sizeof(complex_float_t);
    case ISMRMRD::ISMRMRD_CXDOUBLE: return sizeof(complex_double_t);
    default: return 0;
    }
}

bool can_bulk_write_latest_images(const std::vector<ParsedLatestWireImage>& images)
{
    if (images.empty()) return false;

    const auto& first = images.front().header;
    const auto pixel_bytes = images.front().pixel_bytes;
    const auto element_bytes = image_data_type_bytes(first.data_type);
    if (element_bytes == 0) return false;

    const size_t expected_pixel_bytes = static_cast<size_t>(first.channels)
                                      * static_cast<size_t>(first.matrix_size[2])
                                      * static_cast<size_t>(first.matrix_size[1])
                                      * static_cast<size_t>(first.matrix_size[0])
                                      * element_bytes;
    if (expected_pixel_bytes != pixel_bytes) return false;

    for (const auto& image : images) {
        const auto& hdr = image.header;
        if (hdr.data_type != first.data_type
            || hdr.channels != first.channels
            || hdr.matrix_size[0] != first.matrix_size[0]
            || hdr.matrix_size[1] != first.matrix_size[1]
            || hdr.matrix_size[2] != first.matrix_size[2]
            || image.pixel_bytes != pixel_bytes) {
            return false;
        }
    }

    return true;
}

hid_t make_hdf5_image_header_type()
{
    hid_t datatype = H5Tcreate(H5T_COMPOUND, sizeof(ISMRMRD::ISMRMRD_ImageHeader));
    if (datatype < 0) return datatype;

    herr_t status = 0;
    hsize_t arraydims[1];
    ScopedH5Handle vartype;

    status = H5Tinsert(datatype, "version", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, version), H5T_NATIVE_UINT16);
    status = status < 0 ? status : H5Tinsert(datatype, "data_type", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, data_type), H5T_NATIVE_UINT16);
    status = status < 0 ? status : H5Tinsert(datatype, "flags", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, flags), H5T_NATIVE_UINT64);
    status = status < 0 ? status : H5Tinsert(datatype, "measurement_uid", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, measurement_uid), H5T_NATIVE_UINT32);

    arraydims[0] = 3;
    vartype.reset(H5Tarray_create2(H5T_NATIVE_UINT16, 1, arraydims), H5Tclose);
    status = status < 0 ? status : H5Tinsert(datatype, "matrix_size", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, matrix_size), vartype.id);
    vartype.reset(H5Tarray_create2(H5T_NATIVE_FLOAT, 1, arraydims), H5Tclose);
    status = status < 0 ? status : H5Tinsert(datatype, "field_of_view", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, field_of_view), vartype.id);
    status = status < 0 ? status : H5Tinsert(datatype, "channels", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, channels), H5T_NATIVE_UINT16);
    status = status < 0 ? status : H5Tinsert(datatype, "position", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, position), vartype.id);
    status = status < 0 ? status : H5Tinsert(datatype, "read_dir", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, read_dir), vartype.id);
    status = status < 0 ? status : H5Tinsert(datatype, "phase_dir", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, phase_dir), vartype.id);
    status = status < 0 ? status : H5Tinsert(datatype, "slice_dir", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, slice_dir), vartype.id);
    status = status < 0 ? status : H5Tinsert(datatype, "patient_table_position", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, patient_table_position), vartype.id);
    vartype.reset();

    status = status < 0 ? status : H5Tinsert(datatype, "average", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, average), H5T_NATIVE_UINT16);
    status = status < 0 ? status : H5Tinsert(datatype, "slice", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, slice), H5T_NATIVE_UINT16);
    status = status < 0 ? status : H5Tinsert(datatype, "contrast", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, contrast), H5T_NATIVE_UINT16);
    status = status < 0 ? status : H5Tinsert(datatype, "phase", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, phase), H5T_NATIVE_UINT16);
    status = status < 0 ? status : H5Tinsert(datatype, "repetition", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, repetition), H5T_NATIVE_UINT16);
    status = status < 0 ? status : H5Tinsert(datatype, "set", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, set), H5T_NATIVE_UINT16);
    status = status < 0 ? status : H5Tinsert(datatype, "acquisition_time_stamp", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, acquisition_time_stamp), H5T_NATIVE_UINT32);

    arraydims[0] = ISMRMRD::ISMRMRD_PHYS_STAMPS;
    vartype.reset(H5Tarray_create2(H5T_NATIVE_UINT32, 1, arraydims), H5Tclose);
    status = status < 0 ? status : H5Tinsert(datatype, "physiology_time_stamp", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, physiology_time_stamp), vartype.id);
    vartype.reset();

    status = status < 0 ? status : H5Tinsert(datatype, "image_type", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, image_type), H5T_NATIVE_UINT16);
    status = status < 0 ? status : H5Tinsert(datatype, "image_index", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, image_index), H5T_NATIVE_UINT16);
    status = status < 0 ? status : H5Tinsert(datatype, "image_series_index", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, image_series_index), H5T_NATIVE_UINT16);

    arraydims[0] = ISMRMRD::ISMRMRD_USER_INTS;
    vartype.reset(H5Tarray_create2(H5T_NATIVE_INT32, 1, arraydims), H5Tclose);
    status = status < 0 ? status : H5Tinsert(datatype, "user_int", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, user_int), vartype.id);
    vartype.reset();

    arraydims[0] = ISMRMRD::ISMRMRD_USER_FLOATS;
    vartype.reset(H5Tarray_create2(H5T_NATIVE_FLOAT, 1, arraydims), H5Tclose);
    status = status < 0 ? status : H5Tinsert(datatype, "user_float", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, user_float), vartype.id);
    vartype.reset();

    status = status < 0 ? status : H5Tinsert(datatype, "attribute_string_len", HOFFSET(ISMRMRD::ISMRMRD_ImageHeader, attribute_string_len), H5T_NATIVE_UINT32);
    if (status < 0) {
        H5Tclose(datatype);
        return -1;
    }

    return datatype;
}

hid_t make_hdf5_image_attribute_string_type()
{
    hid_t datatype = H5Tcopy(H5T_C_S1);
    if (datatype < 0) return datatype;
    if (H5Tset_size(datatype, H5T_VARIABLE) < 0) {
        H5Tclose(datatype);
        return -1;
    }
    return datatype;
}

hid_t make_hdf5_ndarray_type(uint16_t data_type)
{
    switch (data_type) {
    case ISMRMRD::ISMRMRD_USHORT:
        return H5Tcopy(H5T_NATIVE_UINT16);
    case ISMRMRD::ISMRMRD_SHORT:
        return H5Tcopy(H5T_NATIVE_INT16);
    case ISMRMRD::ISMRMRD_UINT:
        return H5Tcopy(H5T_NATIVE_UINT32);
    case ISMRMRD::ISMRMRD_INT:
        return H5Tcopy(H5T_NATIVE_INT32);
    case ISMRMRD::ISMRMRD_FLOAT:
        return H5Tcopy(H5T_NATIVE_FLOAT);
    case ISMRMRD::ISMRMRD_DOUBLE:
        return H5Tcopy(H5T_NATIVE_DOUBLE);
    case ISMRMRD::ISMRMRD_CXFLOAT: {
        hid_t datatype = H5Tcreate(H5T_COMPOUND, sizeof(complex_float_t));
        if (datatype < 0) return datatype;
        if (H5Tinsert(datatype, "real", 0, H5T_NATIVE_FLOAT) < 0
            || H5Tinsert(datatype, "imag", sizeof(float), H5T_NATIVE_FLOAT) < 0) {
            H5Tclose(datatype);
            return -1;
        }
        return datatype;
    }
    case ISMRMRD::ISMRMRD_CXDOUBLE: {
        hid_t datatype = H5Tcreate(H5T_COMPOUND, sizeof(complex_double_t));
        if (datatype < 0) return datatype;
        if (H5Tinsert(datatype, "real", 0, H5T_NATIVE_DOUBLE) < 0
            || H5Tinsert(datatype, "imag", sizeof(double), H5T_NATIVE_DOUBLE) < 0) {
            H5Tclose(datatype);
            return -1;
        }
        return datatype;
    }
    default:
        return -1;
    }
}

void write_latest_image_h5_file_append(const std::filesystem::path& path,
                                       const std::string& xml,
                                       const std::vector<std::vector<uint8_t>>& images)
{
    MrdSink sink(path);
    if (!xml.empty()) sink.set_header(xml);
    for (const auto& image : images) {
        ParsedLatestWireImage parsed;
        if (!parse_latest_wire_image(image, parsed)) {
            LOG_WARN("Skipping malformed image while writing latest H5");
            continue;
        }
        append_latest_wire_image(sink, parsed);
    }
    sink.close();
}

bool write_latest_image_h5_file_bulk(const std::filesystem::path& path,
                                     const std::string& xml,
                                     const std::vector<std::vector<uint8_t>>& images)
{
    std::vector<ParsedLatestWireImage> parsed_images;
    parsed_images.reserve(images.size());
    for (const auto& image : images) {
        ParsedLatestWireImage parsed;
        if (!parse_latest_wire_image(image, parsed)) {
            LOG_WARN("Skipping malformed image while writing latest H5");
            continue;
        }
        parsed_images.push_back(std::move(parsed));
    }

    if (!can_bulk_write_latest_images(parsed_images)) return false;

    {
        MrdSink sink(path);
        if (!xml.empty()) sink.set_header(xml);
        sink.close();
    }

    ScopedH5Handle file(H5Fopen(path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT), H5Fclose);
    if (!file) throw std::runtime_error("H5Fopen failed for latest H5 bulk write");

    ScopedH5Handle dataset_group(H5Gopen2(file.id, kDatasetGroup, H5P_DEFAULT), H5Gclose);
    if (!dataset_group) {
        dataset_group.reset(H5Gcreate2(file.id, kDatasetGroup, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
    }
    if (!dataset_group) throw std::runtime_error("Failed to open /dataset group for latest H5 bulk write");

    if (H5Lexists(dataset_group.id, kImageVarName, H5P_DEFAULT) > 0) {
        if (H5Ldelete(dataset_group.id, kImageVarName, H5P_DEFAULT) < 0) {
            throw std::runtime_error("Failed to replace existing image_0 group in latest H5 bulk write");
        }
    }

    ScopedH5Handle image_group(H5Gcreate2(dataset_group.id, kImageVarName, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
    if (!image_group) throw std::runtime_error("Failed to create /dataset/image_0 group for latest H5 bulk write");

    std::vector<ISMRMRD::ImageHeader> headers;
    headers.reserve(parsed_images.size());
    std::vector<std::string> attributes;
    attributes.reserve(parsed_images.size());
    std::vector<char*> attribute_ptrs;
    attribute_ptrs.reserve(parsed_images.size());

    const size_t per_image_bytes = parsed_images.front().pixel_bytes;
    // MEDIUM #17: checked multiplication for the aggregate buffer. Prior
    // code computed per_image_bytes * count unchecked; an overflowed
    // product would resize to a tiny buffer and the following memcpys
    // would overrun it.
    size_t aggregate_bytes = 0;
    if (!checked_mul(per_image_bytes, parsed_images.size(), aggregate_bytes)) {
        throw std::runtime_error("Latest H5 bulk write: pixel buffer size overflow");
    }
    std::vector<uint8_t> pixel_bytes;
    pixel_bytes.resize(aggregate_bytes);

    size_t pixel_offset = 0;
    for (const auto& image : parsed_images) {
        headers.push_back(image.header);
        attributes.push_back(image.attributes);
        std::memcpy(pixel_bytes.data() + pixel_offset, image.pixel_data, image.pixel_bytes);
        pixel_offset += image.pixel_bytes;
    }
    for (auto& attr : attributes) {
        attribute_ptrs.push_back(attr.empty() ? const_cast<char*>("") : attr.data());
    }

    ScopedH5Handle header_type(make_hdf5_image_header_type(), H5Tclose);
    ScopedH5Handle attr_type(make_hdf5_image_attribute_string_type(), H5Tclose);
    ScopedH5Handle data_type(make_hdf5_ndarray_type(parsed_images.front().header.data_type), H5Tclose);
    if (!header_type || !attr_type || !data_type) {
        throw std::runtime_error("Failed to create HDF5 datatype for latest H5 bulk write");
    }

    hsize_t header_dims[1] = {static_cast<hsize_t>(headers.size())};
    ScopedH5Handle header_space(H5Screate_simple(1, header_dims, nullptr), H5Sclose);
    ScopedH5Handle header_dataset(H5Dcreate2(image_group.id, "header", header_type.id, header_space.id,
                                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
    if (!header_space || !header_dataset) throw std::runtime_error("Failed to create latest H5 header dataset");
    if (H5Dwrite(header_dataset.id, header_type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, headers.data()) < 0) {
        throw std::runtime_error("Failed to write latest H5 header dataset");
    }

    ScopedH5Handle attr_space(H5Screate_simple(1, header_dims, nullptr), H5Sclose);
    ScopedH5Handle attr_dataset(H5Dcreate2(image_group.id, "attributes", attr_type.id, attr_space.id,
                                           H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
    if (!attr_space || !attr_dataset) throw std::runtime_error("Failed to create latest H5 attributes dataset");
    if (H5Dwrite(attr_dataset.id, attr_type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, attribute_ptrs.data()) < 0) {
        throw std::runtime_error("Failed to write latest H5 attributes dataset");
    }

    const auto& first = parsed_images.front().header;
    hsize_t data_dims[5] = {
        static_cast<hsize_t>(parsed_images.size()),
        static_cast<hsize_t>(first.channels),
        static_cast<hsize_t>(first.matrix_size[2]),
        static_cast<hsize_t>(first.matrix_size[1]),
        static_cast<hsize_t>(first.matrix_size[0])
    };
    ScopedH5Handle data_space(H5Screate_simple(5, data_dims, nullptr), H5Sclose);
    ScopedH5Handle data_dataset(H5Dcreate2(image_group.id, "data", data_type.id, data_space.id,
                                           H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
    if (!data_space || !data_dataset) throw std::runtime_error("Failed to create latest H5 data dataset");
    if (H5Dwrite(data_dataset.id, data_type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, pixel_bytes.data()) < 0) {
        throw std::runtime_error("Failed to write latest H5 data dataset");
    }

    return true;
}

} // namespace

void write_latest_image_h5_file(const std::filesystem::path& dest,
                                const std::string& xml,
                                const std::vector<std::vector<uint8_t>>& images)
{
    if (images.empty()) return;

    auto tmp = dest;
    tmp += ".tmp";

    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    ec.clear();
    std::filesystem::remove(tmp, ec);

    if (!write_latest_image_h5_file_bulk(tmp, xml, images)) {
        write_latest_image_h5_file_append(tmp, xml, images);
    }

    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        std::filesystem::remove(dest, ec);
        ec.clear();
        std::filesystem::rename(tmp, dest, ec);
        if (ec) throw std::runtime_error("rename latest H5 failed: " + ec.message());
    }
}

struct LatestImageWriter::Job {
    std::filesystem::path dest;
    std::string xml;
    std::vector<std::vector<uint8_t>> images;
    Completion completion;
};

struct LatestImageWriter::Impl {
    // MEDIUM #14: bound the queue. Latest-image publication is naturally
    // coalescible: a newer job for the same destination supersedes any
    // older pending job for that destination. If the cap is hit and no
    // coalesce opportunity exists, drop the oldest and log once.
    static constexpr size_t kMaxQueuedJobs = 64;

    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Job> jobs;
    bool stopping{false};
    std::atomic<bool> drop_logged{false};
    std::atomic<uint64_t> dropped_count{0};
    std::thread worker;

    Impl()
        : worker([this] { run(); })
    {}

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            stopping = true;
        }
        cv.notify_one();
        if (worker.joinable()) worker.join();
    }

    void enqueue(Job job)
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            // Coalesce: if any pending job already targets this dest, replace
            // its payload with the newer one. This keeps latency bounded
            // and matches "publish the newest snapshot" semantics.
            for (auto& pending : jobs) {
                if (pending.dest == job.dest) {
                    pending.xml = std::move(job.xml);
                    pending.images = std::move(job.images);
                    pending.completion = std::move(job.completion);
                    cv.notify_one();
                    return;
                }
            }
            if (jobs.size() >= kMaxQueuedJobs) {
                // No coalesce opportunity and queue full: drop the oldest.
                jobs.pop_front();
                dropped_count.fetch_add(1);
                if (!drop_logged.exchange(true)) {
                    LOG_WARN("LatestImageWriter queue exceeded " << kMaxQueuedJobs
                             << " jobs; dropping oldest pending");
                }
            }
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
    }

    void run()
    {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [&] { return stopping || !jobs.empty(); });
                if (stopping && jobs.empty()) break;
                job = std::move(jobs.front());
                jobs.pop_front();
            }

            try {
                write_latest_image_h5_file(job.dest, job.xml, job.images);
                if (job.completion) job.completion(job.dest);
            } catch (const std::exception& e) {
                LOG_WARN("Latest H5 write failed: " << e.what());
            }
        }
    }
};

LatestImageWriter::LatestImageWriter()
    : impl_(std::make_unique<Impl>())
{}

LatestImageWriter::~LatestImageWriter() = default;

void LatestImageWriter::enqueue(std::filesystem::path dest,
                                std::string xml,
                                std::vector<std::vector<uint8_t>> images,
                                Completion completion)
{
    impl_->enqueue(Job{std::move(dest), std::move(xml), std::move(images),
                       std::move(completion)});
}

} // namespace mrd
