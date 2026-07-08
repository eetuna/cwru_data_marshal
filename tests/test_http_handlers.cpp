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

TEST_CASE("HTTP Handler: Slice Translation", "[http]") {
    MarshalState state; init_state(state);

    SECTION("GET before any POST returns no content") {
        http::request<http::string_body> req{http::verb::get, "/read/file_slice_translation", 11};
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::no_content);
    }

    SECTION("POST valid +1 then GET round-trips") {
        json payload = {{"client_id", "client-webgl"}, {"sent_at", 1700000000000LL}, {"values", {1}}};
        http::request<http::string_body> req{http::verb::post, "/write/file_slice_translation", 11};
        req.body() = payload.dump();
        req.prepare_payload();

        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::ok);
        auto body = json::parse(res.body());
        CHECK(body["file"] == "file_slice_translation");
        CHECK(body["direction"] == 1.0);

        http::request<http::string_body> get_req{http::verb::get, "/read/file_slice_translation", 11};
        auto get_res = dispatch(get_req, state);
        REQUIRE(get_res.result() == http::status::ok);
        auto get_body = json::parse(get_res.body());
        CHECK(get_body["values"][0] == 1);
        CHECK(get_body.contains("ts"));
    }

    SECTION("POST valid -1") {
        json payload = {{"client_id", "client-webgl"}, {"values", {-1}}};
        http::request<http::string_body> req{http::verb::post, "/write/file_slice_translation", 11};
        req.body() = payload.dump();
        req.prepare_payload();

        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::ok);
        CHECK(json::parse(res.body())["direction"] == -1.0);
    }

    SECTION("POST invalid direction is rejected") {
        json payload = {{"client_id", "client-webgl"}, {"values", {2}}};
        http::request<http::string_body> req{http::verb::post, "/write/file_slice_translation", 11};
        req.body() = payload.dump();
        req.prepare_payload();

        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::bad_request);
    }

    SECTION("POST malformed body is rejected") {
        http::request<http::string_body> req{http::verb::post, "/write/file_slice_translation", 11};
        req.body() = "not json";
        req.prepare_payload();

        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::bad_request);
    }
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

TEST_CASE("Slice command pushback + geometry", "[http][slice]") {
    MarshalState state; init_state(state);

    // Capture frames pushed to the (stubbed) scanner connection.
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> pushed;
    bool push_ok = true;
    state.mrd_push_message = [&](uint16_t tag, const void* d, size_t l) {
        const auto* p = static_cast<const uint8_t*>(d);
        pushed.emplace_back(tag, std::vector<uint8_t>(p, p + l));
        return push_ok;
    };

    auto post_nudge = [&](double dir) {
        json payload = {{"client_id", "client-webgl"}, {"values", {dir}}};
        http::request<http::string_body> req{http::verb::post, "/write/file_slice_translation", 11};
        req.body() = payload.dump();
        req.prepare_payload();
        return dispatch(req, state);
    };

    SECTION("GET /read/slice_geometry is 204 before any image") {
        http::request<http::string_body> req{http::verb::get, "/read/slice_geometry", 11};
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::no_content);
    }

    SECTION("no geometry yet: pushed TEXT has null slice_geometry, delivered=true") {
        auto res = post_nudge(1);
        REQUIRE(res.result() == http::status::ok);
        CHECK(json::parse(res.body())["delivered"] == true);
        REQUIRE(pushed.size() == 1);
        CHECK(pushed[0].first == mrd::MRD_MESSAGE_TEXT);

        const auto& bytes = pushed[0].second;
        REQUIRE(bytes.size() > sizeof(uint32_t));
        uint32_t wire_len = 0;
        std::memcpy(&wire_len, bytes.data(), sizeof(uint32_t));
        REQUIRE(bytes.size() == sizeof(uint32_t) + wire_len);
        REQUIRE(bytes.back() == '\0');   // NUL-terminated, per connection.py

        auto payload = json::parse(std::string(
            bytes.begin() + sizeof(uint32_t), bytes.end() - 1));
        CHECK(payload["type"] == "slice_translation");
        CHECK(payload["direction"] == 1.0);
        CHECK(payload["slice_geometry"].is_null());
    }

    SECTION("geometry from image header flows into endpoint and pushed command") {
        ISMRMRD::ImageHeader hdr{};
        hdr.slice = 3;
        hdr.position[0] = 10.5f; hdr.position[1] = -20.f; hdr.position[2] = 30.f;
        hdr.read_dir[0] = 1.f; hdr.phase_dir[1] = 1.f; hdr.slice_dir[2] = 1.f;
        std::vector<uint8_t> wire(mrd::IMAGE_HEADER_BYTES + sizeof(uint64_t), 0);
        std::memcpy(wire.data(), &hdr, sizeof(hdr));
        mrd::update_slice_geometry(state, wire.data(), wire.size());

        http::request<http::string_body> req{http::verb::get, "/read/slice_geometry", 11};
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::ok);
        auto geom = json::parse(res.body());
        CHECK(geom["latest_slice"] == 3);
        REQUIRE(geom["slices"].contains("3"));
        CHECK(geom["slices"]["3"]["position"][0] == Catch::Approx(10.5));
        CHECK(geom["slices"]["3"]["slice_dir"][2] == Catch::Approx(1.0));

        auto post_res = post_nudge(-1);
        REQUIRE(post_res.result() == http::status::ok);
        REQUIRE(pushed.size() == 1);
        const auto& bytes = pushed[0].second;
        auto payload = json::parse(std::string(
            bytes.begin() + sizeof(uint32_t), bytes.end() - 1));
        CHECK(payload["direction"] == -1.0);
        CHECK(payload["slice_geometry"]["slice"] == 3);
        CHECK(payload["slice_geometry"]["position"][1] == Catch::Approx(-20.0));

        // New scan clears the geometry (same reset the METADATA_XML handler runs)
        {
            std::lock_guard<std::mutex> lk(state.slice_geom_mtx);
            state.slice_geom.clear();
            state.latest_slice = -1;
        }
        http::request<http::string_body> req2{http::verb::get, "/read/slice_geometry", 11};
        auto res2 = dispatch(req2, state);
        REQUIRE(res2.result() == http::status::no_content);
    }

    SECTION("no scanner connected: delivered=false, cache still written") {
        push_ok = false;
        auto res = post_nudge(1);
        REQUIRE(res.result() == http::status::ok);
        CHECK(json::parse(res.body())["delivered"] == false);

        http::request<http::string_body> get_req{http::verb::get, "/read/file_slice_translation", 11};
        auto get_res = dispatch(get_req, state);
        REQUIRE(get_res.result() == http::status::ok);
        CHECK(json::parse(get_res.body())["values"][0] == 1);
    }
}

