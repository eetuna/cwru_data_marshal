"""
CRM Forward Kinematics Examples - Python version
Equivalent to matlab/TestCppMEXFiles.m

Usage:
    python test_crm.py

Prerequisites:
    - Build the crm Python module (see python/CMakeLists.txt and python/README.md)
    - The built crm module (.pyd or .so) must be on the Python path
      (e.g., in the same directory as this script or in the build directory)
"""

import numpy as np

# Import the CRM nanobind module
import crm

# =========================================================================
print("### CRM Forward Kinematics Examples...")
print("Free Space Deflection Example:\n")

# *** Load parameters from file
# This step would typically needs to be executed only once
#   Physical Description of the Catheter
CathParams = crm.Load_CRMCatheterModelParams("../catheterdata/CatheterParameterSet_2.txt")
#   Catheter Configuration in spatial coordinates
CathConfig = crm.Load_CatheterConfiguration("../catheterdata/CatheterSpatialConfiguration_1.txt")

# *** Other External variables
ContactMode = crm.ContactModeType.FREE_TIP
TipForce = np.array([0.0, 0.0, 0.0])
TipConstraintPoint = np.array([0.0, 0.0, 0.0])
FinalValueOnly = False  # We want the localization coil locations too

# *** Numerical Computation Parameters
IntegrationStepSize = 0.2
deltau0_initialguess = np.array([0.0, 0.0, 0.0])
ftip_initialguess = np.array([0.0, 0.0, 0.0])

# *** Control Inputs
InsertedLength = 100.0
ActuationCurrents = np.array([0.100, 0.100, 0.100, 0.100, 0.100, 0.100])
control_inputs = np.concatenate([ActuationCurrents, [InsertedLength]])

# *** Solve the Forward Kinematics
(FKsolution, PotentialEnergy, ReportedMarkerPos, ReportedCoilPos,
 ReportedCoilOrient, localmin) = crm.CRM_ForwardKinematics(
    control_inputs, CathParams, CathConfig,
    ContactMode, TipConstraintPoint, TipForce,
    deltau0_initialguess, ftip_initialguess,
    IntegrationStepSize, FinalValueOnly)

# *** Calculate the Analytical Jacobian
Ja = crm.CRM_FKJacobian_Analytical(
    control_inputs, FKsolution, CathParams, CathConfig,
    ContactMode, TipConstraintPoint, TipForce,
    deltau0_initialguess, ftip_initialguess,
    IntegrationStepSize, FinalValueOnly)

print("p[0..2], R[0..8], deltau0[0..2]:")
print(FKsolution)
print(f"\nPE: {PotentialEnergy}")
print(f"\nReportedMarkerPos:\n{ReportedMarkerPos}")
print(f"\nReportedCoilPos:\n{ReportedCoilPos}")
print(f"\nReportedCoilOrient:\n{ReportedCoilOrient}")
print(f"\nJa: [ dpdz, dpdft; ws_dz, ws_dft ]\n{Ja}")

# =========================================================================
print("\n\n### CRM Forward Kinematics Examples...")
print("Constrained Tip Deflection Example:\n")

# *** Update parameters for constrained tip
TipConstraintPoint = np.array([6.12, 40.57, 93.96])
ContactMode = crm.ContactModeType.FIXED_TIP
InsertedLength = 104.0
control_inputs = np.concatenate([ActuationCurrents, [InsertedLength]])

# *** Solve the Forward Kinematics
(FKsolution, PotentialEnergy, ReportedMarkerPos, ReportedCoilPos,
 ReportedCoilOrient, localmin) = crm.CRM_ForwardKinematics(
    control_inputs, CathParams, CathConfig,
    ContactMode, TipConstraintPoint, TipForce,
    deltau0_initialguess, ftip_initialguess,
    IntegrationStepSize, FinalValueOnly)

# *** Calculate the Analytical Jacobian
Ja = crm.CRM_FKJacobian_Analytical(
    control_inputs, FKsolution, CathParams, CathConfig,
    ContactMode, TipConstraintPoint, TipForce,
    deltau0_initialguess, ftip_initialguess,
    IntegrationStepSize, FinalValueOnly)

print("p[0..2], R[0..8], deltau0[0..2], ftip[0..2]:")
print(FKsolution)
print(f"\nPE: {PotentialEnergy}")
print(f"\nReportedMarkerPos:\n{ReportedMarkerPos}")
print(f"\nReportedCoilPos:\n{ReportedCoilPos}")
print(f"\nReportedCoilOrient:\n{ReportedCoilOrient}")
print(f"\nJa: [ dftip_dz ]\n{Ja}")


# =========================================================================
print("\n\n### CRM Catheter Class Examples...")
print("Free Space Deflection Example (using CRM_Catheter class):\n")

# *** Instantiate the CRM_Catheter class
CathParams2 = crm.Load_CRMCatheterModelParams("../catheterdata/CatheterParameterSet_2.txt")
CathConfig2 = crm.Load_CatheterConfiguration("../catheterdata/CatheterSpatialConfiguration_1.txt")
Catheter = crm.CRM_Catheter(CathParams2, CathConfig2)
Catheter.IntegrationStepSize = 0.2

# *** Control Inputs
InsertedLength = 100.0
ActuationCurrents = np.array([0.100, 0.100, 0.100, 0.100, 0.100, 0.100])
control_inputs = np.concatenate([ActuationCurrents, [InsertedLength]])
CalculateMarkers = True

# *** FREE TIP - Forward Kinematics (incremental)
ptip, Rtip, deltau0, PE, localmin = Catheter.ForwardKinematicsFree(control_inputs, CalculateMarkers)

print("FK Output -- ptip, Rtip, deltau0:")
print(f"ptip:\n{ptip}")
print(f"Rtip:\n{Rtip}")
print(f"deltau0:\n{deltau0}")
print(f"PE: {PE}")
print(f"localmin: {localmin}")
print("----")

print(f"MarkerPos:\n{Catheter.MarkerPos}")
print(f"CoilPos:\n{Catheter.CoilPos}")
print(f"CoilOrient:\n{Catheter.CoilOrient}")
print("----")

# *** Jacobian calculation
Ja = Catheter.FKJacobian_Analytical()
print(f"Ja:\n{Ja}")
print("----")

# *** CONSTRAINED TIP
print("\nConstrained Tip Deflection Example:\n")
Catheter.ResetInitialGuesses()
TipConstraintPoint = np.array([6.12, 40.57, 93.96])
control_inputs_ct = np.concatenate([ActuationCurrents, [104.0]])

ptip, Rtip, deltau0, ftip, PE, localmin = Catheter.ForwardKinematicsContact(
    control_inputs_ct, TipConstraintPoint, CalculateMarkers)

print("FK Output -- ptip, Rtip, deltau0, ftip:")
print(f"ptip:\n{ptip}")
print(f"Rtip:\n{Rtip}")
print(f"deltau0:\n{deltau0}")
print(f"ftip:\n{ftip}")
print(f"PE: {PE}")
print(f"localmin: {localmin}")
print("----")

print(f"MarkerPos:\n{Catheter.MarkerPos}")
print(f"CoilPos:\n{Catheter.CoilPos}")
print(f"CoilOrient:\n{Catheter.CoilOrient}")
print("----")

Ja = Catheter.FKJacobian_Analytical()
print(f"Ja:\n{Ja}")
