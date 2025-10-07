#include "swmr_writer.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <stdexcept>

#include "mrd_io.hpp"

namespace mrd
{
namespace
{
size_t element_size(ElementType type)
{
    switch (type)
    {
    case ElementType::Float32:
        return sizeof(float);
    case ElementType::UInt16:
        return sizeof(uint16_t);
    }
    throw std::runtime_error("unsupported element type");
}

hid_t element_hdf_type(ElementType type)
{
    switch (type)
    {
    case ElementType::Float32:
        return H5T_IEEE_F32LE;
    case ElementType::UInt16:
        return H5T_STD_U16LE;
    }
    throw std::runtime_error("unsupported element type");
}

} // namespace

SwmrFile::SwmrFile(const std::filesystem::path &path,
                   const std::string &stream_id,
                   ElementType type,
                   const StreamDimensions &dims)
    : path_(path), stream_id_(stream_id), type_(type), dims_(dims)
{
    if (dims_.spatial[0] == 0 || dims_.spatial[1] == 0 || dims_.channels == 0)
        throw std::runtime_error("invalid SWMR dimensions");
    open();
}

SwmrFile::~SwmrFile()
{
    close();
}

void SwmrFile::open()
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

    const hsize_t z = dims_.spatial[2] ? dims_.spatial[2] : 1;
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

    dataset_ = H5Dcreate2(file_, "/frames", element_hdf_type(type_), space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
    H5Pclose(dcpl);
    H5Sclose(space);
    if (dataset_ < 0)
    {
        close();
        throw std::runtime_error("H5Dcreate2 failed");
    }

    // Store simple metadata so readers know the layout
    unsigned long long dims_attr[3] = {
        static_cast<unsigned long long>(dims_.spatial[0]),
        static_cast<unsigned long long>(dims_.spatial[1]),
        static_cast<unsigned long long>(z)};
    hsize_t attr_dims[1] = {3};
    hid_t attr_space = H5Screate_simple(1, attr_dims, nullptr);
    hid_t attr = H5Acreate2(dataset_, "spatial_dims", H5T_STD_U64LE, attr_space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr >= 0)
    {
        H5Awrite(attr, H5T_NATIVE_ULLONG, dims_attr);
        H5Aclose(attr);
    }
    H5Sclose(attr_space);

    attr_space = H5Screate(H5S_SCALAR);
    attr = H5Acreate2(dataset_, "channels", H5T_STD_U64LE, attr_space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr >= 0)
    {
        unsigned long long ch = static_cast<unsigned long long>(dims_.channels);
        H5Awrite(attr, H5T_NATIVE_ULLONG, &ch);
        H5Aclose(attr);
    }
    H5Sclose(attr_space);

    attr_space = H5Screate(H5S_SCALAR);
    hid_t str_type = H5Tcopy(H5T_C_S1);
    auto et = element_type_to_string(type_);
    H5Tset_size(str_type, et.size() + 1);
    H5Tset_strpad(str_type, H5T_STR_NULLTERM);
    attr = H5Acreate2(dataset_, "element_type", str_type, attr_space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr >= 0)
    {
        H5Awrite(attr, str_type, et.c_str());
        H5Aclose(attr);
    }
    H5Tclose(str_type);
    H5Sclose(attr_space);

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

void SwmrFile::close()
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

size_t SwmrFile::expected_bytes() const
{
    return static_cast<size_t>(dims_.spatial[0]) * static_cast<size_t>(dims_.spatial[1]) *
           static_cast<size_t>(dims_.spatial[2] ? dims_.spatial[2] : 1) *
           static_cast<size_t>(dims_.channels) * element_size(type_);
}

SwmrFrameResult SwmrFile::append_frame(const void *data, size_t bytes)
{
    std::lock_guard<std::mutex> guard(write_mutex_);
    const size_t need = expected_bytes();
    if (bytes != need)
        throw std::runtime_error("frame payload size mismatch");

    hsize_t new_dims[5] = {frames_ + 1, dims_.channels, dims_.spatial[2] ? dims_.spatial[2] : 1,
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

    if (H5Dflush(dataset_) < 0 || H5Fflush(file_, H5F_SCOPE_GLOBAL) < 0)
        throw std::runtime_error("H5 flush failed");

    SwmrFrameResult result;
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

SwmrManager::SwmrManager(MarshalState &state)
    : state_(state)
{
}

std::shared_ptr<SwmrManager::StreamState> SwmrManager::ensure_stream(const std::string &stream_id,
                                                                      const StreamDimensions &dims,
                                                                      ElementType type,
                                                                      const std::filesystem::path &sink_root)
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

    auto clean = sanitize_stream(stream_id);
    const std::string ts = iso8601_now_ms();
    std::filesystem::path file_path = sink_root / (ts + "_" + clean + ".mrd");

    auto state = std::make_shared<StreamState>();
    state->dims = dims;
    state->type = type;
    state->file = std::make_unique<SwmrFile>(file_path, stream_id, type, dims);

    streams_.emplace(stream_id, state);
    return state;
}

SwmrFrameResult SwmrManager::append_frame(const std::string &stream_id,
                                          const StreamDimensions &dims,
                                          ElementType type,
                                          const void *data,
                                          size_t bytes)
{
    auto sink = resolve_sink_paths(state_);
    auto stream_state = ensure_stream(stream_id, dims, type, sink.sink_root);

    std::lock_guard<std::mutex> guard(stream_state->mutex);
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
        state_.ws_emit_topic(entry.dump(), "mrd.swmr");
    }
    catch (...)
    {
    }

    return result;
}

std::string SwmrManager::sanitize_stream(const std::string &stream_id)
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
        out = "stream";
    return out;
}

std::string SwmrManager::element_type_string(ElementType t)
{
    return element_type_to_string(t);
}

nlohmann::json SwmrManager::make_entry_json(const SwmrFrameResult &result) const
{
    nlohmann::json dims_json = {
        {"x", result.dims.spatial[0]},
        {"y", result.dims.spatial[1]},
        {"z", result.dims.spatial[2] ? result.dims.spatial[2] : 1},
        {"channels", result.dims.channels}};

    return {
        {"type", "mrd.swmr"},
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
    if (value == "uint16" || value == "u16")
        return ElementType::UInt16;
    throw std::runtime_error("unsupported element type: " + value);
}

std::string element_type_to_string(ElementType type)
{
    switch (type)
    {
    case ElementType::Float32:
        return "float32";
    case ElementType::UInt16:
        return "uint16";
    }
    return "unknown";
}

} // namespace mrd