TEST_CASE("Slice target: absolute prescription pushback", "[http][slice]") {
    MarshalState state; init_state(state);

    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> pushed;
    bool push_ok = true;
    state.mrd_push_message = [&](uint16_t tag, const void* d, size_t l) {
        const auto* p = static_cast<const uint8_t*>(d);
        pushed.emplace_back(tag, std::vector<uint8_t>(p, p + l));
        return push_ok;
    };

    auto post_target = [&](json body) {
        http::request<http::string_body> req{http::verb::post, "/write/slice_target", 11};
        req.body() = body.dump();
        req.prepare_payload();
        return dispatch(req, state);
    };
    const json valid = {
        {"position", {12.5, -3.0, 40.0}},
        {"read_dir", {1.0, 0.0, 0.0}},
        {"phase_dir", {0.0, 1.0, 0.0}},
        {"slice_dir", {0.0, 0.0, 1.0}},
    };

    SECTION("GET before any POST returns 204") {
        http::request<http::string_body> req{http::verb::get, "/read/slice_target", 11};
        REQUIRE(dispatch(req, state).result() == http::status::no_content);
    }

    SECTION("valid prescription: pushed as TEXT, cached, delivered=true") {
        auto res = post_target(valid);
        REQUIRE(res.result() == http::status::ok);
        CHECK(json::parse(res.body())["delivered"] == true);

        REQUIRE(pushed.size() == 1);
        CHECK(pushed[0].first == mrd::MRD_MESSAGE_TEXT);
        const auto& bytes = pushed[0].second;
        uint32_t wire_len = 0;
        std::memcpy(&wire_len, bytes.data(), sizeof(uint32_t));
        REQUIRE(bytes.size() == sizeof(uint32_t) + wire_len);
        REQUIRE(bytes.back() == '\0');
        auto payload = json::parse(std::string(
            bytes.begin() + sizeof(uint32_t), bytes.end() - 1));
        CHECK(payload["type"] == "slice_target");
        CHECK(payload["position"][0] == Catch::Approx(12.5));
        CHECK(payload["slice_dir"][2] == Catch::Approx(1.0));
        CHECK(payload.contains("ts"));

        http::request<http::string_body> get_req{http::verb::get, "/read/slice_target", 11};
        auto get_res = dispatch(get_req, state);
        REQUIRE(get_res.result() == http::status::ok);
        CHECK(json::parse(get_res.body())["position"][1] == Catch::Approx(-3.0));
    }

    SECTION("rejections: missing field, non-unit, non-orthogonal") {
        json missing = valid; missing.erase("phase_dir");
        CHECK(post_target(missing).result() == http::status::bad_request);

        json non_unit = valid; non_unit["read_dir"] = {2.0, 0.0, 0.0};
        CHECK(post_target(non_unit).result() == http::status::bad_request);

        json non_ortho = valid; non_ortho["phase_dir"] = {1.0, 0.0, 0.0};
        CHECK(post_target(non_ortho).result() == http::status::bad_request);

        json bad_shape = valid; bad_shape["position"] = {1.0, 2.0};
        CHECK(post_target(bad_shape).result() == http::status::bad_request);

        CHECK(pushed.empty());   // nothing invalid reaches the scanner
    }

    SECTION("no scanner: delivered=false, still cached") {
        push_ok = false;
        auto res = post_target(valid);
        REQUIRE(res.result() == http::status::ok);
        CHECK(json::parse(res.body())["delivered"] == false);
        http::request<http::string_body> get_req{http::verb::get, "/read/slice_target", 11};
        REQUIRE(dispatch(get_req, state).result() == http::status::ok);
    }
}

