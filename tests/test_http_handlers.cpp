#include <catch2/catch_all.hpp>
#include <boost/beast/http.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>
#include <random>

#include "marshal_http.hpp"
#include "marshal_state.hpp"
#include "mrd_sink.hpp"

namespace fs = std::filesystem;
namespace http = boost::beast::http;
using json = nlohmann::json;

// Helper to create a unique temp directory
static std::string unique_temp_dir()
{
    auto base = fs::temp_directory_path();
    std::string name = "cwru_marshal_http_test_" + std::to_string(std::random_device{}());
    fs::path full = base / name;
    fs::create_directories(full);
    return full.string();
}

TEST_CASE("HTTP Handler: Health Check", "[http]")
{
    MarshalState state;
    state.start = std::chrono::steady_clock::now();
    state.mrd_sink = std::make_shared<mrd::MrdSink>(state);
    state.json_writer_running = true;

    http::request<http::string_body> req{http::verb::get, "/health", 11};
    auto res = handle_http_request(req, state);

    REQUIRE(res.result() == http::status::ok);
    auto body = json::parse(res.body());
    CHECK(body["status"] == "ok");
    CHECK(body["data"]["status"] == "healthy");
    CHECK(body["data"].contains("uptime_s"));
    CHECK(body["data"]["hdf5_sink"] == true);
    CHECK(body["data"]["json_writer"] == true);
    CHECK(body["data"]["json_queue_depth"] == 0);
}

TEST_CASE("HTTP Handler: Configuration", "[http]")
{
    MarshalState state;
    state.data_dir = "/tmp/fake_data_dir";

    http::request<http::string_body> req{http::verb::get, "/v1/config", 11};
    auto res = handle_http_request(req, state);

    REQUIRE(res.result() == http::status::ok);
    auto body = json::parse(res.body());
    CHECK(body["status"] == "ok");
    CHECK(body["data"]["data_dir"] == "/tmp/fake_data_dir");
}

TEST_CASE("HTTP Handler: Pose Operations", "[http]")
{
    std::string temp = unique_temp_dir();
    MarshalState state;
    state.data_dir = temp;
    state.sink_mode = SinkMode::MRD; // Needed for resolve_sink_paths
    state.ws_emit = [](const std::string &) {}; // Mock WS

    // Create necessary directories
    fs::create_directories(fs::path(temp) / "mrd");

    SECTION("Get current pose (initial empty)")
    {
        http::request<http::string_body> req{http::verb::get, "/v1/pose/current", 11};
        auto res = handle_http_request(req, state);
        // Cache is empty on startup -> 204 No Content
        REQUIRE(res.result() == http::status::no_content);
    }

    SECTION("Update pose valid")
    {
        json payload = {
            {"p", {1.0, 2.0, 3.0}},
            {"R", {1, 0, 0, 0, 1, 0, 0, 0, 1}},
            {"frame", "test_frame"},
            {"source", "test_source"}
        };
        http::request<http::string_body> req{http::verb::post, "/v1/pose/update", 11};
        req.body() = payload.dump();
        req.prepare_payload();

        auto res = handle_http_request(req, state);
        REQUIRE(res.result() == http::status::ok);
        auto body = json::parse(res.body());
        CHECK(body["status"] == "ok");
        CHECK(body["data"]["pose"]["p"][0] == 1.0);

        // Verify state update
        auto p = state.poses.get();
        CHECK(p.p[0] == 1.0);
        CHECK(p.source == "test_source");

        // Verify write was queued (async writer not running in tests)
        {
            std::lock_guard<std::mutex> lock(state.json_queue_mutex);
            CHECK(!state.json_write_queue.empty());
            CHECK(state.json_write_queue.front().type == MarshalState::WriteType::POSE);
        }
    }

    SECTION("Update pose invalid (missing fields)")
    {
        json payload = {{"p", {1.0, 2.0, 3.0}}}; // Missing R
        http::request<http::string_body> req{http::verb::post, "/v1/pose/update", 11};
        req.body() = payload.dump();
        req.prepare_payload();

        auto res = handle_http_request(req, state);
        REQUIRE(res.result() == http::status::bad_request);
        auto body = json::parse(res.body());
        CHECK(body["status"] == "error");
    }
    
    fs::remove_all(temp);
}

