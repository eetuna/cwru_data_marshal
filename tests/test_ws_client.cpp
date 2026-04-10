/*
 * Tests for WebSocket client — both directions (ingestion + subscriber).
 * Uses Boost.Beast WebSocket client against a WsServer instance.
 *
 * NOTE: These are integration-style tests that require a running WsServer.
 * For now, basic compile verification and unit-level WS message parsing.
 */

#include <catch2/catch_all.hpp>
#include <string>

// Basic compile-check: ensure marshal_ws.hpp compiles with new state
#include "marshal_ws.hpp"
#include "marshal_state.hpp"

TEST_CASE("WsResult default construction", "[ws]") {
    WsResult res;
    CHECK(res.response.empty());
    CHECK(res.broadcast.empty());
    CHECK(res.topic.empty());
    CHECK_FALSE(res.is_subscription);
}

TEST_CASE("handle_ws_message subscribe", "[ws]") {
    MarshalState state;
    state.dump_dir = "/tmp/test_ws";

    std::string text = R"({"subscribe":"images"})";
    std::vector<uint8_t> empty_bin;

    auto res = handle_ws_message(state, text, empty_bin, "test-session");
    CHECK(res.is_subscription);
    CHECK(res.topic == "images");
}

TEST_CASE("handle_ws_message binary returns ignored", "[ws]") {
    MarshalState state;
    state.dump_dir = "/tmp/test_ws";

    std::string empty_text;
    std::vector<uint8_t> bin = {0x01, 0x02, 0x03};

    auto res = handle_ws_message(state, empty_text, bin, "test-session");
    // v2: binary ingestion returns "ignored" status
    CHECK(res.response.find("ignored") != std::string::npos);
}
