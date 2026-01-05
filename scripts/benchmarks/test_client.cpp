// Test client for robot-data-marshal based on upstream client design
// Runs N iterations of read-process-write cycle

#include <iostream>
#include <chrono>
#include <string>
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

int64_t current_time_ns() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <client_id> <read_file> <write_file> <iterations>\n";
        return 1;
    }

    std::string client_id = argv[1];
    std::string read_file = argv[2];
    std::string write_file = argv[3];
    int iterations = std::stoi(argv[4]);

    httplib::Client cli("127.0.0.1", 8081);
    cli.set_connection_timeout(5, 0); // 5 seconds
    cli.set_read_timeout(5, 0);
    cli.set_write_timeout(5, 0);

    for (int iter = 0; iter < iterations; ++iter) {
        // Read from server
        std::string read_endpoint = "/read/" + read_file;
        auto res = cli.Get(read_endpoint.c_str());

        if (!res || res->status != 200) {
            std::cerr << client_id << ": Failed to read from " << read_file
                      << " (iteration " << iter << ")\n";
            return 1;
        }

        // Parse response
        json input_data;
        try {
            input_data = json::parse(res->body);
        } catch (...) {
            std::cerr << client_id << ": Failed to parse JSON from " << read_file << "\n";
            return 1;
        }

        // Process data (simple: just read and re-send)
        double value = iter;
        if (input_data.contains("values") && input_data["values"].is_array() &&
            !input_data["values"].empty()) {
            value = input_data["values"][0].get<double>();
        }

        // Build output
        json out_data = {
            {"client_id", client_id},
            {"sent_at", current_time_ns()},
            {"values", std::vector<double>{value + 1.0}}
        };

        // Write to server
        std::string write_endpoint = "/write/" + write_file;
        auto post_res = cli.Post(write_endpoint.c_str(), out_data.dump(), "application/json");

        if (!post_res || post_res->status != 200) {
            std::cerr << client_id << ": Failed to write to " << write_file
                      << " (iteration " << iter << ")\n";
            return 1;
        }
    }

    std::cout << client_id << ": Completed " << iterations << " iterations\n";
    return 0;
}
