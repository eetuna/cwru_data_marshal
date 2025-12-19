#include <catch2/catch_all.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>

#include "marshal_ws.hpp"
#include "marshal_state.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

// Helper to create a unique temp directory
static std::string unique_temp_dir()
{
    auto base = fs::temp_directory_path();
    std::string name = "cwru_marshal_ws_test_" + std::to_string(std::random_device{}());
    fs::path full = base / name;
    fs::create_directories(full);
    return full.string();
}

TEST_CASE("WS Handler: Text Messages", "[ws]")
{
    MarshalState state;
    std::string topic;

    SECTION("Broadcast Text")
    {
        std::string msg = "hello world";
        auto res = handle_ws_message(state, msg, {}, topic);
        
        CHECK(res.broadcast == msg);
        CHECK(res.response.empty());
        CHECK(!res.is_subscription);
    }

    SECTION("Subscription Request")
    {
        std::string msg = R"({"subscribe": "my_topic"})";
        auto res = handle_ws_message(state, msg, {}, topic);
        
        CHECK(res.is_subscription);
        CHECK(res.topic == "my_topic");
        CHECK(res.response.find("subscribed") != std::string::npos);
        CHECK(res.broadcast.empty());
    }

    SECTION("Malformed JSON (treated as broadcast)")
    {
        std::string msg = "{ invalid json";
        auto res = handle_ws_message(state, msg, {}, topic);
        
        CHECK(res.broadcast == msg);
        CHECK(res.response.empty());
        CHECK(!res.is_subscription);
    }
}

TEST_CASE("WS Handler: Binary Ingestion", "[ws]")
{
    std::string temp = unique_temp_dir();
    MarshalState state;
    state.data_dir = temp;
    state.sink_mode = SinkMode::MRD;
    state.ws_emit = [](const std::string&) {}; // Mock
    state.ws_emit_topic = [](const std::string&, const std::string&) {}; // Mock

    // Create mrd dir
    fs::create_directories(fs::path(temp) / "mrd");

    std::string topic;

    SECTION("Valid Binary Payload")
    {
        std::string content = "binary content";
        std::vector<uint8_t> bin(content.begin(), content.end());
        
        auto res = handle_ws_message(state, "", bin, topic);
        
        CHECK(res.broadcast.empty());
        CHECK(!res.response.empty());
        
        auto j = json::parse(res.response);
        CHECK(j.contains("path"));
        CHECK(fs::exists(j["path"].get<std::string>()));
    }

    SECTION("Empty Binary Payload")
    {
        auto res = handle_ws_message(state, "", {}, topic);
        
        CHECK(res.broadcast.empty());
        CHECK(!res.response.empty());
        auto j = json::parse(res.response);
        CHECK(j["error"] == "empty payload");
    }

    fs::remove_all(temp);
}
