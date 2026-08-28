/*
 * tests/test_slice_http.cpp
 * HTTP slice endpoints -> slice_agent (stubbed). The marshal must behave like
 * Andrew's slice_control tool: six numbers from zero, UI moves add to them,
 * totals sent. Direct handler calls, no networking.
 */

#include <catch2/catch_all.hpp>
#include <boost/beast/http.hpp>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>

#include <ismrmrd/ismrmrd.h>

#include "marshal_http.hpp"
#include "marshal_state.hpp"
#include "live_image_store.hpp"
#include "slice_math.hpp"

namespace fs = std::filesystem;
namespace http = boost::beast::http;
using json = nlohmann::json;

namespace {

std::string unique_temp_dir() {
    auto base = fs::temp_directory_path();
    std::string name = "cwru_marshal_slice_test_" + std::to_string(std::random_device{}());
    fs::path full = base / name;
    fs::create_directories(full);
    return full.string();
}

template <class Body>
http::response<http::string_body> dispatch(http::request<Body>& req, MarshalState& state) {
    http::response<http::string_body> res;
    handle_http_request(std::move(req), state, [&](auto&& r) { res = std::move(r); });
    return res;
}

http::response<http::string_body> post(MarshalState& state, const char* target, const json& body) {
    http::request<http::string_body> req{http::verb::post, target, 11};
    req.body() = body.dump();
    req.prepare_payload();
    return dispatch(req, state);
}

http::response<http::string_body> get(MarshalState& state, const char* target) {
    http::request<http::string_body> req{http::verb::get, target, 11};
    return dispatch(req, state);
}

// Capture commands handed to the (stubbed) slice agent.
struct AgentStub {
    std::vector<slice_math::WireCommand> sent;
    bool deliver{true};
    uint64_t gen{0};
    void install(MarshalState& state, bool enabled = true) {
        state.dump_dir = unique_temp_dir();
        state.slice_agent_cfg.enabled = enabled;
        state.slice_agent_post = [this](const slice_math::WireCommand& c) -> uint64_t {
            sent.push_back(c);
            return ++gen;
        };
        state.slice_agent_wait = [this](uint64_t g) {
            return (g != 0 && deliver) ? slice_math::Delivery::Delivered
                                       : slice_math::Delivery::NotDelivered;
        };
    }
};

json nudge(double dir) { return {{"client_id", "client-webgl"}, {"values", {dir}}}; }

void seed_header_geometry(MarshalState& state) {
    ISMRMRD::ImageHeader hdr{};
    hdr.slice = 3;
    hdr.position[0] = 10.5f; hdr.position[1] = -20.f; hdr.position[2] = 30.f;
    hdr.read_dir[0] = 1.f; hdr.phase_dir[1] = 1.f; hdr.slice_dir[2] = 1.f;
    std::vector<uint8_t> wire(mrd::IMAGE_HEADER_BYTES + sizeof(uint64_t), 0);
    std::memcpy(wire.data(), &hdr, sizeof(hdr));
    mrd::update_slice_geometry(state, wire.data(), wire.size());
}

} // namespace

TEST_CASE("± button: six numbers start at zero, +1 = PgUp", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);

    auto res = post(state, "/write/file_slice_translation", nudge(1));
    REQUIRE(res.result() == http::status::ok);
    auto body = json::parse(res.body());
    CHECK(body["file"] == "file_slice_translation");
    CHECK(body["direction"] == 1.0);
    CHECK(body["delivered"] == true);
    CHECK(body["enabled"] == true);
    CHECK(body["state"]["tz"] == Catch::Approx(1.0));
    REQUIRE(agent.sent.size() == 1);
    CHECK(agent.sent[0].tx == 0.0);
    CHECK(agent.sent[0].tz == Catch::Approx(1.0));
    CHECK(agent.sent[0].rx == 0.0);
    CHECK(agent.sent[0].flags == slice_math::kFlagUpdate);

    post(state, "/write/file_slice_translation", nudge(1));
    post(state, "/write/file_slice_translation", nudge(-1));
    REQUIRE(agent.sent.size() == 3);
    CHECK(agent.sent[1].tz == Catch::Approx(2.0));
    CHECK(agent.sent[2].tz == Catch::Approx(1.0));

    auto g = get(state, "/read/file_slice_translation");
    REQUIRE(g.result() == http::status::ok);
    CHECK(json::parse(g.body())["values"][0] == -1);
}

