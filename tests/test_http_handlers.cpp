/*
 * Tests for marshal_http.hpp — new v2 routes.
 * Tests use direct handler calls (no networking).
 */

#include <catch2/catch_all.hpp>
#include <boost/beast/http.hpp>
#include <filesystem>
#include <random>
#include <cstring>

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

TEST_CASE("POST /frame before /header returns 409", "[http]") {
    MarshalState state; init_state(state);
    http::request<http::string_body> req{http::verb::post, "/frame", 11};
    req.body() = "dummy";
    req.prepare_payload();
    auto res = dispatch(req, state);
    REQUIRE(res.result() == http::status::conflict);
}

TEST_CASE("POST /header + /config + /frame works", "[http]") {
    MarshalState state; init_state(state);

    // POST /header
    {
        http::request<http::string_body> req{http::verb::post, "/header", 11};
        req.body() = R"(<?xml version="1.0"?><ismrmrdHeader xmlns="http://www.ismrmrd.org/ISMRMRD"></ismrmrdHeader>)";
        req.prepare_payload();
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::ok);
    }

    // POST /config
    {
        http::request<http::string_body> req{http::verb::post, "/config", 11};
        req.body() = "simplefft";
        req.prepare_payload();
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::ok);
    }

    // POST /frame with a valid acquisition
    {
        constexpr uint16_t nsamples = 64;
        constexpr uint16_t nchannels = 1;
        ISMRMRD::AcquisitionHeader ahdr;
        std::memset(&ahdr, 0, sizeof(ahdr));
        ahdr.version = 1;
        ahdr.number_of_samples = nsamples;
        ahdr.active_channels = nchannels;
        ahdr.available_channels = nchannels;
        ahdr.trajectory_dimensions = 0;

        size_t data_bytes = nsamples * nchannels * sizeof(complex_float_t);
        std::vector<uint8_t> body(sizeof(ahdr) + data_bytes, 0);
        std::memcpy(body.data(), &ahdr, sizeof(ahdr));

        http::request<http::string_body> req{http::verb::post, "/frame", 11};
        req.body().assign(reinterpret_cast<char*>(body.data()), body.size());
        req.prepare_payload();
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::accepted);
    }

    // POST /close
    {
        http::request<http::string_body> req{http::verb::post, "/close", 11};
        req.prepare_payload();
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::ok);
    }

    // Verify HDF5 file was created in from_scanner/
    auto scanner_path = fs::path(state.dump_dir) / "from_scanner";
    bool found_h5 = false;
    if (fs::exists(scanner_path)) {
        for (auto& e : fs::directory_iterator(scanner_path)) {
            if (e.path().extension() == ".h5") { found_h5 = true; break; }
        }
    }
    CHECK(found_h5);
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
