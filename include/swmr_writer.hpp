#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <hdf5.h>

#include <nlohmann/json.hpp>

#include "marshal_state.hpp"

namespace mrd
{

enum class ElementType
{
    Float32,
    UInt16
};

struct StreamDimensions
{
    std::array<hsize_t, 3> spatial{}; // {x, y, z}
    hsize_t channels{1};
};

struct SwmrFrameResult
{
    std::filesystem::path file_path;
    std::string stream_id;
    std::string timestamp;
    uint64_t frame_index{0};
    size_t bytes{0};
    ElementType element_type{ElementType::Float32};
    StreamDimensions dims;
};

class SwmrFile
{
  public:
    SwmrFile(const std::filesystem::path &path,
             const std::string &stream_id,
             ElementType type,
             const StreamDimensions &dims,
             std::string header_xml);
    ~SwmrFile();

    SwmrFile(const SwmrFile &) = delete;
    SwmrFile &operator=(const SwmrFile &) = delete;

    SwmrFrameResult append_frame(const void *data, size_t bytes);

    const std::filesystem::path &path() const noexcept { return path_; }
    ElementType element_type() const noexcept { return type_; }
    const StreamDimensions &dims() const noexcept { return dims_; }
    uint64_t frame_count() const noexcept { return frames_; }
    size_t frame_bytes() const noexcept { return frame_bytes_; }

  private:
    std::filesystem::path path_;
    std::string stream_id_;
    ElementType type_;
    StreamDimensions dims_;
    std::string header_xml_;
    hid_t file_{-1};
    hid_t dataset_{-1};
    size_t frame_bytes_{0};
    uint64_t frames_{0};
    std::mutex write_mutex_;

    void open();
    void close();
};

class SwmrManager
{
  public:
    explicit SwmrManager(MarshalState &state);

    SwmrManager(const SwmrManager &) = delete;
    SwmrManager &operator=(const SwmrManager &) = delete;

    SwmrFrameResult append_frame(const std::string &stream_id,
                                 const StreamDimensions &dims,
                                 ElementType type,
                                 std::string_view header_xml,
                                 const void *data,
                                 size_t bytes);

  private:
    struct StreamState
    {
        std::unique_ptr<SwmrFile> file;
        std::mutex mutex;
        StreamDimensions dims;
        ElementType type{ElementType::Float32};
        std::string header_xml;
    };

    MarshalState &state_;
    std::mutex map_mutex_;
    std::unordered_map<std::string, std::shared_ptr<StreamState>> streams_;

    std::shared_ptr<StreamState> ensure_stream(const std::string &stream_id,
                                               const StreamDimensions &dims,
                                               ElementType type,
                                               const std::filesystem::path &sink_root,
                                               std::string_view header_xml);
    static std::string sanitize_stream(const std::string &stream_id);
    static std::string element_type_string(ElementType t);
    nlohmann::json make_entry_json(const SwmrFrameResult &result) const;
};

ElementType parse_element_type(const std::string &value);
std::string element_type_to_string(ElementType type);
size_t element_type_bytes(ElementType type);
ElementType element_type_from_ismrmrd(uint16_t data_type);
std::string default_ismrmrd_header(const StreamDimensions &dims, ElementType type, std::string_view stream_id);

} // namespace mrd