TEST_CASE("Rotation sliders add degrees like W/S, A/D, Q/E", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);
    const double d10 = 10.0 * slice_math::kDeg2Rad;

    REQUIRE(post(state, "/write/slice_delta", {{"rotation_rad", {d10, 0, 0}}}).result() == http::status::ok);
    REQUIRE(post(state, "/write/slice_delta", {{"rotation_rad", {0, -d10, 0}}}).result() == http::status::ok);
    REQUIRE(post(state, "/write/slice_delta", {{"rotation_rad", {0, 0, 3 * d10}}}).result() == http::status::ok);
    REQUIRE(agent.sent.size() == 3);
    CHECK(agent.sent[0].rx == Catch::Approx(10.0));    // W: +10
    CHECK(agent.sent[1].ry == Catch::Approx(-10.0));   // A: -10
    CHECK(agent.sent[2].rz == Catch::Approx(30.0));    // E x3
    CHECK(agent.sent[2].rx == Catch::Approx(10.0));    // angles accumulate independently

    // PgUp after the rotations follows row 2 of buildRot(10,-10,30)
    REQUIRE(post(state, "/write/file_slice_translation", nudge(1)).result() == http::status::ok);
    const auto R = slice_math::build_rot_matrix(10 * slice_math::kDeg2Rad, -10 * slice_math::kDeg2Rad, 30 * slice_math::kDeg2Rad);
    REQUIRE(agent.sent.size() == 4);
    CHECK(agent.sent[3].tx == Catch::Approx(R[2][0]).margin(1e-12));
    CHECK(agent.sent[3].ty == Catch::Approx(R[2][1]).margin(1e-12));
    CHECK(agent.sent[3].tz == Catch::Approx(R[2][2]).margin(1e-12));

    // in-plane translation follows rows 0/1; both fields at once = key then key
    REQUIRE(post(state, "/write/slice_delta", {{"translation_mm", {2, 0, 0}}, {"rotation_rad", {0.1, 0, 0}}}).result()
            == http::status::ok);
    REQUIRE(agent.sent.size() == 5);
    CHECK(agent.sent[4].tx == Catch::Approx(agent.sent[3].tx + 2 * R[0][0]).margin(1e-12));
    CHECK(agent.sent[4].rx == Catch::Approx(10.0 + 0.1 * slice_math::kRad2Deg));

    CHECK(json::parse(get(state, "/read/slice_delta").body())["translation_mm"][0] == Catch::Approx(2.0));
}

TEST_CASE("Rejections: bad bodies", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);
    REQUIRE(get(state, "/read/file_slice_translation").result() == http::status::no_content);
    REQUIRE(get(state, "/read/slice_delta").result() == http::status::no_content);
    http::request<http::string_body> bad{http::verb::post, "/write/file_slice_translation", 11};
    bad.body() = "not json"; bad.prepare_payload();
    CHECK(dispatch(bad, state).result() == http::status::bad_request);
    CHECK(post(state, "/write/file_slice_translation", nudge(2)).result() == http::status::bad_request);
    CHECK(post(state, "/write/slice_delta", json::object()).result() == http::status::bad_request);
    CHECK(post(state, "/write/slice_delta", {{"translation_mm", {1.0, 2.0}}}).result() == http::status::bad_request);
    CHECK(agent.sent.empty());
}

TEST_CASE("Scan start zeroes the six numbers and tells the agent", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);
    post(state, "/write/file_slice_translation", nudge(1));
    post(state, "/write/slice_delta", {{"rotation_rad", {0.2, 0, 0}}});
    REQUIRE(agent.sent.size() == 2);

    state.reset_slice_state();   // what the METADATA_XML handler and close_scan() run
    REQUIRE(agent.sent.size() == 3);
    CHECK(agent.sent[2].tz == 0.0);
    CHECK(agent.sent[2].rx == 0.0);
    state.reset_slice_state();   // nothing sent since -> nothing posted
    CHECK(agent.sent.size() == 3);

    post(state, "/write/file_slice_translation", nudge(1));
    REQUIRE(agent.sent.size() == 4);
    CHECK(agent.sent[3].tz == Catch::Approx(1.0));
    CHECK(agent.sent[3].rx == 0.0);

    post(state, "/write/file_slice_translation", nudge(1));
    state.close_scan();
    CHECK(agent.sent.back().tz == 0.0);
}

