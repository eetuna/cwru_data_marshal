#include "mrd_sink.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "mrd_io.hpp"
#include <ismrmrd/ismrmrd.h>

namespace mrd
{
namespace
{
hid_t element_hdf_type(ElementType type)
{
    switch (type)
    {
    case ElementType::Float32:
        return H5T_IEEE_F32LE;
    case ElementType::Int16:
        return H5T_STD_I16LE;
    case ElementType::UInt16:
        return H5T_STD_U16LE;
    case ElementType::ComplexFloat32:
    {
        static hid_t complex_type = [] {
            hid_t t = H5Tcreate(H5T_COMPOUND, sizeof(float) * 2);
            if (t < 0)
                throw std::runtime_error("H5Tcreate complex32 failed");
            if (H5Tinsert(t, "r", 0, H5T_IEEE_F32LE) < 0)
            {
                H5Tclose(t);
                throw std::runtime_error("H5Tinsert complex32 real failed");
            }
            if (H5Tinsert(t, "i", sizeof(float), H5T_IEEE_F32LE) < 0)
            {
                H5Tclose(t);
                throw std::runtime_error("H5Tinsert complex32 imag failed");
            }
            return t;
        }();
        return complex_type;
    }
    }
    throw std::runtime_error("unsupported element type");
}

} // namespace

MrdFile::MrdFile(const std::filesystem::path &path,
                 const std::string &stream_id,
                 ElementType type,
                 const ImageDimensions &dims,
                 std::string header_xml)
    : path_(path), stream_id_(stream_id), type_(type), dims_(dims), header_xml_(std::move(header_xml))
{
    if (dims_.spatial[0] == 0 || dims_.spatial[1] == 0 || dims_.channels == 0)
        throw std::runtime_error("invalid MRD dimensions");
    if (dims_.spatial[2] == 0)
        dims_.spatial[2] = 1;
    frame_bytes_ = element_type_bytes(type_) * static_cast<size_t>(dims_.spatial[0]) *
                   static_cast<size_t>(dims_.spatial[1]) * static_cast<size_t>(dims_.spatial[2]) *
                   static_cast<size_t>(dims_.channels);
    open();
}

MrdFile::~MrdFile()
{
    close();
}

void MrdFile::open()
{
    namespace fs = std::filesystem;
    fs::create_directories(path_.parent_path());

    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    if (fapl < 0)
        throw std::runtime_error("H5Pcreate failed");

    if (H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) < 0)
    {
        H5Pclose(fapl);
        throw std::runtime_error("H5Pset_libver_bounds failed");
    }

