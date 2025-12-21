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

    http::request<http::string_body> req{http::verb::get, "/health", 11};
    auto res = handle_http_request(req, state);

    REQUIRE(res.result() == http::status::ok);
    auto body = json::parse(res.body());
    CHECK(body["status"] == "ok");
    CHECK(body.contains("uptime_s"));
}

TEST_CASE("HTTP Handler: Configuration", "[http]")
{
    MarshalState state;
    state.data_dir = "/tmp/fake_data_dir";

    http::request<http::string_body> req{http::verb::get, "/v1/config", 11};
    auto res = handle_http_request(req, state);

    REQUIRE(res.result() == http::status::ok);
    auto body = json::parse(res.body());
    CHECK(body["data_dir"] == "/tmp/fake_data_dir");
    CHECK(body["ws_port"] == 8090);
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
        REQUIRE(res.result() == http::status::ok);
        auto body = json::parse(res.body());
        CHECK(body["pose"]["p"] == std::vector<double>{0, 0, 0});
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

        // Verify state update
        auto p = state.poses.get();
        CHECK(p.p[0] == 1.0);
        CHECK(p.source == "test_source");

        // Verify persistence
        fs::path pose_log = fs::path(temp) / "mrd" / "poses.jsonl";
        CHECK(fs::exists(pose_log));
    }

    SECTION("Update pose invalid (missing fields)")
    {
        json payload = {{"p", {1.0, 2.0, 3.0}}}; // Missing R
        http::request<http::string_body> req{http::verb::post, "/v1/pose/update", 11};
        req.body() = payload.dump();
        req.prepare_payload();

        auto res = handle_http_request(req, state);
        REQUIRE(res.result() == http::status::bad_request);
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
        std::string content = "fake mrd content";
        http::request<http::string_body> req{http::verb::post, "/v1/mrd/ingest", 11};
        req.body() = content;
        req.prepare_payload();

        auto res = handle_http_request(req, state);
        REQUIRE(res.result() == http::status::created);
        
        auto body = json::parse(res.body());
        CHECK(body.contains("path"));
        CHECK(body.contains("ts"));
        
        // Verify file exists
        std::string filepath = body["path"];
        CHECK(fs::exists(filepath));
        
        // Verify index.jsonl
        CHECK(fs::exists(fs::path(temp) / "mrd" / "index.jsonl"));
        
        // Verify latest.json
        CHECK(fs::exists(fs::path(temp) / "mrd" / "latest.json"));
    }

    SECTION("Get Latest MRD")
    {
        // Setup initial state
        fs::path latest_path = fs::path(temp) / "mrd" / "latest.json";
        std::ofstream(latest_path) << R"({"id": "file1.mrd"})";

        http::request<http::string_body> req{http::verb::get, "/v1/mrd/latest", 11};
        auto res = handle_http_request(req, state);
        REQUIRE(res.result() == http::status::ok);
        CHECK(res.body() == R"({"id": "file1.mrd"})");
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
        REQUIRE(body.is_array());
        REQUIRE(body.size() == 2);
        CHECK(body[0]["id"] == "2");
        CHECK(body[1]["id"] == "3");
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
    ISMRMRD::ImageHeader img_header;
    std::memset(&img_header, 0, sizeof(img_header));
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
            auto j = json::parse(res.body());
            CHECK(j["status"] == "ok");
            CHECK(j["stream"] == "test_stream");
        }
    
        SECTION("Missing Stream Header")
        {
            http::request<http::string_body> req{http::verb::post, "/v1/mrd/frame", 11};
            // No X-MRD-Stream
            req.body() = std::string(body_data.begin(), body_data.end());
            req.prepare_payload();
    
            auto res = handle_http_request(req, state);
            REQUIRE(res.result() == http::status::bad_request);
            auto j = json::parse(res.body());
            CHECK(j["error"] == "missing X-MRD-Stream header");
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
            auto j = json::parse(res.body());
            CHECK(j["error"] == "payload size does not match header");
        }
    fs::remove_all(temp);
}

TEST_CASE("HTTP Handler: 404 Not Found", "[http]")
{
    MarshalState state;
    http::request<http::string_body> req{http::verb::get, "/unknown/endpoint", 11};
    auto res = handle_http_request(req, state);
    REQUIRE(res.result() == http::status::not_found);
}
