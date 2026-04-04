#pragma once

// Use Eigen/Core (basic matrix/vector types and operations).
// This avoids GCC 13 / Eigen 3.4.0 compatibility issues that arise when
// including the full Eigen/Dense header (which pulls in advanced solvers
// not required by this forward-kinematics implementation).
#include <eigen5/Eigen/Core>
#include <string>

using namespace Eigen;

namespace CRMCatheterModel {

// Contact mode constants
const int FREE_TIP     = 0;
const int CONTACT_TIP  = 1;

// Number of electromagnetic actuation coil sets
const int NUM_ACT_SET  = 2;

// Catheter mechanical parameters (loaded from CatheterParameterSet file)
struct CatheterModelParams {
    double totalLength;     // total catheter length (mm)
    double insertDiameter;  // insertion tube outer diameter (mm)
    double EI;              // bending stiffness (N·mm²)
    double GJ;              // torsional stiffness (N·mm²)
    double k_act;           // actuation gain: curvature per unit differential current (1/(mm·A))
};

// Catheter spatial configuration (loaded from CatheterSpatialConfiguration file)
struct CatheterSpatialConfiguration {
    Vector3d p0;   // base position in world frame (mm)
    Matrix3d R0;   // base-to-world rotation matrix (columns = x,y,z body axes)
};

// Forward kinematics computation data (input/output)
struct CRMForwardKinematicsData {
    double    integrationStepSize;   // arc-length step used for integration (mm)
    int       contactMode;           // FREE_TIP or CONTACT_TIP
    MatrixXd  ReportedMarkerPos;     // (numMarkers × 3) world-frame marker positions
    VectorXd  initialGuess;          // initial guess for the BVP shooting variables
};

// Global catheter model state (populated by Load_* functions below)
extern CatheterModelParams          CathParams;
extern CatheterSpatialConfiguration CathConfig;

// -------------------------------------------------------------------------
// Load catheter mechanical parameter set from a text file.
// Falls back to built-in defaults if the file cannot be opened.
// Returns true on success.
// -------------------------------------------------------------------------
bool Load_CRMCatheterModelParams(const std::string& filename);

// -------------------------------------------------------------------------
// Load catheter spatial (base pose) configuration from a text file.
// Falls back to identity pose if the file cannot be opened.
// Returns true on success.
// -------------------------------------------------------------------------
bool Load_CatheterConfiguration(const std::string& filename);

// -------------------------------------------------------------------------
// Compute forward kinematics for the catheter.
//
// control_inputs layout (length = NUM_ACT_SET*3 + 1 = 7):
//   [i1, i2, i3, i4, i5, i6, insertedLength]
//   i1..i6      : actuation coil currents (A)
//   insertedLength : portion of catheter inside the patient (mm)
//
// On return:
//   FKParams.ReportedMarkerPos is filled with world-frame positions of
//   markers evenly spaced along the arc (rows = markers, cols = x,y,z).
//   PotentialEnergy is set to the elastic potential energy (N·mm).
//   localmin is set to false for this implementation.
//
// Returns a VectorXd whose first 3 entries are the tip position [x,y,z].
// -------------------------------------------------------------------------
VectorXd CRM_ForwardKinematics(
    const VectorXd&            control_inputs,
    CRMForwardKinematicsData&  FKParams,
    double&                    PotentialEnergy,
    bool&                      localmin
);

} // namespace CRMCatheterModel
