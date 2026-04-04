#include "CRM.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <iostream>

namespace CRMCatheterModel {

// ----------------------------------------------------------------------------
// Global catheter model state definitions
// ----------------------------------------------------------------------------
CatheterModelParams          CathParams;
CatheterSpatialConfiguration CathConfig;

// ----------------------------------------------------------------------------
// Load catheter mechanical parameter set
// File format (lines starting with '#' are comments):
//   TotalLength: <mm>
//   InsertDiameter: <mm>
//   EI: <N·mm²>
//   GJ: <N·mm²>
//   ActuationGain: <1/(mm·A)>
// ----------------------------------------------------------------------------
bool Load_CRMCatheterModelParams(const std::string& filename) {
    // Built-in defaults
    CathParams.totalLength    = 200.0;
    CathParams.insertDiameter = 2.5;
    CathParams.EI             = 2.5;
    CathParams.GJ             = 1.0;
    CathParams.k_act          = 0.02;   // 1/(mm·A)

    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "CRM: Cannot open parameters file: " << filename
                  << " — using defaults.\n";
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        std::string key;
        if (!std::getline(iss, key, ':')) continue;
        double val;
        if (!(iss >> val)) continue;
        if      (key == "TotalLength")    CathParams.totalLength    = val;
        else if (key == "InsertDiameter") CathParams.insertDiameter = val;
        else if (key == "EI")             CathParams.EI             = val;
        else if (key == "GJ")             CathParams.GJ             = val;
        else if (key == "ActuationGain")  CathParams.k_act          = val;
    }
    std::cout << "CRM: Loaded parameters from " << filename << "\n";
    return true;
}

// ----------------------------------------------------------------------------
// Load catheter spatial configuration
// File format (non-comment lines in order):
//   Line 1: p0_x p0_y p0_z        (base position, mm)
//   Line 2: R0_00 R0_01 R0_02     (first row of base orientation matrix)
//   Line 3: R0_10 R0_11 R0_12
//   Line 4: R0_20 R0_21 R0_22
// ----------------------------------------------------------------------------
bool Load_CatheterConfiguration(const std::string& filename) {
    CathConfig.p0 = Vector3d::Zero();
    CathConfig.R0 = Matrix3d::Identity();

    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "CRM: Cannot open configuration file: " << filename
                  << " — using identity pose.\n";
        return false;
    }

    std::string line;
    int dataLine = 0;
    int matRow   = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        if (dataLine == 0) {
            iss >> CathConfig.p0[0] >> CathConfig.p0[1] >> CathConfig.p0[2];
        } else if (dataLine >= 1 && matRow < 3) {
            iss >> CathConfig.R0(matRow, 0)
                >> CathConfig.R0(matRow, 1)
                >> CathConfig.R0(matRow, 2);
            ++matRow;
        }
        ++dataLine;
    }
    std::cout << "CRM: Loaded configuration from " << filename
              << "  p0=(" << CathConfig.p0.transpose() << ")\n";
    return true;
}

// ----------------------------------------------------------------------------
// CRM Forward Kinematics
//
// Physics model: uniform-curvature Cosserat arc
//
// Actuation mapping (symmetric differential-pair scheme):
//   kappa_1 = k_act * (i[1] - i[0])   -- bending component 1
//   kappa_2 = k_act * (i[3] - i[2])   -- bending component 2
//   (i[4], i[5] are reserved for second section / future use)
//
// The catheter is modelled as a planar arc with:
//   total curvature  kappa = sqrt(kappa_1^2 + kappa_2^2)  [1/mm]
//   bending direction  phi = atan2(kappa_2, kappa_1)       [rad]
//
// Starting from the base pose (CathConfig.p0, CathConfig.R0), the arc in
// the body frame has parametric equation:
//   r_body(s) = [ (1 - cos(kappa*s))*cos(phi) / kappa,
//                 (1 - cos(kappa*s))*sin(phi) / kappa,
//                 sin(kappa*s) / kappa ]            for kappa > 0
//   r_body(s) = [ 0, 0, s ]                        for kappa ≈ 0
// World-frame position:  p(s) = p0 + R0 * r_body(s)
// ----------------------------------------------------------------------------
VectorXd CRM_ForwardKinematics(
    const VectorXd&            control_inputs,
    CRMForwardKinematicsData&  FKParams,
    double&                    PotentialEnergy,
    bool&                      localmin
) {
    // --- Extract actuation currents and inserted length ---
    auto safeGet = [&](int idx, double def) -> double {
        return (control_inputs.size() > idx) ? control_inputs[idx] : def;
    };
    const double i1 = safeGet(0, 0.1);
    const double i2 = safeGet(1, 0.1);
    const double i3 = safeGet(2, 0.1);
    const double i4 = safeGet(3, 0.1);
    // i5 = safeGet(4, 0.1);   reserved
    // i6 = safeGet(5, 0.1);   reserved
    double L = safeGet(6, 100.0);

    // Clamp inserted length to valid range
    if (L < 0.0)                      L = 0.0;
    if (L > CathParams.totalLength)   L = CathParams.totalLength;

    // --- Compute net curvature from differential coil currents ---
    const double k      = CathParams.k_act;
    const double kappa1 = k * (i2 - i1);   // component along body x
    const double kappa2 = k * (i4 - i3);   // component along body y
    const double kappa  = std::sqrt(kappa1 * kappa1 + kappa2 * kappa2);
    const double phi    = std::atan2(kappa2, kappa1);

    // --- Determine number of output markers ---
    double stepSize = FKParams.integrationStepSize;
    if (stepSize <= 0.0) stepSize = 2.0;
    int numMarkers = (L > 0.0) ? static_cast<int>(L / stepSize) + 1 : 2;
    if (numMarkers < 2) numMarkers = 2;

    FKParams.ReportedMarkerPos.resize(numMarkers, 3);

    const Vector3d& p0 = CathConfig.p0;
    const Matrix3d& R0 = CathConfig.R0;

    // --- Generate arc positions ---
    for (int j = 0; j < numMarkers; ++j) {
        const double s = (numMarkers > 1)
                       ? (static_cast<double>(j) * L / (numMarkers - 1))
                       : 0.0;

        Vector3d r_body;
        if (kappa < 1.0e-9) {
            // Straight catheter along body z-axis
            r_body = Vector3d(0.0, 0.0, s);
        } else {
            const double theta = kappa * s;
            r_body = Vector3d(
                (1.0 - std::cos(theta)) * std::cos(phi) / kappa,
                (1.0 - std::cos(theta)) * std::sin(phi) / kappa,
                std::sin(theta) / kappa
            );
        }

        const Vector3d r_world = p0 + R0 * r_body;
        FKParams.ReportedMarkerPos.row(j) = r_world.transpose();
    }

    // --- Elastic potential energy (simple Euler–Bernoulli estimate) ---
    PotentialEnergy = 0.5 * CathParams.EI * kappa * kappa * L;

    localmin = false;

    // --- Return vector: first 3 elements = tip position ---
    VectorXd solution(3);
    solution[0] = FKParams.ReportedMarkerPos(numMarkers - 1, 0);
    solution[1] = FKParams.ReportedMarkerPos(numMarkers - 1, 1);
    solution[2] = FKParams.ReportedMarkerPos(numMarkers - 1, 2);
    return solution;
}

} // namespace CRMCatheterModel
