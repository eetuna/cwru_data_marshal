#include <iostream>
#include <fstream>
#include <filesystem>
#include <shared_mutex>
#include <unordered_map>
#include <chrono>
#include <string>

#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;
//std::mutex file_mutex;
// Map filename -> shared_mutex
std::unordered_map<std::string, std::shared_mutex> file_mutexes;

// Mutex to protect the map itself
std::shared_mutex map_mutex;

// Load filenames from JSON config file into file_mutexes
bool load_config(const std::string& config_path) {
    std::ifstream ifs(config_path);
    if (!ifs) {
        std::cerr << "Failed to open config file: " << config_path << "\n";
        return false;
    }

    json config_json;
    try {
        ifs >> config_json;
    } catch (const std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << "\n";
        return false;
    }

    if (!config_json.is_array()) {
        std::cerr << "Config JSON is not an array of filenames\n";
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(map_mutex);
    for (const auto& filename_json : config_json) {
        if (!filename_json.is_string()) continue;
        std::string filename = filename_json.get<std::string>();

        // Insert if missing
        file_mutexes.try_emplace(filename);
    }

    return true;
}

// Access mutex for a file, assuming it exists in file_mutexes
std::shared_mutex& get_mutex_for_file(const std::string& filename) {
    std::shared_lock<std::shared_mutex> lock(map_mutex);
    auto it = file_mutexes.find(filename);
    if (it == file_mutexes.end()) {
        throw std::runtime_error("Unknown file: " + filename);
    }
    return it->second;
}

int64_t current_time_ns() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

int main() {
    httplib::Server server;
    const std::string storage_dir = "./";
    const std::string config_path = "files.json";

    if (!load_config(config_path)) {
        return 1;
    }

    // POST /write/<filename>
    server.Post(R"(/write/([\w\-.]+))", [&](const httplib::Request& req, httplib::Response& res) {
        std::string filename = req.matches[1];
        std::string path = storage_dir + filename;

        //std::lock_guard<std::mutex> lock(file_mutex);  // 🔒 LOCK
        std::shared_mutex* mtx = nullptr;
        try {
            mtx = &get_mutex_for_file(filename);
        } catch (...) {
            res.status = 404;
            res.set_content(R"({"error":"Unknown file"})", "application/json");
            return;
        }
        std::unique_lock<std::shared_mutex> lock(*mtx);

        try {
            auto input_json = json::parse(req.body);

            if (!input_json.contains("sent_at") || !input_json["sent_at"].is_number_integer()) {
                res.status = 400;
                res.set_content(R"({"error":"Missing or invalid 'sent_at'"})", "application/json");
                return;
            }

            if (!input_json.contains("values") || !input_json["values"].is_array()) {
                res.status = 400;
                res.set_content(R"({"error":"Missing or invalid 'values'"})", "application/json");
                return;
            }

            if (!input_json.contains("client_id") || !input_json["client_id"].is_string()) {
                res.status = 400;
                res.set_content(R"({"error":"Missing or invalid 'client_id'"})", "application/json");
                return;
            }

            input_json["received_at"] = current_time_ns();
            std::string json_output = input_json.dump(4); //cache
            std::string tmp_path = path + ".tmp";
            std::ofstream ofs(tmp_path);

            if (!ofs) {
                res.status = 500;
                res.set_content(R"({"error":"Failed to write file"})", "application/json");
                return;
            }

            ofs << json_output;
            ofs.close();
            try {
                std::filesystem::rename(tmp_path, path);
            } catch (const std::filesystem::filesystem_error& e) {
                res.status = 500;
                std::string err_msg = std::string(R"({"error":"Failed to rename temp file: )") + e.what() + "\"}";
                res.set_content(err_msg, "application/json");
                std::error_code ec;
                std::filesystem::remove(tmp_path, ec);  // optional cleanup, ignore errors here
                return;
            }
            res.set_content(json_output, "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(R"({"error":"Invalid JSON"})", "application/json");
        }
        // 🔓 UNLOCK: automatic via std::lock_guard
    });

    // GET /read/<filename>
    server.Get(R"(/read/([\w\-.]+))", [&](const httplib::Request& req, httplib::Response& res) {
        std::string filename = req.matches[1];
        std::string path = storage_dir + filename;
        std::shared_mutex* mtx = nullptr;
        try {
            mtx = &get_mutex_for_file(filename);
        } catch (...) {
            res.status = 404;
            res.set_content(R"({"error":"Unknown file"})", "application/json");
            return;
        }
        std::shared_lock<std::shared_mutex> lock(*mtx);

        std::ifstream ifs(path);
        if (!ifs) {
            res.status = 404;
            res.set_content(R"({"error":"File not found"})", "application/json");
            return;
        }

        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        res.set_content(content, "application/json");
    });

    std::cout << "Server running at http://localhost:8080\n";
    server.listen("172.28.1.10", 8080);
}
