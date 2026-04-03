#include <iostream>
#include <fstream>
#include <chrono>
#include <string>
#include <map>
#include <cstdlib>

#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

int64_t current_time_ns() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
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
        std::string client_id = "client-controller";

        // Step 1: Load routing config
        std::ifstream routes_file("file_routes.json");
        if (!routes_file) {
            std::cerr << "Could not open routing config\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        json routes_config;
        routes_file >> routes_config;

        if (!routes_config.contains(client_id)) {
            std::cerr << "Routing for client not found\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        std::string read_file_desired_planned_motion = routes_config[client_id]["read_from"];
        std::string read_file_catheter_base_configuration = routes_config[client_id]["read_from2"];
        std::string read_file_tip_position_orientation = routes_config[client_id]["read_from3"];
        std::string read_file_biological_signals = routes_config[client_id]["read_from4"];
        std::string write_file = routes_config[client_id]["write_to"];

        // Step 2: Read from server file via GET
        std::string read_endpoint_desired_planned_motion = "/read/" + read_file_desired_planned_motion;
        auto res_desired_planned_motion = cli.Get(read_endpoint_desired_planned_motion.c_str());

        if (!res_desired_planned_motion || res_desired_planned_motion->status != 200) {
            std::cerr << "Failed to read from server file: " << read_file_desired_planned_motion << "\n";
            if (res_desired_planned_motion) {
                std::cerr << "GET request response status: " << res_desired_planned_motion->status << "\n";
                std::cerr << "GET request response body: " << res_desired_planned_motion->body << "\n";
            } else {
                std::cerr << "No response (connection failed)\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        json input_data_desired_planned_motion = json::parse(res_desired_planned_motion->body);
        if (!input_data_desired_planned_motion.contains("values") || !input_data_desired_planned_motion["values"].is_array()) {
            std::cerr << "Invalid data in server file\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Step 2a: Read from server file via GET
        std::string read_endpoint_catheter_base_configuration = "/read/" + read_file_catheter_base_configuration;
        auto res_catheter_base_configuration = cli.Get(read_endpoint_catheter_base_configuration.c_str());

        if (!res_catheter_base_configuration || res_catheter_base_configuration->status != 200) {
            std::cerr << "Failed to read from server file: " << read_file_catheter_base_configuration << "\n";
            if (res_catheter_base_configuration) {
                std::cerr << "GET request response status: " << res_catheter_base_configuration->status << "\n";
                std::cerr << "GET request response body: " << res_catheter_base_configuration->body << "\n";
            } else {
                std::cerr << "No response (connection failed)\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        json input_data_catheter_base_configuration = json::parse(res_catheter_base_configuration->body);
        if (!input_data_catheter_base_configuration.contains("values") || !input_data_catheter_base_configuration["values"].is_array()) {
            std::cerr << "Invalid data in server file\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Step 2b: Read from server file via GET
        std::string read_endpoint_tip_position_orientation = "/read/" + read_file_tip_position_orientation;
        auto res_tip_position_orientation = cli.Get(read_endpoint_tip_position_orientation.c_str());

        if (!res_tip_position_orientation || res_tip_position_orientation->status != 200) {
            std::cerr << "Failed to read from server file: " << read_file_tip_position_orientation << "\n";
            if (res_tip_position_orientation) {
                std::cerr << "GET request response status: " << res_tip_position_orientation->status << "\n";
                std::cerr << "GET request response body: " << res_tip_position_orientation->body << "\n";
            } else {
                std::cerr << "No response (connection failed)\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        json input_data_tip_position_orientation = json::parse(res_tip_position_orientation->body);
        if (!input_data_tip_position_orientation.contains("values") || !input_data_tip_position_orientation["values"].is_array()) {
            std::cerr << "Invalid data in server file\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Step 2c: Read from server file via GET
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
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Step 3: Compute result = multiply values + index sum
        const auto& values = input_data_desired_planned_motion["values"];
        std::cout << "Read values: " << values.dump(2) << "\n";

        //double result = 1.0;
        std::vector<double> result = {2.5, 3.3, 3.6, 2.0,3.0,5.0, 1.0, 4.0, 2.0, 3.0, 4.0,5.0}; // Example result vector (replace with actual computation)
        /*for (size_t i = 0; i < values.size(); ++i) {
            result += values[i].get<double>();
            result += i;
        }*/

        // Step 4: Build output JSON
        json out_data = {
            {"client_id", client_id},
            {"sent_at", current_time_ns()},
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
