#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <string>
#include <map>
#include <cstdlib>
#include <cmath>

#include "CRM.hpp"
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;
using namespace CRMCatheterModel;

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

    // -------------------------------------------------------------------------
    // One-time CRM setup (before the main control loop)
    // -------------------------------------------------------------------------
    std::cout << "Loading CRM catheter model parameters...\n";

    // Load parameters — these return objects, not bools. They throw on error.
    CRMCatheterModelParams CathParams = Load_CRMCatheterModelParams("catheterdata/CatheterParameterSet_2.txt");
    CatheterConfiguration CathConfig = Load_CatheterConfiguration("catheterdata/CatheterSpatialConfiguration_1.txt");

    // Allocate storage for marker positions and coil data (C-style arrays, NOT Eigen)
    double (*ReportedMarkerPos)[3] = new double[CathParams.no_locmarkers][3];
    double (*ReportedCoilPos)[3] = new double[CathParams.no_act_set][3];
    double (*ReportedCoilOrient)[9] = new double[CathParams.no_act_set][9];

    // Set up CRMForwardKinematicsData struct — must set ALL fields
    CRMForwardKinematicsData FKParams;
    FKParams.CathConfig = &CathConfig;
    FKParams.CathParams = &CathParams;
    FKParams.ContactMode = ContactModeType::FREE_TIP;
    FKParams.FinalValueOnly = false;  // we want marker locations
    FKParams.ReportedMarkerPos = ReportedMarkerPos;
    FKParams.ReportedCoilPos = ReportedCoilPos;
    FKParams.ReportedCoilOrient = ReportedCoilOrient;
    FKParams.IntegrationStepSize = 0.2;  // mm
    for (int i = 0; i < 3; i++) FKParams.TipConstraintPoint[i] = 0.0;
    for (int i = 0; i < 3; i++) FKParams.TipForce[i] = 0.0;
    for (int i = 0; i < 3; i++) FKParams.deltau0_initialguess[i] = 0.0;
    for (int i = 0; i < 3; i++) FKParams.ftip_initialguess[i] = 0.0;

    double PotentialEnergy = 0.0;
    int    localmin        = 0;  // MUST be int, not bool

    // -------------------------------------------------------------------------
    // Main control loop
    // -------------------------------------------------------------------------
    while (true) {
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

        std::string read_file_desired_planned_motion      = routes_config[client_id]["read_from"];
        std::string read_file_catheter_base_configuration = routes_config[client_id]["read_from2"];
        std::string read_file_tip_position_orientation    = routes_config[client_id]["read_from3"];
        std::string read_file_biological_signals          = routes_config[client_id]["read_from4"];
        std::string write_file                            = routes_config[client_id]["write_to"];

        // Step 2: Read desired planned motion (provides FK inputs)
        std::string read_endpoint_desired_planned_motion = "/read/" + read_file_desired_planned_motion;
        auto res_desired_planned_motion = cli.Get(read_endpoint_desired_planned_motion.c_str());

        if (!res_desired_planned_motion || res_desired_planned_motion->status != 200) {
            std::cerr << "Failed to read from server file: " << read_file_desired_planned_motion << "\n";
            if (res_desired_planned_motion) {
                std::cerr << "GET status: " << res_desired_planned_motion->status << "\n";
                std::cerr << "GET body:   " << res_desired_planned_motion->body   << "\n";
            } else {
                std::cerr << "No response (connection failed)\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        json input_data_desired_planned_motion = json::parse(res_desired_planned_motion->body);
        if (!input_data_desired_planned_motion.contains("values") ||
            !input_data_desired_planned_motion["values"].is_array()) {
            std::cerr << "Invalid data in server file\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Step 2a: Read catheter base configuration
        std::string read_endpoint_catheter_base_configuration = "/read/" + read_file_catheter_base_configuration;
        auto res_catheter_base_configuration = cli.Get(read_endpoint_catheter_base_configuration.c_str());
        if (!res_catheter_base_configuration || res_catheter_base_configuration->status != 200) {
            std::cerr << "Failed to read from server file: " << read_file_catheter_base_configuration << "\n";
            if (res_catheter_base_configuration) {
                std::cerr << "GET status: " << res_catheter_base_configuration->status << "\n";
                std::cerr << "GET body:   " << res_catheter_base_configuration->body   << "\n";
            } else {
                std::cerr << "No response (connection failed)\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        json input_data_catheter_base_configuration = json::parse(res_catheter_base_configuration->body);
        if (!input_data_catheter_base_configuration.contains("values") ||
            !input_data_catheter_base_configuration["values"].is_array()) {
            std::cerr << "Invalid data in server file\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Update P0 from catheter_base_configuration pixel selection (written by webgl front-end)
        {
            const auto& cbc_values = input_data_catheter_base_configuration["values"];
            if (cbc_values.size() >= 3) {
                CathConfig.p0[0] = cbc_values[0].get<double>();
                CathConfig.p0[1] = cbc_values[1].get<double>();
                CathConfig.p0[2] = cbc_values[2].get<double>();
                std::cout << "Updated P0 from catheter_base_configuration: ("
                          << CathConfig.p0[0] << ", " << CathConfig.p0[1] << ", " << CathConfig.p0[2] << ")\n";
            }
        }

        // Step 2b: Read tip position/orientation
        std::string read_endpoint_tip_position_orientation = "/read/" + read_file_tip_position_orientation;
        auto res_tip_position_orientation = cli.Get(read_endpoint_tip_position_orientation.c_str());
        if (!res_tip_position_orientation || res_tip_position_orientation->status != 200) {
            std::cerr << "Failed to read from server file: " << read_file_tip_position_orientation << "\n";
            if (res_tip_position_orientation) {
                std::cerr << "GET status: " << res_tip_position_orientation->status << "\n";
                std::cerr << "GET body:   " << res_tip_position_orientation->body   << "\n";
            } else {
                std::cerr << "No response (connection failed)\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        json input_data_tip_position_orientation = json::parse(res_tip_position_orientation->body);
        if (!input_data_tip_position_orientation.contains("values") ||
            !input_data_tip_position_orientation["values"].is_array()) {
            std::cerr << "Invalid data in server file\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Step 2c: Read biological signals
        std::string read_endpoint_biological_signals = "/read/" + read_file_biological_signals;
        auto res_biological_signals = cli.Get(read_endpoint_biological_signals.c_str());
        if (!res_biological_signals || res_biological_signals->status != 200) {
            std::cerr << "Failed to read from server file: " << read_file_biological_signals << "\n";
            if (res_biological_signals) {
                std::cerr << "GET status: " << res_biological_signals->status << "\n";
                std::cerr << "GET body:   " << res_biological_signals->body   << "\n";
            } else {
                std::cerr << "No response (connection failed)\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        json input_data_biological_signals = json::parse(res_biological_signals->body);
        if (!input_data_biological_signals.contains("values") ||
            !input_data_biological_signals["values"].is_array()) {
            std::cerr << "Invalid data in server file\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Step 3: Extract actuation currents and inserted length
        //
        // Expected format: [i1, i2, i3, i4, i5, i6, insertedLength]
        //   i1..i6         : coil actuation currents (A)
        //   insertedLength : catheter inserted length (mm)
        //
        const auto& values = input_data_desired_planned_motion["values"];
        std::cout << "Read desired_planned_motion: " << values.dump() << "\n";

        double i1 = 0.1, i2 = 0.1, i3 = 0.1, i4 = 0.1, i5 = 0.1, i6 = 0.1;
        double insertedLength = 100.0;

        if (values.size() >= 7) {
            i1             = values[0].get<double>();
            i2             = values[1].get<double>();
            i3             = values[2].get<double>();
            i4             = values[3].get<double>();
            i5             = values[4].get<double>();
            i6             = values[5].get<double>();
            insertedLength = values[6].get<double>();
        } else {
            std::cerr << "Warning: expected 7 values [i1..i6, insertedLength], got "
                      << values.size() << " — using defaults.\n";
        }

        // Step 4: Build control_inputs vector for CRM_ForwardKinematics
        VectorXd control_inputs(NUM_ACT_SET * 3 + 1);
        control_inputs[0] = i1;
        control_inputs[1] = i2;
        control_inputs[2] = i3;
        control_inputs[3] = i4;
        control_inputs[4] = i5;
        control_inputs[5] = i6;
        control_inputs[6] = insertedLength;

        // Step 5: Compute CRM Forward Kinematics
        VectorXd FKsolution = CRM_ForwardKinematics(
            control_inputs, FKParams, PotentialEnergy, localmin);

        // Step 6: Assemble output — flat array of 3-D positions
        //   Layout: [p0, markerN, markerN-1, ..., marker1, tip]
        //   p0      = catheter base (from CathConfig)
        //   markers = CRM markers in reverse order (base-to-tip along catheter)
        //   tip     = FK solution tip position
        std::vector<double> result;
        const int numMarkers = CathParams.no_locmarkers;

        // Base position p0
        result.push_back(CathConfig.p0[0]);
        result.push_back(CathConfig.p0[1]);
        result.push_back(CathConfig.p0[2]);

        // CRM marker positions in REVERSE order (from base toward tip)
        for (int j = numMarkers - 1; j >= 0; --j) {
            result.push_back(ReportedMarkerPos[j][0]);
            result.push_back(ReportedMarkerPos[j][1]);
            result.push_back(ReportedMarkerPos[j][2]);
        }

        // Tip position from FK solution
        result.push_back(FKsolution[0]);
        result.push_back(FKsolution[1]);
        result.push_back(FKsolution[2]);

        std::cout << "FK computed: " << numMarkers << " markers, "
                  << "tip=(" << FKsolution[0] << ", "
                              << FKsolution[1] << ", "
                              << FKsolution[2] << "), "
                  << "PE=" << PotentialEnergy << " N.mm\n";

        // Step 7: Build output JSON and POST to server
        json out_data = {
            {"client_id", client_id},
            {"sent_at",   current_time_ms()},
            {"values",    result}
        };

        std::string write_endpoint = "/write/" + write_file;
        auto post_res = cli.Post(write_endpoint.c_str(), out_data.dump(), "application/json");

        if (post_res && post_res->status == 200) {
            std::cout << "FK result sent (" << result.size() / 3 << " points).\n";
        } else {
            std::cerr << "Failed to POST FK result to server.\n";
        }

        // 5 ms period (200 Hz) — matches original controller update rate
        //40
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    // Cleanup allocated arrays
    delete[] ReportedMarkerPos;
    delete[] ReportedCoilPos;
    delete[] ReportedCoilOrient;

    return 0;
}
