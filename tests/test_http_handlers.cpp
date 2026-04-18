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
                                            uint16_t nz = 1) {
    ISMRMRD::ImageHeader hdr{};
    hdr.version = 1;
    hdr.data_type = ISMRMRD::ISMRMRD_FLOAT;
    hdr.matrix_size[0] = 4;
    hdr.matrix_size[1] = 4;
    hdr.matrix_size[2] = nz;
    hdr.channels = 1;
    hdr.image_series_index = series;
    hdr.slice = slice;

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

static uint32_t latest_image_count(const fs::path& path) {
    ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
    return ds.getNumberOfImages("image_0");
}

static uint16_t latest_image_series(const fs::path& path) {
    ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
    ISMRMRD::Image<float> image;
    ds.readImage("image_0", 0, image);
    return image.getHead().image_series_index;
}

static uint16_t latest_image_series_at(const fs::path& path, uint32_t index) {
    ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
    ISMRMRD::Image<float> image;
    ds.readImage("image_0", index, image);
    return image.getHead().image_series_index;
}

static uint16_t latest_image_slice(const fs::path& path) {
    ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
    ISMRMRD::Image<float> image;
    ds.readImage("image_0", 0, image);
    return image.getHead().slice;
}

static uint16_t latest_image_slice_at(const fs::path& path, uint32_t index) {
    ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
    ISMRMRD::Image<float> image;
    ds.readImage("image_0", index, image);
    return image.getHead().slice;
}

static uint16_t latest_image_matrix_z(const fs::path& path) {
    ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
    ISMRMRD::Image<float> image;
    ds.readImage("image_0", 0, image);
    return image.getHead().matrix_size[2];
}

TEST_CASE("GET /image/latest returns 204 before first image", "[http]") {
    MarshalState state; init_state(state);
    http::request<http::string_body> req{http::verb::get, "/image/latest", 11};
    auto res = dispatch(req, state);
    REQUIRE(res.result() == http::status::no_content);
}

TEST_CASE("Latest image path follows most recently updated lane", "[http][latest]") {
    MarshalState state; init_state(state);
    state.latest_writer.reset();
    state.current_xml_header = TEST_XML;
    state.header_received.store(true);  // simulate post-XML-header state
    state.current_scan_filename = "scan_test.h5";

    auto recon_first = make_wire_image(3, 0, 3.0f);
    handle_recon_image(state, recon_first.data(), recon_first.size());

    {
        std::lock_guard<std::mutex> lk(state.latest_image_mtx);
        CHECK(state.latest_image_path == (mrd::live_recon_dir(state.dump_dir) / "latest_image.h5").string());
        CHECK(state.latest_image_error == false);
    }

    auto scanner_first = make_wire_image(8, 0, 8.0f);
    mrd::append_live_image(state, mrd::LiveLane::Scanner, scanner_first.data(), scanner_first.size());

    {
        std::lock_guard<std::mutex> lk(state.latest_image_mtx);
        CHECK(state.latest_image_path == (mrd::live_scanner_dir(state.dump_dir) / "latest_image.h5").string());
        CHECK(state.latest_image_error == false);
    }

    mrd::flush_all_live_lanes(state);
    CHECK(fs::exists(mrd::live_recon_dir(state.dump_dir) / "scan_test.h5"));
    CHECK(fs::exists(mrd::live_scanner_dir(state.dump_dir) / "scan_test.h5"));
}

TEST_CASE("Latest recon multislice grows within one series", "[http][latest][multislice]") {
    MarshalState state; init_state(state);
    state.latest_writer.reset();
    state.current_xml_header = TEST_XML;
    state.header_received.store(true);  // simulate post-XML-header state
    state.current_scan_filename = "scan_multislice.h5";
    state.recon_expected_slices = 2;

    auto first = make_wire_image(7, 0, 1.0f);
    auto second = make_wire_image(7, 1, 2.0f);

    handle_recon_image(state, first.data(), first.size());
    auto latest_path = mrd::live_recon_dir(state.dump_dir) / "latest_image.h5";
    CHECK_FALSE(fs::exists(latest_path));

    handle_recon_image(state, second.data(), second.size());
    REQUIRE(fs::exists(latest_path));
    CHECK(latest_image_count(latest_path) == 2);
    CHECK(latest_image_series_at(latest_path, 0) == 7);
    CHECK(latest_image_series_at(latest_path, 1) == 7);
    CHECK(latest_image_slice_at(latest_path, 0) == 0);
    CHECK(latest_image_slice_at(latest_path, 1) == 1);
}

TEST_CASE("Latest recon series rollover replaces the prior logical result", "[http][latest][series]") {
    MarshalState state; init_state(state);
    state.latest_writer.reset();
    state.current_xml_header = TEST_XML;
    state.header_received.store(true);  // simulate post-XML-header state
    state.current_scan_filename = "scan_series_rollover.h5";
    state.recon_expected_slices = 2;

    auto first = make_wire_image(7, 0, 1.0f);
    auto second = make_wire_image(7, 1, 2.0f);
    auto third = make_wire_image(8, 0, 3.0f);

    auto latest_path = mrd::live_recon_dir(state.dump_dir) / "latest_image.h5";
    handle_recon_image(state, first.data(), first.size());
    handle_recon_image(state, second.data(), second.size());
    REQUIRE(fs::exists(latest_path));
    CHECK(latest_image_count(latest_path) == 2);

    handle_recon_image(state, third.data(), third.size());
    REQUIRE(fs::exists(latest_path));
    CHECK(latest_image_count(latest_path) == 2);
    CHECK(latest_image_series_at(latest_path, 0) == 7);
    CHECK(latest_image_series_at(latest_path, 1) == 7);

    auto fourth = make_wire_image(8, 1, 4.0f);
    handle_recon_image(state, fourth.data(), fourth.size());
    REQUIRE(fs::exists(latest_path));
    CHECK(latest_image_count(latest_path) == 2);
    CHECK(latest_image_series_at(latest_path, 0) == 8);
    CHECK(latest_image_series_at(latest_path, 1) == 8);
}

TEST_CASE("Latest recon preserves a single true 3D image", "[http][latest][3d]") {
    MarshalState state; init_state(state);
    state.latest_writer.reset();
    state.current_xml_header = TEST_XML;
    state.header_received.store(true);  // simulate post-XML-header state
    state.current_scan_filename = "scan_3d.h5";

    auto volume = make_wire_image(9, 0, 4.0f, 5);
    auto latest_path = mrd::live_recon_dir(state.dump_dir) / "latest_image.h5";

    handle_recon_image(state, volume.data(), volume.size());
    REQUIRE(fs::exists(latest_path));
    CHECK(latest_image_count(latest_path) == 1);
    CHECK(latest_image_series(latest_path) == 9);
    CHECK(latest_image_matrix_z(latest_path) == 5);
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
