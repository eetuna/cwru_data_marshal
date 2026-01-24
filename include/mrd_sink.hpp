#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <hdf5.h>

#include <nlohmann/json.hpp>

#include "marshal_state.hpp"

namespace mrd
{

enum class ElementType
{
    Float32,
    Int16,
    UInt16,
    ComplexFloat32
};

struct ImageDimensions
{
    std::array<hsize_t, 3> spatial{}; // {x, y, z}
    hsize_t channels{1};
};

struct FrameAppendResult
{
    std::filesystem::path file_path;
    std::string stream_id;
    std::string timestamp;
    uint64_t frame_index{0};
    size_t bytes{0};
    ElementType element_type{ElementType::Float32};
    ImageDimensions dims;
    bool flushed{true};
};

struct FrameReadResult
{
    std::vector<uint8_t> data;
    std::string stream_id;
    uint64_t frame_index{0};
    uint64_t total_frames{0};
    ElementType element_type{ElementType::Float32};
    ImageDimensions dims;
    bool success{false};
};

class MrdFile
{
  public:
    MrdFile(const std::filesystem::path &path,
            const std::string &stream_id,
            ElementType type,
            const ImageDimensions &dims,
            std::string header_xml,
            FlushPolicy flush_policy = {});
    ~MrdFile();

    MrdFile(const MrdFile &) = delete;
    MrdFile &operator=(const MrdFile &) = delete;

    FrameAppendResult append_frame(const void *data, size_t bytes);
    void set_flush_policy(FlushPolicy policy);
    void flush();

    const std::filesystem::path &path() const noexcept { return path_; }
    ElementType element_type() const noexcept { return type_; }
    const ImageDimensions &dims() const noexcept { return dims_; }
    uint64_t frame_count() const noexcept { return frames_; }
    size_t frame_bytes() const noexcept { return frame_bytes_; }

  private:
    std::filesystem::path path_;
    std::string stream_id_;
    ElementType type_;
    ImageDimensions dims_;
    std::string header_xml_;
    hid_t file_{-1};
    hid_t dataset_{-1};
    size_t frame_bytes_{0};
    uint64_t frames_{0};
    std::mutex write_mutex_;
    FlushPolicy flush_policy_{};
    size_t frames_since_flush_{0};
    std::chrono::steady_clock::time_point last_flush_;

    void open();
    void close();
    bool perform_flush(bool force);
};

class MrdSink
{
  public:
    explicit MrdSink(MarshalState &state);

    MrdSink(const MrdSink &) = delete;
    MrdSink &operator=(const MrdSink &) = delete;

    FrameAppendResult append_frame(const std::string &stream_id,
                                   const ImageDimensions &dims,
                                   ElementType type,
                                   std::string_view header_xml,
                                   const void *data,
                                   size_t bytes,
                                   std::string_view session_token = {});

    // Read frame from SWMR file (safe while writer is writing)
    // frame_index < 0 means latest frame
    FrameReadResult read_frame(const std::filesystem::path &mrd_path, int64_t frame_index = -1);

    void cleanup_idle_streams(std::chrono::seconds idle_timeout = std::chrono::seconds(600));
    void flush_all();

  private:
    struct StreamState
    {
        std::unique_ptr<MrdFile> file;
        std::mutex mutex;
        ImageDimensions dims;
        ElementType type{ElementType::Float32};
        std::string header_xml;
        std::string canonical_name;
        std::filesystem::path sink_root;
        size_t generation{0};
        FlushPolicy flush_policy;
        std::string active_session;
        std::chrono::steady_clock::time_point last_accessed{std::chrono::steady_clock::now()};
    };

    MarshalState &state_;
    std::mutex map_mutex_;
    std::unordered_map<std::string, std::shared_ptr<StreamState>> streams_;

    std::shared_ptr<StreamState> ensure_stream(const std::string &stream_id,
                                               const ImageDimensions &dims,
                                               ElementType type,
                                               const std::filesystem::path &sink_root,
                                               std::string_view header_xml,
                                               std::string_view session_token);
    static std::string canonical_scan_name(const std::string &stream_id);
    static std::string element_type_string(ElementType t);
    nlohmann::json make_entry_json(const FrameAppendResult &result) const;
};

ElementType parse_element_type(const std::string &value);
std::string element_type_to_string(ElementType type);
size_t element_type_bytes(ElementType type);
ElementType element_type_from_ismrmrd(uint16_t data_type);
std::string default_ismrmrd_header(const ImageDimensions &dims, ElementType type, std::string_view stream_id);

} // namespace mrd
