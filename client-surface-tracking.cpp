#include <iostream>
#include <fstream>
#include <chrono>
#include <string>
#include <map>
#include <cstdlib>

#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

int64_t current_time_ms() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

int main() {
    // Configurable host/port via environment variables (default: localhost:8081)
    const char* host_env = std::getenv("ROBOT_MARSHAL_HOST");
    const char* port_env = std::getenv("ROBOT_MARSHAL_PORT");
    std::string marshal_host = host_env ? host_env : "localhost";
    int marshal_port = port_env ? std::stoi(port_env) : 8081;

    std::cout << "Connecting to robot marshal at " << marshal_host << ":" << marshal_port << std::endl;
    httplib::Client cli(marshal_host, marshal_port);

    while(true){
        std::string client_id = "client-surface-tracking";

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

        std::string read_file_biological_signals = routes_config[client_id]["read_from"];
        std::string read_file_streaming_2D_images = routes_config[client_id]["read_from2"];
        std::string write_file = routes_config[client_id]["write_to"];

        // Step 2: Read from server file via GET
        std::string read_endpoint_biological_signals = "/read/" + read_file_biological_signals;
        auto res_biological_signals = cli.Get(read_endpoint_biological_signals.c_str());

        if (!res_biological_signals || res_biological_signals->status != 200) {
            std::cerr << "Failed to read from server file: " << read_file_biological_signals << "\n";
            if (res_biological_signals) {
                std::cerr << "GET request response status: " << res_biological_signals->status << "\n";
                std::cerr << "GET request response body: " << res_biological_signals->body << "\n";
            } else {
                std::cerr << "No response (connection failed)\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        json input_data_biological_signals = json::parse(res_biological_signals->body);
        if (!input_data_biological_signals.contains("values") || !input_data_biological_signals["values"].is_array()) {
            std::cerr << "Invalid data in server file\n";
            return 1;
        }

        // Step 2a: Read from server file via GET
        std::string read_endpoint_streaming_2D_images = "/read/" + read_file_streaming_2D_images;
        auto res_streaming_2D_images = cli.Get(read_endpoint_streaming_2D_images.c_str());

        if (!res_streaming_2D_images || res_streaming_2D_images->status != 200) {
            std::cerr << "Failed to read from server file: " << read_file_streaming_2D_images << "\n";
            if (res_streaming_2D_images) {
                std::cerr << "GET request response status: " << res_streaming_2D_images->status << "\n";
                std::cerr << "GET request response body: " << res_streaming_2D_images->body << "\n";
            } else {
                std::cerr << "No response (connection failed)\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        json input_data_streaming_2D_images = json::parse(res_streaming_2D_images->body);
        if (!input_data_streaming_2D_images.contains("values") || !input_data_streaming_2D_images["values"].is_array()) {
            std::cerr << "Invalid data in server file\n";
            return 1;
        }

        // Step 3: Compute result = multiply values + index sum
        const auto& values = input_data_biological_signals["values"];
        std::cout << "Read values: " << values.dump(2) << "\n";

        //double result = 1.0;
        std::vector<double> result = {1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0};
        /*for (size_t i = 0; i < values.size(); ++i) {
            result += values[i].get<double>();
            result += i;
        }*/

        // Step 4: Build output JSON
        json out_data = {
            {"client_id", client_id},
            {"sent_at", current_time_ms()},
            //{"tags", {"computed", "example"}},
            {"values", result}
        };

        // Step 5: Send POST to server to write result
        std::string write_endpoint = "/write/" + write_file;
        auto post_res = cli.Post(write_endpoint.c_str(), out_data.dump(), "application/json");

        if (post_res && post_res->status == 200) {
            std::cout << "Result sent successfully. Server response:\n" << post_res->body << "\n";
        } else {
            std::cerr << "Failed to POST result to server.\n";
        }
        // Wait 5 milliseconds before the next request
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}
