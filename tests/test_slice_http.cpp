/*
 * tests/test_slice_http.cpp
 * HTTP slice-command endpoints -> slice_agent (stubbed). The marshal must
 * behave like Andrew's slice_control tool: six numbers from zero, UI moves
 * add to them, totals sent. Direct handler calls, no networking.
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
    bool connected{true};
    int cleared{0};
    void install(MarshalState& state, bool enabled = true) {
        state.dump_dir = unique_temp_dir();
        state.slice_agent_cfg.enabled = enabled;
        state.slice_agent_send = [this](const slice_math::WireCommand& c) {
            sent.push_back(c);
            return deliver;
        };
        state.slice_agent_connected = [this] { return connected; };
        state.slice_agent_clear = [this] { ++cleared; };
    }
};

json nudge(double dir) { return {{"client_id", "client-webgl"}, {"values", {dir}}}; }

} // namespace

TEST_CASE("Six numbers start at zero; +1 = PgUp", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);

    auto res = post(state, "/write/file_slice_translation", nudge(1));
    REQUIRE(res.result() == http::status::ok);
    auto body = json::parse(res.body());
    CHECK(body["file"] == "file_slice_translation");
    CHECK(body["direction"] == 1.0);
    CHECK(body["delivered"] == true);
    CHECK(body["agent_connected"] == true);
    CHECK(body["enabled"] == true);
    CHECK(body["state"]["tz"] == Catch::Approx(1.0));
    CHECK(body["state"]["rx_deg"] == Catch::Approx(0.0));
    REQUIRE(agent.sent.size() == 1);
    CHECK(agent.sent[0].tx == 0.0);
    CHECK(agent.sent[0].ty == 0.0);
    CHECK(agent.sent[0].tz == Catch::Approx(1.0));
    CHECK(agent.sent[0].flags == slice_math::kFlagUpdate);

    // second press accumulates; -1 undoes
    post(state, "/write/file_slice_translation", nudge(1));
    post(state, "/write/file_slice_translation", nudge(-1));
    REQUIRE(agent.sent.size() == 3);
    CHECK(agent.sent[1].tz == Catch::Approx(2.0));
    CHECK(agent.sent[2].tz == Catch::Approx(1.0));

    // GET caches
    auto g = get(state, "/read/file_slice_translation");
    REQUIRE(g.result() == http::status::ok);
    CHECK(json::parse(g.body())["values"][0] == -1);
    auto c = get(state, "/read/slice_commanded");
    REQUIRE(c.result() == http::status::ok);
    auto cb = json::parse(c.body());
    CHECK(cb["state"]["tz"] == Catch::Approx(1.0));
    CHECK(cb["count"] == 3);
    CHECK(cb["geometry"]["slice_dir"][2] == Catch::Approx(1.0));
    CHECK(cb["agent"]["connected"] == true);
}

TEST_CASE("Rotation sliders add degrees exactly like W/S, A/D, Q/E", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);
    const double d10 = 10.0 * slice_math::kDeg2Rad;

    REQUIRE(post(state, "/write/slice_delta", {{"rotation_rad", {d10, 0, 0}}}).result() == http::status::ok);
    REQUIRE(post(state, "/write/slice_delta", {{"rotation_rad", {0, -d10, 0}}}).result() == http::status::ok);
    REQUIRE(post(state, "/write/slice_delta", {{"rotation_rad", {0, 0, 3 * d10}}}).result() == http::status::ok);
    REQUIRE(agent.sent.size() == 3);
    CHECK(agent.sent[0].rx == Catch::Approx(10.0));    // W: +10, NOT -10
    CHECK(agent.sent[1].ry == Catch::Approx(-10.0));   // A: -10
    CHECK(agent.sent[2].rz == Catch::Approx(30.0));    // E x3
    CHECK(agent.sent[2].rx == Catch::Approx(10.0));    // angles accumulate independently

    // PgUp after the tilt follows row 2 of buildRot(10,-10,30)
    REQUIRE(post(state, "/write/file_slice_translation", nudge(1)).result() == http::status::ok);
    const auto R = slice_math::build_rot_matrix(10 * slice_math::kDeg2Rad, -10 * slice_math::kDeg2Rad, 30 * slice_math::kDeg2Rad);
    REQUIRE(agent.sent.size() == 4);
    CHECK(agent.sent[3].tx == Catch::Approx(R[2][0]).margin(1e-12));
    CHECK(agent.sent[3].ty == Catch::Approx(R[2][1]).margin(1e-12));
    CHECK(agent.sent[3].tz == Catch::Approx(R[2][2]).margin(1e-12));
}

TEST_CASE("slice_delta: in-plane translation follows rows 0/1; both fields at once", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);
    REQUIRE(post(state, "/write/slice_delta", {{"rotation_rad", {0, 0, 90 * slice_math::kDeg2Rad}}}).result()
            == http::status::bad_request);   // 90 > 30 deg clamp
    state.slice_agent_cfg.max_step_deg = 180.0;
    REQUIRE(post(state, "/write/slice_delta", {{"rotation_rad", {0, 0, 90 * slice_math::kDeg2Rad}}}).result()
            == http::status::ok);
    // "read" step of 2 mm after a 90-degree roll: row 0 of buildRot(0,0,90)
    // = (cos 90, -sin 90, 0) = (0, -1, 0) — exactly slice_control's RIGHT key.
    REQUIRE(post(state, "/write/slice_delta", {{"translation_mm", {2, 0, 0}}}).result() == http::status::ok);
    REQUIRE(agent.sent.size() == 2);
    CHECK(agent.sent[1].tx == Catch::Approx(0.0).margin(1e-12));
    CHECK(agent.sent[1].ty == Catch::Approx(-2.0));
    // translation then rotation in one request == PgUp then key
    REQUIRE(post(state, "/write/slice_delta", {{"translation_mm", {0, 0, 1}}, {"rotation_rad", {0.1, 0, 0}}}).result()
            == http::status::ok);
    REQUIRE(agent.sent.size() == 3);
    CHECK(agent.sent[2].tz == Catch::Approx(1.0));
    CHECK(agent.sent[2].rx == Catch::Approx(0.1 * slice_math::kRad2Deg));
}

TEST_CASE("Clamps: per-step and accumulated absolute", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);
    CHECK(post(state, "/write/slice_delta", {{"translation_mm", {0, 0, 51}}}).result() == http::status::bad_request);
    CHECK(post(state, "/write/slice_delta", {{"translation_mm", {1.0, 2.0}}}).result() == http::status::bad_request);
    CHECK(post(state, "/write/slice_delta", json::object()).result() == http::status::bad_request);
    CHECK(agent.sent.empty());
    // walk to the absolute limit: 6 x 50 = 300 OK, the 7th is rejected and not sent
    for (int i = 0; i < 6; ++i)
        REQUIRE(post(state, "/write/slice_delta", {{"translation_mm", {0, 0, 50}}}).result() == http::status::ok);
    CHECK(post(state, "/write/slice_delta", {{"translation_mm", {0, 0, 50}}}).result() == http::status::bad_request);
    CHECK(agent.sent.size() == 6);
    CHECK(agent.sent.back().tz == Catch::Approx(300.0));
    // rejected delta is not cached
    CHECK(json::parse(get(state, "/read/slice_delta").body())["translation_mm"][2] == Catch::Approx(50.0));
}

TEST_CASE("slice_reset zeroes and sends (the '0' key)", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);
    post(state, "/write/file_slice_translation", nudge(1));
    post(state, "/write/slice_delta", {{"rotation_rad", {0.2, 0, 0}}});
    auto res = post(state, "/write/slice_reset", json::object());
    REQUIRE(res.result() == http::status::ok);
    REQUIRE(agent.sent.size() == 3);
    CHECK(agent.sent[2].tz == 0.0);
    CHECK(agent.sent[2].rx == 0.0);
    CHECK(json::parse(res.body())["state"]["rx_deg"] == 0.0);
}

TEST_CASE("Scan start clears the state and the client's last command", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);
    post(state, "/write/file_slice_translation", nudge(1));
    REQUIRE(get(state, "/read/slice_commanded").result() == http::status::ok);
    {   // same reset the METADATA_XML handler runs
        std::lock_guard<std::mutex> lk(state.slice_state_mtx);
        state.slice_state.reset();
        state.slice_state_ts.clear();
        state.slice_state_count = 0;
        state.slice_agent_clear();
    }
    CHECK(agent.cleared == 1);
    REQUIRE(get(state, "/read/slice_commanded").result() == http::status::no_content);
    // next press starts from zero again
    post(state, "/write/file_slice_translation", nudge(1));
    REQUIRE(agent.sent.size() == 2);
    CHECK(agent.sent[1].tz == Catch::Approx(1.0));
    CHECK(json::parse(get(state, "/read/slice_commanded").body())["count"] == 1);
}

TEST_CASE("Agent unreachable / channel off", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);
    agent.deliver = false; agent.connected = false;
    auto res = post(state, "/write/file_slice_translation", nudge(1));
    REQUIRE(res.result() == http::status::ok);
    auto body = json::parse(res.body());
    CHECK(body["delivered"] == false);
    CHECK(body["agent_connected"] == false);
    CHECK(body["enabled"] == true);
    // the state still advanced (command kept for delivery on connect)
    post(state, "/write/file_slice_translation", nudge(1));
    CHECK(agent.sent.size() == 2);
    CHECK(agent.sent[1].tz == Catch::Approx(2.0));

    MarshalState off; AgentStub none; none.install(off, /*enabled=*/false);
    off.slice_agent_send = [](const slice_math::WireCommand&) { return false; };
    auto r2 = post(off, "/write/file_slice_translation", nudge(1));
    REQUIRE(r2.result() == http::status::ok);
    CHECK(json::parse(r2.body())["enabled"] == false);
    CHECK(json::parse(r2.body())["delivered"] == false);
    // nothing ever goes to the scanner socket
    int scanner_pushes = 0;
    off.mrd_push_message = [&](uint16_t, const void*, size_t) { ++scanner_pushes; return true; };
    post(off, "/write/slice_delta", {{"translation_mm", {0, 0, 1}}});
    CHECK(scanner_pushes == 0);
}

