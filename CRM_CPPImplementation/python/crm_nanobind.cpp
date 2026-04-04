/*
 * Python bindings for CRM Catheter Kinematics Library using nanobind
 *
 * This module exposes the following to Python:
 *   - Load_CRMCatheterModelParams()  -- load catheter physical parameters from file
 *   - Load_CatheterConfiguration()   -- load catheter spatial configuration from file
 *   - CRM_ForwardKinematics()        -- forward kinematics (Eigen version)
 *   - CRM_FKJacobian_Analytical()    -- analytical Jacobian
 *   - CRM_FKJacobian_Numerical()     -- numerical (finite difference) Jacobian
 *   - CRM_Catheter class             -- encapsulated catheter kinematics
 *   - Enums: CatheterSegmentType, ContactModeType
 *   - Structs: CatheterConfiguration, CRMForwardKinematicsData, CRMCatheterModelParams
 */

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/eigen/dense.h>

#include "CRM.hpp"

namespace nb = nanobind;
using namespace nb::literals;
using namespace CRMCatheterModel;


NB_MODULE(crm, m) {

    m.doc() = "CRM Catheter Kinematics Library - Python bindings";

    // Expose compile-time constant
    m.attr("NUM_ACT_SET") = NUM_ACT_SET;
    m.attr("NUM_STATES") = NUM_STATES;

    // ---------------------------------------------------------------
    // Enums
    // ---------------------------------------------------------------

    nb::enum_<CatheterSegmentType>(m, "CatheterSegmentType")
        .value("FLEXIBLE", CatheterSegmentType::FLEXIBLE)
        .value("RIGID_WITH_ACTUATOR", CatheterSegmentType::RIGID_WITH_ACTUATOR)
        .value("RIGID", CatheterSegmentType::RIGID);

    nb::enum_<ContactModeType>(m, "ContactModeType")
        .value("FREE_TIP", ContactModeType::FREE_TIP)
        .value("FIXED_TIP", ContactModeType::FIXED_TIP);

    // ---------------------------------------------------------------
    // CRMCatheterModelParams
    // ---------------------------------------------------------------

    nb::class_<CRMCatheterModelParams>(m, "CRMCatheterModelParams")
        .def(nb::init<int32_t, int32_t, int32_t, int32_t>(),
             "no_flex"_a, "no_rigid"_a, "no_act"_a, "no_loc"_a)
        .def(nb::init<const CRMCatheterModelParams&>())  // copy constructor (deep copy)
        .def_ro("no_flex_seg", &CRMCatheterModelParams::no_flex_seg)
        .def_ro("no_rigid_seg", &CRMCatheterModelParams::no_rigid_seg)
        .def_ro("no_act_set", &CRMCatheterModelParams::no_act_set)
        .def_ro("no_segments", &CRMCatheterModelParams::no_segments)
        .def_ro("no_locmarkers", &CRMCatheterModelParams::no_locmarkers)
        // Expose raw pointer arrays as copies (Eigen vectors/matrices)
        .def_prop_ro("SegLengths", [](CRMCatheterModelParams& self) {
            return Eigen::Map<VectorXd>(self.SegLengths, self.no_segments);
        })
        .def_prop_ro("rho", [](CRMCatheterModelParams& self) {
            return Eigen::Map<VectorXd>(self.rho, self.no_segments);
        })
        .def_prop_ro("InnerRadius", [](CRMCatheterModelParams& self) {
            return Eigen::Map<VectorXd>(self.InnerRadius, self.no_flex_seg);
        })
        .def_prop_ro("OuterRadius", [](CRMCatheterModelParams& self) {
            return Eigen::Map<VectorXd>(self.OuterRadius, self.no_flex_seg);
        })
        .def_prop_ro("YoungsModulus", [](CRMCatheterModelParams& self) {
            return Eigen::Map<VectorXd>(self.YoungsModulus, self.no_flex_seg);
        })
        .def_prop_ro("ShearModulus", [](CRMCatheterModelParams& self) {
            return Eigen::Map<VectorXd>(self.ShearModulus, self.no_flex_seg);
        })
        .def_prop_ro("ActMass", [](CRMCatheterModelParams& self) {
            return Eigen::Map<VectorXd>(self.ActMass, self.no_act_set);
        })
        .def_prop_ro("LocMarkers", [](CRMCatheterModelParams& self) {
            return Eigen::Map<VectorXd>(self.LocMarkers, self.no_locmarkers);
        })
        .def_prop_ro("ustar", [](CRMCatheterModelParams& self) {
            // ustar is double(*)[3], laid out contiguously as (no_flex_seg x 3) row-major
            Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic, 3, Eigen::RowMajor>> m(
                &(self.ustar[0][0]), self.no_flex_seg, 3);
            Eigen::MatrixXd result = m;  // copy to standard column-major
            return result;
        })
        .def_prop_ro("CoilAlignmentAngles", [](CRMCatheterModelParams& self) {
            Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic, 2, Eigen::RowMajor>> m(
                &(self.CoilAlignmentAngles[0][0]), self.no_act_set, 2);
            Eigen::MatrixXd result = m;
            return result;
        })
        .def_prop_ro("CoilTurnAreaMat", [](CRMCatheterModelParams& self) {
            Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic, 9, Eigen::RowMajor>> m(
                &(self.CoilTurnAreaMat[0][0]), self.no_act_set, 9);
            Eigen::MatrixXd result = m;
            return result;
        })
        ;

    // ---------------------------------------------------------------
    // CatheterConfiguration
    // ---------------------------------------------------------------

    nb::class_<CatheterConfiguration>(m, "CatheterConfiguration")
        .def(nb::init<>())
        .def_prop_rw("B0",
            [](CatheterConfiguration& self) {
                return Eigen::Map<Vector3d>(self.B0);
            },
            [](CatheterConfiguration& self, const Vector3d& v) {
                Eigen::Map<Vector3d>(self.B0) = v;
            })
        .def_prop_rw("g",
            [](CatheterConfiguration& self) {
                return Eigen::Map<Vector3d>(self.g);
            },
            [](CatheterConfiguration& self, const Vector3d& v) {
                Eigen::Map<Vector3d>(self.g) = v;
            })
        .def_prop_rw("p0",
            [](CatheterConfiguration& self) {
                return Eigen::Map<Vector3d>(self.p0);
            },
            [](CatheterConfiguration& self, const Vector3d& v) {
                Eigen::Map<Vector3d>(self.p0) = v;
            })
        .def_prop_rw("R0",
            [](CatheterConfiguration& self) {
                // R0 is stored row-major in a flat double[9]
                Eigen::Map<Eigen::Matrix<double, 3, 3, Eigen::RowMajor>> Rmap(self.R0);
                Eigen::Matrix3d R = Rmap;
                return R;
            },
            [](CatheterConfiguration& self, const Eigen::Matrix3d& R) {
                Eigen::Map<Eigen::Matrix<double, 3, 3, Eigen::RowMajor>>(self.R0) = R;
            })
        ;

    // ---------------------------------------------------------------
    // CRMForwardKinematicsData
    // ---------------------------------------------------------------

    nb::class_<CRMForwardKinematicsData>(m, "CRMForwardKinematicsData")
        .def(nb::init<>())
        .def_rw("ContactMode", &CRMForwardKinematicsData::ContactMode)
        .def_rw("IntegrationStepSize", &CRMForwardKinematicsData::IntegrationStepSize)
        .def_rw("FinalValueOnly", &CRMForwardKinematicsData::FinalValueOnly)
        .def_prop_rw("TipConstraintPoint",
            [](CRMForwardKinematicsData& self) {
                return Eigen::Map<Vector3d>(self.TipConstraintPoint);
            },
            [](CRMForwardKinematicsData& self, const Vector3d& v) {
                Eigen::Map<Vector3d>(self.TipConstraintPoint) = v;
            })
        .def_prop_rw("TipForce",
            [](CRMForwardKinematicsData& self) {
                return Eigen::Map<Vector3d>(self.TipForce);
            },
            [](CRMForwardKinematicsData& self, const Vector3d& v) {
                Eigen::Map<Vector3d>(self.TipForce) = v;
            })
        .def_prop_rw("deltau0_initialguess",
            [](CRMForwardKinematicsData& self) {
                return Eigen::Map<Vector3d>(self.deltau0_initialguess);
            },
            [](CRMForwardKinematicsData& self, const Vector3d& v) {
                Eigen::Map<Vector3d>(self.deltau0_initialguess) = v;
            })
        .def_prop_rw("ftip_initialguess",
            [](CRMForwardKinematicsData& self) {
                return Eigen::Map<Vector3d>(self.ftip_initialguess);
            },
            [](CRMForwardKinematicsData& self, const Vector3d& v) {
                Eigen::Map<Vector3d>(self.ftip_initialguess) = v;
            })
        // setup() must be called after assigning CathParams and CathConfig
        // to wire up the internal pointers and allocate marker/coil output arrays.
        // This replaces the raw pointer assignments done in C++.
        ;

    // ---------------------------------------------------------------
    // Free functions: Load parameters from file
    // ---------------------------------------------------------------

    m.def("Load_CRMCatheterModelParams",
        [](const std::string& path) {
            return Load_CRMCatheterModelParams(path.c_str());
        },
        "path"_a,
        "Load catheter model parameters from file. Returns CRMCatheterModelParams.");

    m.def("Load_CatheterConfiguration",
        [](const std::string& path) {
            return Load_CatheterConfiguration(path.c_str());
        },
        "path"_a,
        "Load catheter spatial configuration from file. Returns CatheterConfiguration.");

    // ---------------------------------------------------------------
    // CRM_ForwardKinematics (Eigen version)
    //   Returns (out_y, PotentialEnergy, ReportedMarkerPos, ReportedCoilPos, ReportedCoilOrient, localmin)
    // ---------------------------------------------------------------

    m.def("CRM_ForwardKinematics",
        [](const VectorXd& in_x,
           CRMCatheterModelParams& CathParams,
           CatheterConfiguration& CathConfig,
           ContactModeType ContactMode,
           const Vector3d& TipConstraintPoint,
           const Vector3d& TipForce,
           const Vector3d& deltau0_initialguess,
           const Vector3d& ftip_initialguess,
           double IntegrationStepSize,
           bool FinalValueOnly)
        {
            CRMForwardKinematicsData Params;
            Params.CathParams = &CathParams;
            Params.CathConfig = &CathConfig;
            Params.ContactMode = ContactMode;
            for (int i = 0; i < 3; i++) {
                Params.TipConstraintPoint[i] = TipConstraintPoint(i);
                Params.TipForce[i] = TipForce(i);
                Params.deltau0_initialguess[i] = deltau0_initialguess(i);
                Params.ftip_initialguess[i] = ftip_initialguess(i);
            }
            Params.IntegrationStepSize = IntegrationStepSize;
            Params.FinalValueOnly = FinalValueOnly;

            // Allocate output arrays for markers and coils
            int no_loc = CathParams.no_locmarkers;
            int no_act = CathParams.no_act_set;
            std::vector<double> markerBuf(no_loc * 3, 0.0);
            std::vector<double> coilPosBuf(no_act * 3, 0.0);
            std::vector<double> coilOrientBuf(no_act * 9, 0.0);
            Params.ReportedMarkerPos = reinterpret_cast<double(*)[3]>(markerBuf.data());
            Params.ReportedCoilPos = reinterpret_cast<double(*)[3]>(coilPosBuf.data());
            Params.ReportedCoilOrient = reinterpret_cast<double(*)[9]>(coilOrientBuf.data());

            double PotentialEnergy = 0.0;
            int localmin = 0;
            VectorXd out_y = CRM_ForwardKinematics(in_x, Params, PotentialEnergy, localmin);

            // Convert marker positions to Eigen matrix (no_loc x 3)
            Eigen::MatrixXd ReportedMarkerPos(no_loc, 3);
            for (int i = 0; i < no_loc; i++)
                for (int j = 0; j < 3; j++)
                    ReportedMarkerPos(i, j) = Params.ReportedMarkerPos[i][j];

            // Convert coil positions to Eigen matrix (no_act x 3)
            Eigen::MatrixXd ReportedCoilPos(no_act, 3);
            for (int i = 0; i < no_act; i++)
                for (int j = 0; j < 3; j++)
                    ReportedCoilPos(i, j) = Params.ReportedCoilPos[i][j];

            // Convert coil orientations to list of 3x3 matrices packed into (no_act x 9) row-major
            Eigen::MatrixXd ReportedCoilOrient(no_act, 9);
            for (int i = 0; i < no_act; i++)
                for (int j = 0; j < 9; j++)
                    ReportedCoilOrient(i, j) = Params.ReportedCoilOrient[i][j];

            return std::make_tuple(out_y, PotentialEnergy, ReportedMarkerPos, ReportedCoilPos, ReportedCoilOrient, localmin);
        },
        "in_x"_a,
        "CathParams"_a,
        "CathConfig"_a,
        "ContactMode"_a,
        "TipConstraintPoint"_a,
        "TipForce"_a,
        "deltau0_initialguess"_a,
        "ftip_initialguess"_a,
        "IntegrationStepSize"_a,
        "FinalValueOnly"_a,
        "CRM Forward Kinematics.\n"
        "Returns (FKsolution, PotentialEnergy, ReportedMarkerPos, ReportedCoilPos, ReportedCoilOrient, localmin)."
    );

    // ---------------------------------------------------------------
    // CRM_FKJacobian_Analytical
    //   Returns Jacobian matrix
    // ---------------------------------------------------------------

    m.def("CRM_FKJacobian_Analytical",
        [](const VectorXd& in_x,
           const VectorXd& in_FKouty,
           CRMCatheterModelParams& CathParams,
           CatheterConfiguration& CathConfig,
           ContactModeType ContactMode,
           const Vector3d& TipConstraintPoint,
           const Vector3d& TipForce,
           const Vector3d& deltau0_initialguess,
           const Vector3d& ftip_initialguess,
           double IntegrationStepSize,
           bool FinalValueOnly)
        {
            CRMForwardKinematicsData Params;
            Params.CathParams = &CathParams;
            Params.CathConfig = &CathConfig;
            Params.ContactMode = ContactMode;
            for (int i = 0; i < 3; i++) {
                Params.TipConstraintPoint[i] = TipConstraintPoint(i);
                Params.TipForce[i] = TipForce(i);
                Params.deltau0_initialguess[i] = deltau0_initialguess(i);
                Params.ftip_initialguess[i] = ftip_initialguess(i);
            }
            Params.IntegrationStepSize = IntegrationStepSize;
            Params.FinalValueOnly = FinalValueOnly;

            // Allocate output arrays (needed by internal code even though Jacobian doesn't report them)
            int no_loc = CathParams.no_locmarkers;
            int no_act = CathParams.no_act_set;
            std::vector<double> markerBuf(no_loc * 3, 0.0);
            std::vector<double> coilPosBuf(no_act * 3, 0.0);
            std::vector<double> coilOrientBuf(no_act * 9, 0.0);
            Params.ReportedMarkerPos = reinterpret_cast<double(*)[3]>(markerBuf.data());
            Params.ReportedCoilPos = reinterpret_cast<double(*)[3]>(coilPosBuf.data());
            Params.ReportedCoilOrient = reinterpret_cast<double(*)[9]>(coilOrientBuf.data());

            MatrixXd Ja = CRM_FKJacobian_Analytical(in_x, in_FKouty, Params);
            return Ja;
        },
        "in_x"_a,
        "in_FKouty"_a,
        "CathParams"_a,
        "CathConfig"_a,
        "ContactMode"_a,
        "TipConstraintPoint"_a,
        "TipForce"_a,
        "deltau0_initialguess"_a,
        "ftip_initialguess"_a,
        "IntegrationStepSize"_a,
        "FinalValueOnly"_a,
        "CRM Forward Kinematics Analytical Jacobian.\n"
        "Returns Jacobian matrix."
    );

    // ---------------------------------------------------------------
    // CRM_FKJacobian_Numerical
    //   Returns (Jacobian, out_y, localmin)
    // ---------------------------------------------------------------

    m.def("CRM_FKJacobian_Numerical",
        [](const VectorXd& in_x,
           CRMCatheterModelParams& CathParams,
           CatheterConfiguration& CathConfig,
           ContactModeType ContactMode,
           const Vector3d& TipConstraintPoint,
           const Vector3d& TipForce,
           const Vector3d& deltau0_initialguess,
           const Vector3d& ftip_initialguess,
           double IntegrationStepSize,
           bool FinalValueOnly)
        {
            CRMForwardKinematicsData Params;
            Params.CathParams = &CathParams;
            Params.CathConfig = &CathConfig;
            Params.ContactMode = ContactMode;
            for (int i = 0; i < 3; i++) {
                Params.TipConstraintPoint[i] = TipConstraintPoint(i);
                Params.TipForce[i] = TipForce(i);
                Params.deltau0_initialguess[i] = deltau0_initialguess(i);
                Params.ftip_initialguess[i] = ftip_initialguess(i);
            }
            Params.IntegrationStepSize = IntegrationStepSize;
            Params.FinalValueOnly = FinalValueOnly;

            int no_loc = CathParams.no_locmarkers;
            int no_act = CathParams.no_act_set;
            std::vector<double> markerBuf(no_loc * 3, 0.0);
            std::vector<double> coilPosBuf(no_act * 3, 0.0);
            std::vector<double> coilOrientBuf(no_act * 9, 0.0);
            Params.ReportedMarkerPos = reinterpret_cast<double(*)[3]>(markerBuf.data());
            Params.ReportedCoilPos = reinterpret_cast<double(*)[3]>(coilPosBuf.data());
            Params.ReportedCoilOrient = reinterpret_cast<double(*)[9]>(coilOrientBuf.data());

            VectorXd out_y;
            int localmin = 0;
            MatrixXd Jn = CRM_FKJacobian_Numerical(in_x, out_y, Params, localmin);
            return std::make_tuple(Jn, out_y, localmin);
        },
        "in_x"_a,
        "CathParams"_a,
        "CathConfig"_a,
        "ContactMode"_a,
        "TipConstraintPoint"_a,
        "TipForce"_a,
        "deltau0_initialguess"_a,
        "ftip_initialguess"_a,
        "IntegrationStepSize"_a,
        "FinalValueOnly"_a,
        "CRM Forward Kinematics Numerical Jacobian.\n"
        "Returns (Jacobian, FKsolution, localmin)."
    );

    // ---------------------------------------------------------------
    // CRM_Catheter class
    // ---------------------------------------------------------------

    nb::class_<CRM_Catheter>(m, "CRM_Catheter")
        .def(nb::init<CRMCatheterModelParams, CatheterConfiguration>(),
             "CathParams"_a, "CathConfig"_a)

        .def_prop_rw("IntegrationStepSize",
            [](CRM_Catheter& self) { return self.IntegrationStepSize; },
            [](CRM_Catheter& self, double v) { self.IntegrationStepSize = v; })

        .def("ResetInitialGuesses", &CRM_Catheter::ResetInitialGuesses)

        // ForwardKinematicsFree (incremental version)
        .def("ForwardKinematicsFree",
            [](CRM_Catheter& self, const VectorXd& ActuationVector, bool CalculateMarkers) {
                double PE = 0.0;
                int localmin = 0;
                auto [ptip, Rtip, deltau0] = self.ForwardKinematicsFree(ActuationVector, CalculateMarkers, PE, localmin);
                return std::make_tuple(ptip, Rtip, deltau0, PE, localmin);
            },
            "ActuationVector"_a, "CalculateMarkers"_a,
            "Forward kinematics for free tip (incremental).\n"
            "Returns (ptip, Rtip, deltau0, PotentialEnergy, localmin).")

        // ForwardKinematicsFree (full version with TipForce and initial guess)
        .def("ForwardKinematicsFree",
            [](CRM_Catheter& self, const VectorXd& ActuationVector,
               const Vector3d& TipForce, const Vector3d& deltau0_initialguess,
               bool CalculateMarkers) {
                double PE = 0.0;
                int localmin = 0;
                auto [ptip, Rtip, deltau0] = self.ForwardKinematicsFree(
                    ActuationVector, TipForce, deltau0_initialguess, CalculateMarkers, PE, localmin);
                return std::make_tuple(ptip, Rtip, deltau0, PE, localmin);
            },
            "ActuationVector"_a, "TipForce"_a, "deltau0_initialguess"_a, "CalculateMarkers"_a,
            "Forward kinematics for free tip (with explicit initial guess and tip force).\n"
            "Returns (ptip, Rtip, deltau0, PotentialEnergy, localmin).")

        // ForwardKinematicsContact (incremental version)
        .def("ForwardKinematicsContact",
            [](CRM_Catheter& self, const VectorXd& ActuationVector,
               const Vector3d& TipConstraintPoint, bool CalculateMarkers) {
                double PE = 0.0;
                int localmin = 0;
                auto [ptip, Rtip, deltau0, ftip] = self.ForwardKinematicsContact(
                    ActuationVector, TipConstraintPoint, CalculateMarkers, PE, localmin);
                return std::make_tuple(ptip, Rtip, deltau0, ftip, PE, localmin);
            },
            "ActuationVector"_a, "TipConstraintPoint"_a, "CalculateMarkers"_a,
            "Forward kinematics for constrained tip (incremental).\n"
            "Returns (ptip, Rtip, deltau0, ftip, PotentialEnergy, localmin).")

        // ForwardKinematicsContact (full version with initial guesses)
        .def("ForwardKinematicsContact",
            [](CRM_Catheter& self, const VectorXd& ActuationVector,
               const Vector3d& TipConstraintPoint,
               const Vector3d& deltau0_initialguess,
               const Vector3d& ftip_initialguess,
               bool CalculateMarkers) {
                double PE = 0.0;
                int localmin = 0;
                auto [ptip, Rtip, deltau0, ftip] = self.ForwardKinematicsContact(
                    ActuationVector, TipConstraintPoint, deltau0_initialguess, ftip_initialguess,
                    CalculateMarkers, PE, localmin);
                return std::make_tuple(ptip, Rtip, deltau0, ftip, PE, localmin);
            },
            "ActuationVector"_a, "TipConstraintPoint"_a,
            "deltau0_initialguess"_a, "ftip_initialguess"_a, "CalculateMarkers"_a,
            "Forward kinematics for constrained tip (with explicit initial guesses).\n"
            "Returns (ptip, Rtip, deltau0, ftip, PotentialEnergy, localmin).")

        // FKJacobian_Analytical
        .def("FKJacobian_Analytical", &CRM_Catheter::FKJacobian_Analytical,
             "Analytical Jacobian of the last FK calculation.\n"
             "Must be called after ForwardKinematicsFree or ForwardKinematicsContact.")

        // MarkerPos (read-only, returns copy as Eigen matrix)
        .def_prop_ro("MarkerPos",
            [](CRM_Catheter& self) {
                int n = self.CathParams.no_locmarkers;
                Eigen::MatrixXd M(n, 3);
                for (int i = 0; i < n; i++)
                    for (int j = 0; j < 3; j++)
                        M(i, j) = self.MarkerPos[i][j];
                return M;
            },
            "Localization marker positions (no_locmarkers x 3 matrix).")

        // CoilPos (read-only, returns copy as Eigen matrix)
        .def_prop_ro("CoilPos",
            [](CRM_Catheter& self) {
                int n = self.CathParams.no_act_set;
                Eigen::MatrixXd M(n, 3);
                for (int i = 0; i < n; i++)
                    for (int j = 0; j < 3; j++)
                        M(i, j) = self.CoilPos[i][j];
                return M;
            },
            "Coil positions (no_act_set x 3 matrix).")

        // CoilOrient (read-only, returns copy as Eigen matrix)
        .def_prop_ro("CoilOrient",
            [](CRM_Catheter& self) {
                int n = self.CathParams.no_act_set;
                Eigen::MatrixXd M(n, 9);
                for (int i = 0; i < n; i++)
                    for (int j = 0; j < 9; j++)
                        M(i, j) = self.CoilOrient[i][j];
                return M;
            },
            "Coil orientations (no_act_set x 9 matrix, each row is a 3x3 rotation matrix in row-major order).")

        // Expose CathParams and CathConfig for inspection
        .def_ro("CathParams", &CRM_Catheter::CathParams)
        .def_rw("CathConfig", &CRM_Catheter::CathConfig)
        ;
}
