#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

struct PlaybackItem {
    std::chrono::system_clock::time_point ts;
    fs::path file;
};

// Helper to parse ISO8601-ish timestamp
inline std::chrono::system_clock::time_point parse_iso_loose(const std::string &s)
{
    std::tm tm{};
    long ms = 0;
    if (s.size() >= 20)
    {
        tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
        tm.tm_mon = std::stoi(s.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(s.substr(8, 2));
        tm.tm_hour = std::stoi(s.substr(11, 2));
        tm.tm_min = std::stoi(s.substr(14, 2));
        tm.tm_sec = std::stoi(s.substr(17, 2));
        auto dot = s.find('.', 19);
        if (dot != std::string::npos)
            ms = std::stol(s.substr(dot + 1));
    }
    auto tt = timegm(&tm);
    return std::chrono::system_clock::from_time_t(tt) + std::chrono::milliseconds(ms);
}

// Loads and normalizes items from an index.jsonl file
inline std::vector<PlaybackItem> load_index(const fs::path& index_path, const fs::path& session_dir) {
    std::vector<PlaybackItem> items;
    std::ifstream ifs(index_path);
    if (!ifs) return items;

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            if (!j.contains("file") && !j.contains("path")) continue;

            const std::string ts_str = j.value("ts", "");
            std::string path_str = j.contains("file")
                                       ? j["file"].get<std::string>()
                                       : j["path"].get<std::string>();

            // Normalize entry:
            //  - absolute paths: use as-is
            //  - relative paths:
            //      * if they already contain "files/", trim to "files/…"
            //      * then join with session_dir
            fs::path p_final;
            fs::path p_raw(path_str);

            if (p_raw.is_absolute()) {
                p_final = p_raw;
            } else {
                // strip leading "./"
                if (path_str.rfind("./", 0) == 0)
                    path_str.erase(0, 2);
                // if the string embeds the session path, strip everything before "files/"
                auto pos = path_str.find("files/");
                std::string rel = (pos != std::string::npos) ? path_str.substr(pos)
                                                             : path_str;
                p_final = session_dir / rel;
            }

            auto ts = ts_str.empty() ? std::chrono::system_clock::now() : parse_iso_loose(ts_str);
            items.push_back({ts, p_final});
        } catch (...) {
            // skip malformed line
        }
    }
    return items;
}