TEST_CASE("slice_target: absolute geometry -> six numbers", "[http][slice][target]") {
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
    auto body = json::parse(res.body());
    CHECK(body["delivered"] == true);
    CHECK(body["state"]["tx"] == Catch::Approx(12.5));
    CHECK(body["state"]["rz_deg"] == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(agent.sent.size() == 1);
    CHECK(agent.sent[0].tz == Catch::Approx(40.0));
    CHECK(json::parse(get(state, "/read/slice_target").body())["position"][1] == Catch::Approx(-3.0));

    // a following +1 builds on the target
    post(state, "/write/file_slice_translation", nudge(1));
    CHECK(agent.sent[1].tz == Catch::Approx(41.0));

    // tilted target: the angles rebuild the direction rows
    const auto R = slice_math::build_rot_matrix(0.4, -0.2, 0.9);
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

    // rejections
    json missing = valid; missing.erase("phase_dir");
    CHECK(post(state, "/write/slice_target", missing).result() == http::status::bad_request);
    json non_unit = valid; non_unit["read_dir"] = {2.0, 0.0, 0.0};
    CHECK(post(state, "/write/slice_target", non_unit).result() == http::status::bad_request);
    json non_ortho = valid; non_ortho["phase_dir"] = {1.0, 0.0, 0.0};
    CHECK(post(state, "/write/slice_target", non_ortho).result() == http::status::bad_request);
    json left = valid; left["slice_dir"] = {0.0, 0.0, -1.0};
    auto lres = post(state, "/write/slice_target", left);
    CHECK(lres.result() == http::status::bad_request);
    CHECK(json::parse(lres.body())["error"].get<std::string>().find("left-handed") != std::string::npos);
    json far = valid; far["position"] = {0.0, 0.0, 1000.0};
    CHECK(post(state, "/write/slice_target", far).result() == http::status::bad_request);
    CHECK(agent.sent.size() == 3);   // nothing invalid reached the agent
}

TEST_CASE("/status carries the slice_agent block", "[http][slice]") {
    MarshalState state; AgentStub agent; agent.install(state);
    auto j = json::parse(get(state, "/status").body());
    CHECK(j["slice_agent"]["enabled"] == true);
    CHECK(j["slice_agent"]["has_state"] == false);
    post(state, "/write/file_slice_translation", nudge(1));
    j = json::parse(get(state, "/status").body());
    CHECK(j["slice_agent"]["has_state"] == true);
    CHECK(j["slice_agent"]["commands_this_scan"] == 1);
}