    if (H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI) < 0)
    {
        H5Pclose(fapl);
        throw std::runtime_error("H5Pset_fclose_degree failed");
    }

    file_ = H5Fcreate(path_.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
    H5Pclose(fapl);
    if (file_ < 0)
        throw std::runtime_error("H5Fcreate failed");

    hid_t images_group = H5Gcreate2(file_, "/images", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (images_group < 0)
    {
        close();
        throw std::runtime_error("H5Gcreate2(/images) failed");
    }
    H5Gclose(images_group);

    hid_t header_space = H5Screate(H5S_SCALAR);
    if (header_space < 0)
    {
        close();
        throw std::runtime_error("H5Screate header failed");
    }
    hid_t str_type = H5Tcopy(H5T_C_S1);
    if (str_type < 0)
    {
        H5Sclose(header_space);
        close();
        throw std::runtime_error("H5Tcopy failed");
    }
    H5Tset_size(str_type, header_xml_.size() + 1);
    H5Tset_strpad(str_type, H5T_STR_NULLTERM);
    hid_t header_dset = H5Dcreate2(file_, "/header", str_type, header_space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (header_dset < 0)
    {
        H5Tclose(str_type);
        H5Sclose(header_space);
        close();
        throw std::runtime_error("H5Dcreate2(/header) failed");
    }
    if (H5Dwrite(header_dset, str_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, header_xml_.c_str()) < 0)
    {
        H5Dclose(header_dset);
        H5Tclose(str_type);
        H5Sclose(header_space);
        close();
        throw std::runtime_error("H5Dwrite(/header) failed");
    }
    H5Dclose(header_dset);
    H5Tclose(str_type);
    H5Sclose(header_space);

    const hsize_t z = dims_.spatial[2];
    hsize_t initial_dims[5] = {0, dims_.channels, z, dims_.spatial[1], dims_.spatial[0]};
    hsize_t max_dims[5] = {H5S_UNLIMITED, dims_.channels, z, dims_.spatial[1], dims_.spatial[0]};
    hsize_t chunk[5] = {1, dims_.channels, z, dims_.spatial[1], dims_.spatial[0]};

    hid_t space = H5Screate_simple(5, initial_dims, max_dims);
    if (space < 0)
    {
        close();
        throw std::runtime_error("H5Screate_simple failed");
    }

    hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl < 0)
    {
        H5Sclose(space);
        close();
        throw std::runtime_error("H5Pcreate (dcpl) failed");
    }

    if (H5Pset_chunk(dcpl, 5, chunk) < 0 ||
        H5Pset_fill_time(dcpl, H5D_FILL_TIME_NEVER) < 0 ||
        H5Pset_alloc_time(dcpl, H5D_ALLOC_TIME_EARLY) < 0)
    {
        H5Pclose(dcpl);
        H5Sclose(space);
        close();
        throw std::runtime_error("chunk configuration failed");
    }

    dataset_ = H5Dcreate2(file_, "/images/data", element_hdf_type(type_), space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
    H5Pclose(dcpl);
    H5Sclose(space);
    if (dataset_ < 0)
    {
        close();
        throw std::runtime_error("H5Dcreate2 failed");
    }

    if (H5Fflush(file_, H5F_SCOPE_GLOBAL) < 0)
    {
        close();
        throw std::runtime_error("H5Fflush failed before SWMR start");
    }

    if (H5Fstart_swmr_write(file_) < 0)
    {
        close();
        throw std::runtime_error("H5Fstart_swmr_write failed");
    }

    H5Fflush(file_, H5F_SCOPE_GLOBAL);
}

void MrdFile::close()
{
    if (dataset_ >= 0)
    {
        H5Dclose(dataset_);
        dataset_ = -1;
    }
    if (file_ >= 0)
    {
        H5Fflush(file_, H5F_SCOPE_GLOBAL);
        H5Fclose(file_);
        file_ = -1;
    }
}

FrameAppendResult MrdFile::append_frame(const void *data, size_t bytes)
{
    std::lock_guard<std::mutex> guard(write_mutex_);
    const size_t need = frame_bytes_;
    if (bytes != need)
        throw std::runtime_error("frame payload size mismatch");

    hsize_t new_dims[5] = {frames_ + 1, dims_.channels, dims_.spatial[2],
                           dims_.spatial[1], dims_.spatial[0]};
    if (H5Dset_extent(dataset_, new_dims) < 0)
        throw std::runtime_error("H5Dset_extent failed");

    hid_t filespace = H5Dget_space(dataset_);
    if (filespace < 0)
        throw std::runtime_error("H5Dget_space failed");

    hsize_t start[5] = {frames_, 0, 0, 0, 0};
    hsize_t count[5] = {1, dims_.channels, new_dims[2], new_dims[3], new_dims[4]};
    if (H5Sselect_hyperslab(filespace, H5S_SELECT_SET, start, nullptr, count, nullptr) < 0)
    {
        H5Sclose(filespace);
        throw std::runtime_error("H5Sselect_hyperslab failed");
    }

    hid_t memspace = H5Screate_simple(5, count, nullptr);
    if (memspace < 0)
    {
        H5Sclose(filespace);
        throw std::runtime_error("H5Screate_simple (mem) failed");
    }

    if (H5Dwrite(dataset_, element_hdf_type(type_), memspace, filespace, H5P_DEFAULT, data) < 0)
    {
        H5Sclose(memspace);
        H5Sclose(filespace);
        throw std::runtime_error("H5Dwrite failed");
    }

    H5Sclose(memspace);
    H5Sclose(filespace);

    if (H5Dflush(dataset_) < 0)
        throw std::runtime_error("H5Dflush failed");
    if (H5Fflush(file_, H5F_SCOPE_GLOBAL) < 0)
        throw std::runtime_error("H5Fflush failed");

    FrameAppendResult result;
    result.file_path = path_;
    result.stream_id = stream_id_;
    result.frame_index = frames_;
    result.bytes = bytes;
    result.element_type = type_;
    result.dims = dims_;
    result.timestamp = iso8601_now_ms();

    frames_++;
    return result;
}

MrdSink::MrdSink(MarshalState &state)
    : state_(state)
{
}

std::shared_ptr<MrdSink::StreamState> MrdSink::ensure_stream(const std::string &stream_id,
                                                             const ImageDimensions &dims,
                                                             ElementType type,
                                                             const std::filesystem::path &sink_root,
                                                             std::string_view header_xml)
{
    std::lock_guard<std::mutex> guard(map_mutex_);
    auto it = streams_.find(stream_id);
    if (it != streams_.end())
    {
        auto state = it->second;
        if (state->dims.spatial != dims.spatial || state->dims.channels != dims.channels || state->type != type)
            throw std::runtime_error("stream dimensions/type mismatch");
        return state;
    }

    const std::string canonical = canonical_scan_name(stream_id);
    std::filesystem::path file_path = sink_root / (canonical + ".mrd");

    auto state = std::make_shared<StreamState>();
    state->dims = dims;
    state->type = type;
    state->header_xml = std::string(header_xml);
    state->file = std::make_unique<MrdFile>(file_path, stream_id, type, dims, state->header_xml);

    streams_.emplace(stream_id, state);
    return state;
}

FrameAppendResult MrdSink::append_frame(const std::string &stream_id,
                                        const ImageDimensions &dims,
                                        ElementType type,
                                        std::string_view header_xml,
                                        const void *data,
                                        size_t bytes)
{
    auto sink = resolve_sink_paths(state_);
    auto stream_state = ensure_stream(stream_id, dims, type, sink.sink_root, header_xml);

    std::lock_guard<std::mutex> guard(stream_state->mutex);
    if (!header_xml.empty() && stream_state->header_xml != header_xml)
    {
        if (stream_state->header_xml.empty())
        {
            stream_state->header_xml.assign(header_xml);
        }
        else if (stream_state->header_xml != header_xml)
        {
            throw std::runtime_error("stream header mismatch");
        }
    }

    auto result = stream_state->file->append_frame(data, bytes);

    auto seq = ingest_sequence().fetch_add(1);
    nlohmann::json entry = make_entry_json(result);
    entry["seq"] = seq;

    append_line(sink.index_root / "index.jsonl", entry.dump());
    const std::string latest = entry.dump();
    write_atomic(sink.index_root / "latest.json", latest.data(), latest.size());

    try
    {
        state_.ws_emit(entry.dump());
        state_.ws_emit_topic(entry.dump(), "mrd");
    }
    catch (...)
    {
    }

    return result;
}

std::string MrdSink::canonical_scan_name(const std::string &stream_id)
{
    std::string out;
    out.reserve(stream_id.size());
    for (char c : stream_id)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
            out.push_back(c);
        else
            out.push_back('_');
    }
    if (out.empty())
        out = "scan";
    return out;
}

std::string MrdSink::element_type_string(ElementType t)
{
    return element_type_to_string(t);
}

nlohmann::json MrdSink::make_entry_json(const FrameAppendResult &result) const
{
    nlohmann::json dims_json = {
        {"x", result.dims.spatial[0]},
        {"y", result.dims.spatial[1]},
        {"z", result.dims.spatial[2] ? result.dims.spatial[2] : 1},
        {"channels", result.dims.channels}};

    return {
        {"type", "mrd"},
        {"path", result.file_path.string()},
        {"stream", result.stream_id},
        {"ts", result.timestamp},
        {"frame_index", result.frame_index},
        {"element_type", element_type_string(result.element_type)},
        {"dims", dims_json},
        {"size_bytes", result.bytes}};
}

ElementType parse_element_type(const std::string &value)
{
    if (value == "float32" || value == "float" || value == "f32")
        return ElementType::Float32;
    if (value == "int16" || value == "i16" || value == "short")
        return ElementType::Int16;
    if (value == "uint16" || value == "u16")
        return ElementType::UInt16;
    if (value == "complex64" || value == "cfloat" || value == "cf32")
        return ElementType::ComplexFloat32;
    throw std::runtime_error("unsupported element type: " + value);
}

std::string element_type_to_string(ElementType type)
{
    switch (type)
    {
    case ElementType::Float32:
        return "float32";
    case ElementType::Int16:
        return "int16";
    case ElementType::UInt16:
        return "uint16";
    case ElementType::ComplexFloat32:
        return "complex64";
    }
    return "unknown";
}

size_t element_type_bytes(ElementType type)
{
    switch (type)
    {
    case ElementType::Float32:
        return sizeof(float);
    case ElementType::Int16:
        return sizeof(int16_t);
    case ElementType::UInt16:
        return sizeof(uint16_t);
    case ElementType::ComplexFloat32:
        return sizeof(float) * 2;
    }
    throw std::runtime_error("unsupported element type");
}

ElementType element_type_from_ismrmrd(uint16_t data_type)
{
    switch (data_type)
    {
    case ISMRMRD::ISMRMRD_DataTypes::ISMRMRD_FLOAT:
        return ElementType::Float32;
    case ISMRMRD::ISMRMRD_DataTypes::ISMRMRD_SHORT:
        return ElementType::Int16;
    case ISMRMRD::ISMRMRD_DataTypes::ISMRMRD_USHORT:
        return ElementType::UInt16;
    case ISMRMRD::ISMRMRD_DataTypes::ISMRMRD_CXFLOAT:
        return ElementType::ComplexFloat32;
    default:
        throw std::runtime_error("unsupported ISMRMRD image data_type");
    }
}

std::string default_ismrmrd_header(const ImageDimensions &dims, ElementType, std::string_view stream_id)
{
    auto safe = stream_id.empty() ? std::string("scan") : std::string(stream_id);
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << "<ismrmrdHeader xmlns=\"http://www.ismrm.org/ISMRMRD\">\n";
    oss << "  <experimentalConditions>\n";
    oss << "    <H1resonanceFrequency_Hz>123000000</H1resonanceFrequency_Hz>\n";
    oss << "  </experimentalConditions>\n";
    oss << "  <acquisitionSystemInformation>\n";
    oss << "    <systemVendor>CWRU</systemVendor>\n";
    oss << "    <systemModel>marshal</systemModel>\n";
    oss << "  </acquisitionSystemInformation>\n";
    oss << "  <encoding>\n";
    oss << "    <encodedSpace>\n";
    const auto z = dims.spatial[2] ? dims.spatial[2] : static_cast<hsize_t>(1);
    oss << "      <matrixSize><x>" << dims.spatial[0] << "</x><y>" << dims.spatial[1] << "</y><z>" << z << "</z></matrixSize>\n";
    oss << "      <fieldOfView_mm><x>1</x><y>1</y><z>1</z></fieldOfView_mm>\n";
    oss << "    </encodedSpace>\n";
    oss << "    <reconSpace>\n";
    oss << "      <matrixSize><x>" << dims.spatial[0] << "</x><y>" << dims.spatial[1] << "</y><z>" << z << "</z></matrixSize>\n";
    oss << "      <fieldOfView_mm><x>1</x><y>1</y><z>1</z></fieldOfView_mm>\n";
    oss << "    </reconSpace>\n";
    oss << "    <trajectory>cartesian</trajectory>\n";
    oss << "  </encoding>\n";
    oss << "  <measurementInformation>\n";
    oss << "    <measurementID>" << safe << "</measurementID>\n";
    oss << "  </measurementInformation>\n";
    oss << "</ismrmrdHeader>";
    return oss.str();
}

} // namespace mrd
