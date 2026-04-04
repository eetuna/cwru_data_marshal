#pragma once
#include <tuple>

//
// ---------------------------------------------------------
// This file contains the definition for the 
//    analytic Jacobian of the IVP solver of the
//    Cosserot Rod Model of the Catheter
// ---------------------------------------------------------
//

//
// Implementation Note - 8/11/2022 MCC:
//    The CRMJacobianIntegrand functions assumes that:
//	    - the distributed moment load density l is assumed to be zero
//      - the K and Kinv matrices are assumed to be diagonal
//

// Implementation Note - 7/17/2022 MCC:
//    In the descriptions below, two different parametrizations are refered to when parametrizing locations along the length of the catheter, namely,
//    \lambda parameters, and s parameters:
//      \lambda refers to distances along the length of the cathteter measured from the tip towards the base (distal to proximal)
//      s refers to distances along the length of the cathteter measured from the entry point towards the top (proximal to distal)
//    It is important to note that s-parameters of specific locations/features changes as a function of the Insertion Length of the catheter whereas 
//      \lambda parameters are fixed.  
//   

namespace CRMCatheterModel {

	//
	// ---------------------------------------------------------
	//        Cosserat Rod Model - Integrator for Calculating the Jacobian of the Initial Value Problem Solver
	// ---------------------------------------------------------
	//

	// CRMSolverIVPJacobian API which uses CRMShootinMethodParams data structure
	//    note that in_ftip[] will be used, ignoring the value in_Params.TipForce[]
	std::tuple<MatrixXd, MatrixXd, MatrixXd, MatrixXd, MatrixXd> CRMSolverIVPJacobian(
		CRMShootingMethodParams in_Params,
		double in_deltau0[3], double in_ftip[3],
		bool in_FinalValueOnly,
		double out_x_N[NUM_STATES], double out_MomentResidual[3]);


	// Core Computations used for CRMSolverIVP Jacobian Calculation
	//    parameters are prepared using CRMSolverIVP_Prep
	//    the values in in_u and in_ftp should be those coming from an equilibrium calculation
	//    if needed. CRMSolverIVP_CorewithJacobian can be called multiple times by changing only u[0..2] components of xi (in_u[]) and in_ftip[]
	//    for a single execution of CRMSolverIVP_Prep - the values in in_params should not be changed by user
	template <typename IVPJacobians>
	void CRMSolverIVP_CoreWithJacobian(CRMIVPCoreParams& in_params,
		double in_deltau[3], double in_ftip[3],
		AugmentedStateVector<IVPJacobians>& out_x_N, double out_MomentResidual[3]);

}
