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
    std::string client_id = "client-planning";

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

    std::string read_file_catheter_base_configuration = routes_config[client_id]["read_from"];
    std::string read_file_tip_position_orientation = routes_config[client_id]["read_from2"];
    std::string read_file_surface_model_parameters = routes_config[client_id]["read_from3"];
    std::string read_file_user_input = routes_config[client_id]["read_from4"];
    std::string read_file_3D_images = routes_config[client_id]["read_from5"];
    std::string read_file_biological_signals = routes_config[client_id]["read_from6"];
    std::string write_file = routes_config[client_id]["write_to"];

    // Step 2: Read from server file via GET
    std::string read_endpoint_catheter_base_configuration = "/read/" + read_file_catheter_base_configuration;
    auto res_catheter_base_configuration = cli.Get(read_endpoint_catheter_base_configuration.c_str());

    if (!res_catheter_base_configuration || res_catheter_base_configuration->status != 200) {
        std::cerr << "Failed to read from server file: " << read_file_catheter_base_configuration << "\n";
        std::cerr << "GET request response status: " << res_catheter_base_configuration->status << "\n";
        std::cerr << "GET request response body: " << res_catheter_base_configuration->body << "\n";
        return 1;
    }

    json input_data_catheter_base_configuration = json::parse(res_catheter_base_configuration->body);
    if (!input_data_catheter_base_configuration.contains("values") || !input_data_catheter_base_configuration["values"].is_array()) {
        std::cerr << "Invalid data in server file\n";
        return 1;
    }
    // Step 2a: Read from server file via GET
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
    // Step 2b: Read from server file via GET
    std::string read_endpoint_surface_model_parameters = "/read/" + read_file_surface_model_parameters;
    auto res_surface_model_parameters = cli.Get(read_endpoint_surface_model_parameters.c_str());

    if (!res_surface_model_parameters || res_surface_model_parameters->status != 200) {
        std::cerr << "Failed to read from server file: " << read_file_surface_model_parameters << "\n";
        std::cerr << "GET request response status: " << res_surface_model_parameters->status << "\n";
        std::cerr << "GET request response body: " << res_surface_model_parameters->body << "\n";
        return 1;
    }

    json input_data_surface_model_parameters = json::parse(res_surface_model_parameters->body);
    if (!input_data_surface_model_parameters.contains("values") || !input_data_surface_model_parameters["values"].is_array()) {
        std::cerr << "Invalid data in server file\n";
        return 1;
    }
    // Step 2c: Read from server file via GET
    std::string read_endpoint_user_input = "/read/" + read_file_user_input;
    auto res_user_input = cli.Get(read_endpoint_user_input.c_str());

    if (!res_user_input || res_user_input->status != 200) {
        std::cerr << "Failed to read from server file: " << read_file_user_input << "\n";
        std::cerr << "GET request response status: " << res_user_input->status << "\n";
        std::cerr << "GET request response body: " << res_user_input->body << "\n";
        return 1;
    }

    json input_data_user_input = json::parse(res_user_input->body);
    if (!input_data_user_input.contains("values") || !input_data_user_input["values"].is_array()) {
        std::cerr << "Invalid data in server file\n";
        return 1;
    }
    // Step 2d: Read from server file via GET
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
    // Step 2e: Read from server file via GET
    std::string read_endpoint_biological_signals = "/read/" + read_file_biological_signals;
    auto res_biological_signals = cli.Get(read_endpoint_biological_signals.c_str());

    if (!res_biological_signals || res_biological_signals->status != 200) {
        std::cerr << "Failed to read from server file: " << read_file_biological_signals << "\n";
        std::cerr << "GET request response status: " << res_biological_signals->status << "\n";
        std::cerr << "GET request response body: " << res_biological_signals->body << "\n";
        return 1;
    }

    json input_data_biological_signals = json::parse(res_biological_signals->body);
    if (!input_data_biological_signals.contains("values") || !input_data_biological_signals["values"].is_array()) {
        std::cerr << "Invalid data in server file\n";
        return 1;
    }
    // Step 3: Compute result = multiply values + index sum
    const auto& values = input_data_catheter_base_configuration["values"];
    std::cout << "Read values: " << values.dump(2) << "\n";
    std::vector<double> result = {100.0,2000.0,500.0,400.0,1000.0,1500.0,10.0};
    //double result = 1.0;
    /*for (size_t i = 0; i < values.size(); ++i) {
        result += values[i].get<double>();
        result += i;
    }*/

    // Step 4: Build output JSON
    json out_data = {
        {"client_id", client_id},
        {"sent_at", current_time_ns()},
        //{"tags", {"computed", "example"}},
        {"values", {result}}
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