TEST_CASE("Slice delta: relative move pushback", "[http][slice]") {
    MarshalState state; init_state(state);
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> pushed;
    state.mrd_push_message = [&](uint16_t tag, const void* d, size_t l) {
        const auto* p = static_cast<const uint8_t*>(d);
        pushed.emplace_back(tag, std::vector<uint8_t>(p, p + l));
        return true;
    };
    auto post_delta = [&](json body) {
        http::request<http::string_body> req{http::verb::post, "/write/slice_delta", 11};
        req.body() = body.dump();
        req.prepare_payload();
        return dispatch(req, state);
    };

    SECTION("translation + rotation pushed with defaults filled") {
        auto res = post_delta({{"translation_mm", {1.5, 0.0, -2.0}},
                               {"rotation_rad", {0.0, 0.1, 0.0}}});
        REQUIRE(res.result() == http::status::ok);
        CHECK(json::parse(res.body())["delivered"] == true);
        REQUIRE(pushed.size() == 1);
        auto payload = json::parse(std::string(
            pushed[0].second.begin() + sizeof(uint32_t), pushed[0].second.end() - 1));
        CHECK(payload["type"] == "slice_delta");
        CHECK(payload["translation_mm"][2] == Catch::Approx(-2.0));
        CHECK(payload["rotation_rad"][1] == Catch::Approx(0.1));
        CHECK(payload["slice_geometry"].is_null());
    }

    SECTION("translation only: rotation defaults to zeros") {
        post_delta({{"translation_mm", {0.0, 0.0, 5.0}}});
        REQUIRE(pushed.size() == 1);
        auto payload = json::parse(std::string(
            pushed[0].second.begin() + sizeof(uint32_t), pushed[0].second.end() - 1));
        CHECK(payload["rotation_rad"][0] == Catch::Approx(0.0));
    }

    SECTION("rejections: neither field, wrong shape") {
        CHECK(post_delta(json::object()).result() == http::status::bad_request);
        CHECK(post_delta({{"translation_mm", {1.0, 2.0}}}).result() == http::status::bad_request);
        CHECK(pushed.empty());
    }

    SECTION("cache readable at GET /read/slice_delta") {
        http::request<http::string_body> pre{http::verb::get, "/read/slice_delta", 11};
        REQUIRE(dispatch(pre, state).result() == http::status::no_content);
        post_delta({{"translation_mm", {1.0, 0.0, 0.0}}});
        http::request<http::string_body> post{http::verb::get, "/read/slice_delta", 11};
        auto res = dispatch(post, state);
        REQUIRE(res.result() == http::status::ok);
        CHECK(json::parse(res.body())["translation_mm"][0] == Catch::Approx(1.0));
    }
}

TEST_CASE("GET /image/latest returns 204 before first image", "[http]") {
    MarshalState state; init_state(state);
    http::request<http::string_body> req{http::verb::get, "/image/latest", 11};
    auto res = dispatch(req, state);
    REQUIRE(res.result() == http::status::no_content);
}

TEST_CASE("Dump mode disables live snapshot/history filesystem side effects",
          "[http][dump][mode]") {
    MarshalState state; init_state(state);
    state.dump_enabled = true;
    state.current_xml_header = TEST_XML;
    state.header_received.store(true);
    state.current_scan_filename = "scan_dumpmode_test.h5";

    SECTION("GET /image/latest returns 404") {
        http::request<http::string_body> req{http::verb::get, "/image/latest", 11};
        auto res = dispatch(req, state);
        REQUIRE(res.result() == http::status::not_found);
        auto j = json::parse(res.body());
        CHECK(j.contains("error"));
    }

    SECTION("handle_recon_image does not write live latest_image.h5") {
        const auto live_recon = mrd::live_recon_dir(state.dump_dir);
        const auto latest = live_recon / "latest_image.h5";
        std::error_code ec;
        fs::remove(latest, ec);

        auto wire = make_wire_image(1, 0, 1.0f);
        handle_recon_image(state, wire.data(), wire.size());

        // Give any (incorrectly enqueued) live writer a moment to flush.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        CHECK_FALSE(fs::exists(latest));
        std::lock_guard<std::mutex> lk(state.latest_image_mtx);
        CHECK(state.latest_image_path.empty());
    }

    SECTION("Live recorders not constructed in dump mode") {
        // The constructor gating in marshal_main.cpp lives outside this test
        // (we instantiate MarshalState directly), but the contract is that
        // when dump_enabled is true the http/listener paths skip the live
        // calls entirely. Confirm via state: append_live_image must early-out
        // and not lazily create a recorder.
        REQUIRE(state.dump_enabled);
        CHECK(state.scanner_live.recorder == nullptr);
        CHECK(state.recon_live.recorder == nullptr);

        auto wire = make_wire_image(2, 0, 2.0f);
        handle_recon_image(state, wire.data(), wire.size());

        // handle_recon_image returns early in dump mode, so the lazy
        // construction in append_live_image must not have happened.
        CHECK(state.recon_live.recorder == nullptr);
    }
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