TEST_CASE("HTTP Handler: MRD Ingest and Retrieval", "[http][mrd]")
{
    std::string temp = unique_temp_dir();
    MarshalState state;
    state.data_dir = temp;
    state.sink_mode = SinkMode::MRD;
    state.ws_emit = [](const std::string &) {};

    fs::create_directories(fs::path(temp) / "mrd");

    SECTION("Ingest MRD file")
    {
        // Build payload with HDF5 magic signature so detect_mrd_type() recognizes it
        const uint8_t hdf5_sig[] = {0x89, 0x48, 0x44, 0x46, 0x0d, 0x0a, 0x1a, 0x0a};
        std::string content(reinterpret_cast<const char*>(hdf5_sig), sizeof(hdf5_sig));
        content += "fake hdf5 payload data";

        http::request<http::string_body> req{http::verb::post, "/v1/mrd/ingest", 11};
        req.body() = content;
        req.prepare_payload();

        auto res = handle_http_request(req, state);
        REQUIRE(res.result() == http::status::created);

        auto body = json::parse(res.body());
        CHECK(body["status"] == "ok");
        CHECK(body["data"].contains("path"));
        CHECK(body["data"].contains("ts"));

        // Verify file exists
        std::string filepath = body["data"]["path"];
        CHECK(fs::exists(filepath));

        // Verify index.jsonl
        CHECK(fs::exists(fs::path(temp) / "mrd" / "index.jsonl"));

        // Verify latest.json
        CHECK(fs::exists(fs::path(temp) / "mrd" / "latest.json"));
    }

    SECTION("Get Latest MRD")
    {
        // Handler reads from in-memory cache, not from file
        {
            std::lock_guard<std::mutex> lock(state.latest_mrd_mutex);
            state.latest_mrd_json = R"({"id": "file1.mrd"})";
        }

        http::request<http::string_body> req{http::verb::get, "/v1/mrd/latest", 11};
        auto res = handle_http_request(req, state);
        REQUIRE(res.result() == http::status::ok);
        auto body = json::parse(res.body());
        CHECK(body["data"]["id"] == "file1.mrd");
    }

    SECTION("Get Latest MRD (empty cache)")
    {
        http::request<http::string_body> req{http::verb::get, "/v1/mrd/latest", 11};
        auto res = handle_http_request(req, state);
        REQUIRE(res.result() == http::status::no_content);
    }

    SECTION("Get MRD Since")
    {
        // Create index.jsonl with some entries
        fs::path index_path = fs::path(temp) / "mrd" / "index.jsonl";
        {
            std::ofstream f(index_path);
            f << R"({"id": "1", "ts": "2023-01-01T10:00:00Z"})" << "\n";
            f << R"({"id": "2", "ts": "2023-01-01T10:01:00Z"})" << "\n";
            f << R"({"id": "3", "ts": "2023-01-01T10:02:00Z"})" << "\n";
        }
        REQUIRE(fs::exists(index_path));

        http::request<http::string_body> req{http::verb::get, "/v1/mrd/since?ts=2023-01-01T10:00:30Z", 11};
        auto res = handle_http_request(req, state);
        
        REQUIRE(res.result() == http::status::ok);
        auto body = json::parse(res.body());
        REQUIRE(body["data"].is_array());
        REQUIRE(body["data"].size() == 2);
        CHECK(body["data"][0]["id"] == "2");
        CHECK(body["data"][1]["id"] == "3");
    }

    fs::remove_all(temp);
}

TEST_CASE("HTTP Handler: ISMRMRD Frame", "[http][ismrmrd]")
{
    std::string temp = unique_temp_dir();
    MarshalState state;
    state.data_dir = temp;
    state.sink_mode = SinkMode::MRD;
    state.ws_emit = [](const std::string &) {};
    state.mrd_sink = std::make_unique<mrd::MrdSink>(state);

    fs::create_directories(fs::path(temp) / "mrd");

    mrd::ImageDimensions dims;
    dims.spatial = {2, 2, 1};
    dims.channels = 1;
    
    // Create valid ISMRMRD Image Header
    // version must be non-zero for detect_mrd_type() to recognize as IMAGE
    ISMRMRD::ImageHeader img_header;
    std::memset(&img_header, 0, sizeof(img_header));
    img_header.version = 1;
    img_header.matrix_size[0] = 2;
    img_header.matrix_size[1] = 2;
    img_header.matrix_size[2] = 1;
    img_header.channels = 1;
    img_header.data_type = ISMRMRD::ISMRMRD_FLOAT;

    const size_t payload_size = 2 * 2 * 1 * sizeof(float);
    std::vector<char> body_data(sizeof(ISMRMRD::ImageHeader) + payload_size);
    std::memcpy(body_data.data(), &img_header, sizeof(img_header));
    
    // Fill payload with dummy data
    std::fill(body_data.begin() + sizeof(ISMRMRD::ImageHeader), body_data.end(), 0);

        SECTION("Valid Frame Append")
        {
            http::request<http::string_body> req{http::verb::post, "/v1/mrd/frame", 11};
            req.set("X-MRD-Stream", "test_stream");
            req.body() = std::string(body_data.begin(), body_data.end());
            req.prepare_payload();
    
            auto res = handle_http_request(req, state);
            REQUIRE(res.result() == http::status::ok);
            auto body = json::parse(res.body());
            CHECK(body["status"] == "ok");
            CHECK(body["data"]["stream"] == "test_stream");
        }
    
        SECTION("Missing Stream Header")
        {
            http::request<http::string_body> req{http::verb::post, "/v1/mrd/frame", 11};
            // No X-MRD-Stream
            req.body() = std::string(body_data.begin(), body_data.end());
            req.prepare_payload();
    
            auto res = handle_http_request(req, state);
            REQUIRE(res.result() == http::status::bad_request);
            auto body = json::parse(res.body());
            CHECK(body["status"] == "error");
            CHECK(body["error"] == "missing X-MRD-Stream header");
        }
    
        SECTION("Invalid Payload Size")
        {
            http::request<http::string_body> req{http::verb::post, "/v1/mrd/frame", 11};
            req.set("X-MRD-Stream", "test_stream");
            // Body too short for declared dimensions
            req.body() = std::string(body_data.begin(), body_data.end() - 1);
            req.prepare_payload();
    
            auto res = handle_http_request(req, state);
            REQUIRE(res.result() == http::status::bad_request);
            auto body = json::parse(res.body());
            CHECK(body["status"] == "error");
            CHECK(body["error"] == "payload size does not match header");
        }
    fs::remove_all(temp);
}

TEST_CASE("HTTP Handler: 404 Not Found", "[http]")
{
    MarshalState state;
    http::request<http::string_body> req{http::verb::get, "/unknown/endpoint", 11};
    auto res = handle_http_request(req, state);
    REQUIRE(res.result() == http::status::not_found);
    auto body = json::parse(res.body());
    CHECK(body["status"] == "error");
}