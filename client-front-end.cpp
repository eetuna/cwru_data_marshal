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
    std::string client_id = "client-front-end";

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
    std::string read_file_3D_images = routes_config[client_id]["read_from2"];
    std::string read_file_tip_position_orientation = routes_config[client_id]["read_from3"];
    std::string write_file = routes_config[client_id]["write_to"];
    std::string write_file_catheter_base_configuration = routes_config[client_id]["write_to2"];

     // Step 2: Read from localization data server file via GET
    std::string read_endpoint_streaming_2D_images = "/read/" + read_file;
    auto res_streaming_2D_images = cli.Get(read_endpoint_streaming_2D_images.c_str());

    if (!res_streaming_2D_images || res_streaming_2D_images->status != 200) {
        std::cerr << "Failed to read from server file: " << read_file << "\n";
        std::cerr << "GET request response status: " << res_streaming_2D_images->status << "\n";
        std::cerr << "GET request response body: " << res_streaming_2D_images->body << "\n";
        return 1;
    }

    json input_data_streaming_2D_images = json::parse(res_streaming_2D_images->body);
    if (!input_data_streaming_2D_images.contains("values") || !input_data_streaming_2D_images["values"].is_array()) {
        std::cerr << "Invalid data in server file\n";
        return 1;
    }

    // Step 2': Read from catheter base configuration server file via GET
    std::string read_endpoint_3D_images = "/read/" + read_file_3D_images;
    auto res_3D_images = cli.Get(read_endpoint_3D_images.c_str());

    if (!res_3D_images || res_3D_images->status != 200) {
        std::cerr << "Failed to read from server file: " << read_file_3D_images << "\n";
        std::cerr << "GET request response status: " << res_3D_images->status << "\n";
        std::cerr << "GET request response body: " << res_3D_images->body << "\n";
        return 1;
    }

    json input_data_3D_images = json::parse(res_3D_images->body);
    if (!input_data_3D_images.contains("values") || !input_data_3D_images["values"].is_array()) {
        std::cerr << "Invalid data in server file\n";
        return 1;
    }

     // Step 2'': Read from catheter forward kinematics server file via GET
    std::string read_endpoint_tip_position_orientation = "/read/" + read_file_tip_position_orientation;
    auto res_tip_position_orientation = cli.Get(read_endpoint_tip_position_orientation.c_str());

    if (!res_tip_position_orientation || res_tip_position_orientation->status != 200) {
        std::cerr << "Failed to read from server file: " << read_file_tip_position_orientation << "\n";
        std::cerr << "GET request response status: " << res_tip_position_orientation->status << "\n";
        std::cerr << "GET request response body: " << res_tip_position_orientation->body << "\n";
        return 1;
    }

    json input_data_tip_position_orientation = json::parse(res_tip_position_orientation->body);
    if (!input_data_tip_position_orientation.contains("values") || !input_data_tip_position_orientation["values"].is_array()) {
        std::cerr << "Invalid data in server file\n";
        return 1;
    }
    // Step 3: Compute result = multiply values + index sum
    //Step 3: Dummy output for user input
    const auto& values = input_data_streaming_2D_images["values"];
    std::cout << "Read values: " << values.dump(2) << "\n";
    std::vector<double> result = {1.0, 3.0, 5.0, 5.0,135.0,180.0,5.0};
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
     // Step 3a: Compute result = multiply values + index sum
     //Step 3a: Dummy output for catheter base configuration
   // const auto& values_catheter_base_configuration = input_data["values"];
   // std::cout << "Read values: " << values_catheter_base_configuration.dump(2) << "\n";
    std::vector<double> result_catheter_base_configuration = {4.0, 3.0, 2.0, 0.0,5.0,180.0};
    /*for (size_t i = 0; i < values.size(); ++i) {
        result += values[i].get<double>();
        result += i;
    }*/

    // Step 4a: Build output JSON
    json out_data_catheter_base_configuration = {
        {"client_id", client_id},
        {"sent_at", current_time_ns()},
        //{"tags", {"computed", "example"}},
        {"values", result_catheter_base_configuration}
    };

    // Step 5a: Send POST to server to write result
    std::string write_endpoint_catheter_base_configuration = "/write/" + write_file_catheter_base_configuration;
    auto post_res_catheter_base_configuration = cli.Post(write_endpoint_catheter_base_configuration.c_str(), out_data_catheter_base_configuration.dump(), "application/json");

    if (post_res_catheter_base_configuration && post_res_catheter_base_configuration->status == 200) {
        std::cout << "Result sent successfully. Server response:\n" << post_res_catheter_base_configuration->body << "\n";
    } else {
        std::cerr << "Failed to POST result to server.\n";
    }
    // Wait 5 milliseconds before the next request
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
    return 0;
}
