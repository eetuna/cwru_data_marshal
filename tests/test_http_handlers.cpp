/*
 * Tests for marshal_http.hpp — new v2 routes.
 * Tests use direct handler calls (no networking).
 */

#include <catch2/catch_all.hpp>
#include <boost/beast/http.hpp>
#include <filesystem>
#include <random>
#include <cstring>
#include <fstream>

#include <ismrmrd/ismrmrd.h>

#include "marshal_http.hpp"
#include "marshal_state.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"

namespace fs = std::filesystem;
namespace http = boost::beast::http;
using json = nlohmann::json;

static std::string unique_temp_dir() {
    auto base = fs::temp_directory_path();
    std::string name = "cwru_marshal_http_test_" + std::to_string(std::random_device{}());
    fs::path full = base / name;
    fs::create_directories(full);
    return full.string();
}

static void init_state(MarshalState& state) {
    state.dump_dir = unique_temp_dir();
}

// Helper to call handle_http_request and capture response
template <class Body>
http::response<http::string_body> dispatch(http::request<Body>& req, MarshalState& state) {
    http::response<http::string_body> res;
    handle_http_request(std::move(req), state, [&](auto&& r) { res = std::move(r); });
    return res;
}

TEST_CASE("GET /health returns ok", "[http]") {
    MarshalState state; init_state(state);
    http::request<http::string_body> req{http::verb::get, "/health", 11};
    auto res = dispatch(req, state);
    REQUIRE(res.result() == http::status::ok);
    auto j = json::parse(res.body());
    CHECK(j["status"] == "ok");
    CHECK(j.contains("uptime_s"));
}

TEST_CASE("GET /transform returns zeros then clears", "[http]") {
    MarshalState state; init_state(state);

    // PUT a transform
    {
        http::request<http::string_body> req{http::verb::put, "/transform", 11};
        req.body() = R"({"through_plane_mm": 5.0, "readout_mm": 1.0})";
        req.prepare_payload();
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::ok);
    }

    // GET should return the values
    {
        http::request<http::string_body> req{http::verb::get, "/transform", 11};
        auto res = dispatch(req, state);
        auto j = json::parse(res.body());
        CHECK(j["through_plane_mm"] == 5.0);
        CHECK(j["readout_mm"] == 1.0);
    }

    // Second GET should return zeros (consumed)
    {
        http::request<http::string_body> req{http::verb::get, "/transform", 11};
        auto res = dispatch(req, state);
        auto j = json::parse(res.body());
        CHECK(j["through_plane_mm"] == 0.0);
        CHECK(j["readout_mm"] == 0.0);
    }
}

TEST_CASE("POST/GET /pose roundtrip", "[http]") {
    MarshalState state; init_state(state);

    {
        http::request<http::string_body> req{http::verb::post, "/pose", 11};
        req.body() = R"({"position": [1.0, 2.0, 3.0], "orientation": [1,0,0,0,1,0,0,0,1]})";
        req.prepare_payload();
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::ok);
    }

    {
        http::request<http::string_body> req{http::verb::get, "/pose", 11};
        auto res = dispatch(req, state);
        auto j = json::parse(res.body());
        auto p = j["p"];
        CHECK(p[0] == 1.0);
        CHECK(p[1] == 2.0);
        CHECK(p[2] == 3.0);
    }
}

TEST_CASE("Unknown route returns 404", "[http]") {
    MarshalState state; init_state(state);
    http::request<http::string_body> req{http::verb::get, "/v1/mrd/latest", 11};
    auto res = dispatch(req, state);
    REQUIRE(res.result() == http::status::not_found);
}

TEST_CASE("No /v1/ routes exist", "[http]") {
    MarshalState state; init_state(state);
    for (auto* path : {"/v1/mrd/frame", "/v1/mrd/latest", "/v1/pose/current", "/v1/config"}) {
        http::request<http::string_body> req{http::verb::get, path, 11};
        auto res = dispatch(req, state);
        CHECK(res.result() == http::status::not_found);
    }
}


TEST_CASE("Dump endpoints list H5 files only", "[http]") {
    MarshalState state; init_state(state);
    auto dump_scanner = mrd::dump_scanner_dir(state.dump_dir);
    auto dump_recon = mrd::dump_recon_dir(state.dump_dir);
    std::ofstream(dump_scanner / "scan_a.h5").put('h');
    std::ofstream(dump_scanner / "stray.txt").put('x');
    std::ofstream(dump_recon / "scan_b.h5").put('h');
    std::ofstream(dump_recon / "latest_error.png").put('p');

    {
        http::request<http::string_body> req{http::verb::get, "/dump/scanner", 11};
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::ok);
        auto j = json::parse(res.body());
        REQUIRE(j.size() == 1);
        std::string combined = j.dump();
        CHECK(combined.find("scan_a.h5") != std::string::npos);
        CHECK(combined.find("stray.txt") == std::string::npos);
    }

    {
        http::request<http::string_body> req{http::verb::get, "/dump/recon", 11};
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::ok);
        auto j = json::parse(res.body());
        REQUIRE(j.size() == 1);
        std::string combined = j.dump();
        CHECK(combined.find("scan_b.h5") != std::string::npos);
        CHECK(combined.find("latest_error.png") == std::string::npos);
    }
}