TEST_CASE("Agent unreachable / channel off", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);
    agent.deliver = false;
    auto res = post(state, "/write/file_slice_translation", nudge(1));
    REQUIRE(res.result() == http::status::ok);
    CHECK(json::parse(res.body())["delivered"] == false);
    CHECK(json::parse(res.body())["enabled"] == true);
    post(state, "/write/file_slice_translation", nudge(1));   // state still advances; sent on connect
    CHECK(agent.sent.size() == 2);
    CHECK(agent.sent[1].tz == Catch::Approx(2.0));

    MarshalState off; AgentStub none; none.install(off, /*enabled=*/false);
    off.slice_agent_post = [](const slice_math::WireCommand&) { return uint64_t{0}; };
    off.slice_agent_wait = [](uint64_t) { return slice_math::Delivery::NotDelivered; };
    int scanner_pushes = 0;
    off.mrd_push_message = [&](uint16_t, const void*, size_t) { ++scanner_pushes; return true; };
    auto r2 = post(off, "/write/file_slice_translation", nudge(1));
    REQUIRE(r2.result() == http::status::ok);
    CHECK(json::parse(r2.body())["enabled"] == false);
    CHECK(json::parse(r2.body())["delivered"] == false);
    CHECK(scanner_pushes == 0);   // nothing ever goes on the scanner socket
}

TEST_CASE("slice_target: header pose -> six numbers", "[http][slice][target]") {
    MarshalState state; AgentStub agent; agent.install(state);
    const json valid = {
        {"position", {12.5, -3.0, 40.0}},
        {"read_dir", {1.0, 0.0, 0.0}},
        {"phase_dir", {0.0, 1.0, 0.0}},
        {"slice_dir", {0.0, 0.0, 1.0}},
    };
    REQUIRE(get(state, "/read/slice_target").result() == http::status::no_content);

    auto res = post(state, "/write/slice_target", valid);
    REQUIRE(res.result() == http::status::ok);
    CHECK(json::parse(res.body())["state"]["tx"] == Catch::Approx(12.5));
    REQUIRE(agent.sent.size() == 1);
    CHECK(agent.sent[0].tz == Catch::Approx(40.0));
    CHECK(agent.sent[0].rz == Catch::Approx(0.0).margin(1e-9));
    CHECK(json::parse(get(state, "/read/slice_target").body())["position"][1] == Catch::Approx(-3.0));

    post(state, "/write/file_slice_translation", nudge(1));   // builds on the target
    CHECK(agent.sent[1].tz == Catch::Approx(41.0));

    const auto R = slice_math::build_rot_matrix(0.4, -0.2, 0.9);   // an oblique plane
    json tilted = {
        {"position", {1.0, 2.0, 3.0}},
        {"read_dir", {R[0][0], R[0][1], R[0][2]}},
        {"phase_dir", {R[1][0], R[1][1], R[1][2]}},
        {"slice_dir", {R[2][0], R[2][1], R[2][2]}},
    };
    REQUIRE(post(state, "/write/slice_target", tilted).result() == http::status::ok);
    const auto& c = agent.sent.back();
    const auto back = slice_math::build_rot_matrix(c.rx * slice_math::kDeg2Rad, c.ry * slice_math::kDeg2Rad, c.rz * slice_math::kDeg2Rad);
    for (int r = 0; r < 3; ++r) for (int k = 0; k < 3; ++k)
        CHECK(back[r][k] == Catch::Approx(R[r][k]).margin(1e-6));

    json missing = valid; missing.erase("phase_dir");
    CHECK(post(state, "/write/slice_target", missing).result() == http::status::bad_request);
    json non_ortho = valid; non_ortho["phase_dir"] = {1.0, 0.0, 0.0};
    CHECK(post(state, "/write/slice_target", non_ortho).result() == http::status::bad_request);
    json left = valid; left["slice_dir"] = {0.0, 0.0, -1.0};
    CHECK(post(state, "/write/slice_target", left).result() == http::status::bad_request);
    CHECK(agent.sent.size() == 3);
}

TEST_CASE("GET /read/slice_geometry serves image-header geometry (display only)", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);
    REQUIRE(get(state, "/read/slice_geometry").result() == http::status::no_content);
    seed_header_geometry(state);
    auto geom = json::parse(get(state, "/read/slice_geometry").body());
    CHECK(geom["latest_slice"] == 3);
    CHECK(geom["slices"]["3"]["position"][0] == Catch::Approx(10.5));
    // header geometry is not the base of a move: the first +1 starts from zero
    post(state, "/write/file_slice_translation", nudge(1));
    REQUIRE(agent.sent.size() == 1);
    CHECK(agent.sent[0].tz == Catch::Approx(1.0));
    CHECK(agent.sent[0].tx == 0.0);
}
