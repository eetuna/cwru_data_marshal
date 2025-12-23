#include <iostream>
#include <fstream>
#include <chrono>
#include <string>
#include <map>

#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

int64_t current_time_ns() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

int main() {
    httplib::Client cli("172.28.1.10", 8080);
    while(true){
    std::string client_id = "client-a";

    // Step 1: Load routing config
    std::ifstream routes_file("file_routes.json");
    if (!routes_file) {
        std::cerr << "Could not open routing config\n";
        return 1;
    }

    json routes_config;
    routes_file >> routes_config;

    if (!routes_config.contains(client_id)) {
        std::cerr << "Routing for client not found\n";
        return 1;
    }

    std::string read_file = routes_config[client_id]["read_from"];
    std::string read_file_2 = routes_config[client_id]["read_from2"];
    std::string write_file = routes_config[client_id]["write_to1"];
    std::string write_file_2 = routes_config[client_id]["write_to2"];

    // Step 2: Read from server file via GET
    std::string read_endpoint = "/read/" + read_file;
    auto res = cli.Get(read_endpoint.c_str());

    if (!res || res->status != 200) {
        std::cerr << "Failed to read from server file: " << read_file << "\n";
        std::cerr << "GET request response status: " << res->status << "\n";
        std::cerr << "GET request response body: " << res->body << "\n";
        return 1;
    }

    json input_data = json::parse(res->body);
    if (!input_data.contains("values") || !input_data["values"].is_array()) {
        std::cerr << "Invalid data in server file\n";
        return 1;
    }

    // Step 3: Compute result = multiply values + index sum
    const auto& values = input_data["values"];
    double result = 1.0;
    for (size_t i = 0; i < values.size(); ++i) {
        result += values[i].get<double>();
        result += i;
    }

    // Step 4: Build output JSON
    json out_data = {
        {"client_id", client_id},
        {"sent_at", current_time_ns()},
        //{"tags", {"computed", "example"}},
        {"values",std::vector<double>{result}}
    };

    // Step 5: Send POST to server to write result
    std::string write_endpoint = "/write/" + write_file;
    auto post_res = cli.Post(write_endpoint.c_str(), out_data.dump(), "application/json");

    if (post_res && post_res->status == 200) {
        std::cout << "Result sent successfully. Server response:\n" << post_res->body << "\n";
    } else {
        std::cerr << "Failed to POST result to server.\n";
    }

    // Step 6: Send POST to server to write to other write file
    std::string write_endpoint_2 = "/write/" + write_file_2;
    //const auto& values = input_data["values"];
    std::vector<double> result2;

    for (size_t i = 0; i < values.size(); ++i) {
        double val = values[i].get<double>();
        result2.push_back(val + i);  // or any computation you want
    }
    for (size_t i = 0; i < 5;++i) {
        double val = 5;
        result2.push_back(val + i);  // or any computation you want
    }
    json out_data2 = {
        {"client_id", client_id},
        {"sent_at", current_time_ns()},
        //{"tags", {"computed", "example"}},
        {"values", result2}
    };
    auto post_res_2 = cli.Post(write_endpoint_2.c_str(), out_data2.dump(), "application/json");

    if (post_res_2 && post_res_2->status == 200) {
        std::cout << "Result2 sent successfully. Server response:\n" << post_res_2->body << "\n";
    } else {
        std::cerr << "Failed to POST result2 to server.\n";
    }
    //Step 7: Use GET to read k last entries from same read file as used in Step 2
    int k = 10000;
    std::string read_endpoint_mult = "/read/" + read_file_2+ "?last=" + std::to_string(k);
    auto res_mult = cli.Get(read_endpoint_mult.c_str());

    if (!res_mult || res_mult->status != 200) {
        std::cerr << "Failed to read from server file: " << read_file << "\n";
        std::cerr << "GET request response status: " << res_mult->status << "\n";
        std::cerr << "GET request response body: " << res_mult->body << "\n";
        return 1;
    }

    json input_data_mult = json::parse(res_mult->body);
    // Check that "entries" exists and is an array

    if (!input_data_mult.contains("entries") || !input_data_mult["entries"].is_array() || input_data_mult["entries"].empty()) {
        std::cerr << "Invalid data in server file (mult data)\n";
        return 1;
    }
    
        // Access the count
    int entry_count = 0;
    if (input_data_mult.contains("count") && input_data_mult["count"].is_number_integer()) {
        entry_count = input_data_mult["count"].get<int>();
    } else {
        // fallback: use size of array
        entry_count = input_data_mult["entries"].size();
    }

    std::cout << "Number of entries: " << entry_count << "\n";

    // Loop over all entries
    for (int entry_idx = 0; entry_idx < entry_count; entry_idx++) {
        const auto& entry = input_data_mult["entries"][entry_idx];

        if (!entry.contains("values") || !entry["values"].is_array()) {
            std::cerr << "Invalid entry at index " << entry_idx << "\n";
            return 1;
        }

        const auto& values = entry["values"];  // JSON array of numbers
        std::cout << "MULT Entries entry " << entry_idx << " values: ";
        for (size_t i = 0; i < values.size(); ++i) {
            std::cout << values[i].get<double>() << " "; // parse each value as double
        }
        std::cout << "\n";
    }
    
    // Wait 5 milliseconds before the next request
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
    return 0;
}
