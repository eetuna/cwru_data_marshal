#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>
#include "playback_core.hpp"
#include <random>

namespace fs = std::filesystem;

static std::string unique_temp_dir()
{
    auto base = fs::temp_directory_path();
    std::string name = "cwru_playback_test_" + std::to_string(std::random_device{}());
    fs::path full = base / name;
    fs::create_directories(full);
    return full.string();
}

TEST_CASE("playback core loads and normalizes index items", "[playback]")
{
    std::string temp = unique_temp_dir();
    fs::path index_path = fs::path(temp) / "index.jsonl";
    fs::path session_dir = fs::path(temp); // treating temp as the session root

    {
        std::ofstream f(index_path);
        // Entry 1: absolute path
        f << R"({"path": "/tmp/abs/file.mrd", "ts": "2023-01-01T10:00:00Z"})" << "\n";
        // Entry 2: relative path (simple)
        f << R"({"file": "./simple.mrd", "ts": "2023-01-01T10:00:01Z"})" << "\n";
        // Entry 3: relative path with embedded session structure (common in dumpbox)
        f << R"({"path": "./data/dumpbox/sess1/files/deep.mrd", "ts": "2023-01-01T10:00:02Z"})" << "\n";
        // Entry 4: invalid (missing path)
        f << R"({"ts": "2023-01-01T10:00:03Z"})" << "\n";
        f.flush();
    }

    UNSCOPED_INFO("index_path: " << index_path.string());
    UNSCOPED_INFO("session_dir: " << session_dir.string());

    auto items = load_index(index_path, session_dir);

    REQUIRE(items.size() == 3);

    // Check item 1 (absolute)
    CHECK(items[0].file == "/tmp/abs/file.mrd");
    
    // Check item 2 (simple relative -> joined with session_dir)
    CHECK(items[1].file == session_dir / "simple.mrd");

    // Check item 3 (deep relative -> stripped to files/... then joined)
    // The logic is: strip everything before "files/", then join.
    // So "./data/dumpbox/sess1/files/deep.mrd" -> "files/deep.mrd"
    CHECK(items[2].file == session_dir / "files/deep.mrd");

    fs::remove_all(temp);
}
