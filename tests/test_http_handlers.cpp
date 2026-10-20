/*
 * Tests for marshal_http.hpp — new v2 routes.
 * Tests use direct handler calls (no networking).
 */

#include <catch2/catch_all.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <filesystem>
#include <random>
#include <cstring>
#include <fstream>
#include <system_error>
#include <thread>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/dataset.h>

#include "marshal_http.hpp"
#include "marshal_state.hpp"
#include "live_image_store.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"

namespace fs = std::filesystem;
namespace http = boost::beast::http;
using json = nlohmann::json;

static const std::string TEST_XML = R"(<?xml version="1.0"?>
<ismrmrdHeader xmlns="http://www.ismrmrd.org/ISMRMRD">
    <encoding><encodedSpace><matrixSize><x>4</x><y>4</y><z>1</z></matrixSize></encodedSpace></encoding>
</ismrmrdHeader>)";

static std::string unique_temp_dir() {
    auto base = fs::temp_directory_path();
    std::string name = "cwru_marshal_http_test_" + std::to_string(std::random_device{}());
    fs::path full = base / name;
    fs::create_directories(full);
    return full.string();
}

static void init_state(MarshalState& state) {
    state.dump_dir = unique_temp_dir();
    state.dump_recorder = std::make_unique<mrd::DumpRecorder>(state.dump_dir);
    state.snapshot = std::make_unique<mrd::LatestSnapshot>(mrd::live_recon_dir(state.dump_dir));
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

static std::vector<uint8_t> make_wire_image(uint16_t series,
                                            uint16_t slice = 0,
                                            float value = 1.0f,
                                            uint16_t nz = 1,
                                            float z_mm = 0.0f) {
    ISMRMRD::ImageHeader hdr{};
    hdr.version = 1;
    hdr.data_type = ISMRMRD::ISMRMRD_FLOAT;
    hdr.matrix_size[0] = 4;
    hdr.matrix_size[1] = 4;
    hdr.matrix_size[2] = nz;
    hdr.channels = 1;
    hdr.image_series_index = series;
    hdr.slice = slice;
    hdr.image_type = ISMRMRD::ISMRMRD_IMTYPE_MAGNITUDE;
    hdr.position[2] = z_mm;
    hdr.read_dir[0] = 1.0f;
    hdr.phase_dir[1] = 1.0f;
    hdr.slice_dir[2] = 1.0f;

    std::string attr = "test";
    uint64_t attr_len = attr.size();
    std::vector<float> pixels(16 * nz, value);
    std::vector<uint8_t> body(mrd::IMAGE_HEADER_BYTES + sizeof(uint64_t) + attr.size() + pixels.size() * sizeof(float));
    size_t off = 0;
    std::memcpy(body.data() + off, &hdr, mrd::IMAGE_HEADER_BYTES);
    off += mrd::IMAGE_HEADER_BYTES;
    std::memcpy(body.data() + off, &attr_len, sizeof(uint64_t));
    off += sizeof(uint64_t);
    std::memcpy(body.data() + off, attr.data(), attr.size());
    off += attr.size();
    std::memcpy(body.data() + off, pixels.data(), pixels.size() * sizeof(float));
    return body;
}

// Poll /image/latest until the snapshot reflects `images` images.
static json wait_latest(MarshalState& state, size_t images) {
    json j;
    for (int i = 0; i < 400; ++i) {
        http::request<http::string_body> req{http::verb::get, "/image/latest", 11};
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::ok);
        j = json::parse(res.body());
        if (!j["path"].is_null() && j["images"] == images) return j;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return j;
}

TEST_CASE("GET /image/latest before any image: no path, status in words", "[http][latest]") {
    MarshalState state; init_state(state);
    http::request<http::string_body> req{http::verb::get, "/image/latest", 11};
    auto res = dispatch(req, state);
    REQUIRE(res.result() == http::status::ok);
    auto j = json::parse(res.body());
    CHECK(j["path"].is_null());
    CHECK(j["images"] == 0);
    CHECK(j["status"] == "waiting for scan");
}

TEST_CASE("Recon image reaches the viewer snapshot and the status", "[http][latest]") {
    MarshalState state; init_state(state);
    state.snapshot->begin_scan(TEST_XML);
    state.set_status("recon failed", "cannot connect");

    auto img = make_wire_image(1, 0, 3.0f);
    handle_recon_image(state, img.data(), img.size());

    auto j = wait_latest(state, 1);
    REQUIRE_FALSE(j["path"].is_null());
    CHECK(fs::exists(j["path"].get<std::string>()));
    CHECK(j["status"] == "recon failed");
    CHECK(j["status_detail"] == "cannot connect");
}

TEST_CASE("Dump mode: images archived, viewer off and says so", "[http][latest][dump]") {
    MarshalState state; init_state(state);
    state.dump_mode = true;
    state.snapshot->begin_scan(TEST_XML);
    state.set_status("scanning");

    auto img = make_wire_image(1, 0, 3.0f);
    handle_recon_image(state, img.data(), img.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    http::request<http::string_body> req{http::verb::get, "/image/latest", 11};
    auto j = json::parse(dispatch(req, state).body());
    CHECK(j["path"].is_null());
    CHECK(j["images"] == 0);
    CHECK(j["status"] == "scanning (dump mode, viewer off)");
    CHECK(state.perf_recon_images_received.load() == 1);
}

TEST_CASE("Snapshot keeps the newest image per slice position", "[http][latest][multislice]") {
    MarshalState state; init_state(state);
    state.snapshot->begin_scan(TEST_XML);

    // Three positions, slice numbers all 0 (a recon that does not number
    // slices): still three slices, because position decides, not numbering.
    for (int z = 0; z < 3; ++z) {
        auto img = make_wire_image(100 + z, 0, 1.0f, 1, 8.0f * z);
        handle_recon_image(state, img.data(), img.size());
    }
    auto j = wait_latest(state, 3);
    CHECK(j["images"] == 3);

    // Next pass over the same positions replaces, never accumulates.
    for (int z = 0; z < 3; ++z) {
        auto img = make_wire_image(200 + z, 0, 2.0f, 1, 8.0f * z);
        handle_recon_image(state, img.data(), img.size());
    }
    j = wait_latest(state, 3);
    CHECK(j["images"] == 3);

    // A new scan starts empty.
    const auto gen = j["generation"].get<uint64_t>();
    state.snapshot->begin_scan(TEST_XML);
    http::request<http::string_body> req{http::verb::get, "/image/latest", 11};
    auto after = json::parse(dispatch(req, state).body());
    CHECK(after["path"].is_null());
    CHECK(after["generation"].get<uint64_t>() > gen);
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
    auto scanner_dir = mrd::dump_scanner_dir(state.dump_dir);
    auto recon_dir = mrd::dump_recon_dir(state.dump_dir);
    std::ofstream(scanner_dir / "scan_a.h5").put('h');
    std::ofstream(scanner_dir / "latest_image.h5").put('x');
    std::ofstream(recon_dir / "scan_b.h5").put('h');
    std::ofstream(recon_dir / "latest_error.png").put('p');

    {
        http::request<http::string_body> req{http::verb::get, "/dump/scanner", 11};
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::ok);
        auto j = json::parse(res.body());
        REQUIRE(j.size() == 1);
        std::string combined = j.dump();
        CHECK(combined.find("scan_a.h5") != std::string::npos);
        CHECK(combined.find("latest_image.h5") == std::string::npos);
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
