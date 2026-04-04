#pragma once

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

	// support function to Propagate Boundary Condition through a Rigid Link - used by CRMSolverIVP_CoreWithJacobian
	template<typename IVPJacobians>
	void CRMSolverIVP_PropagateBCThroughRigidLink(AugmentedStateVector<IVPJacobians>& xi_ip1, double Residual_ip1[3],
		const double RigidSegmentLength, const unsigned int ActNo, const double MagMoment[3], const double CoilAlignmentTurnAreaMatrix[9], const double B0[3], 
		const double ustar_i[3], const double K_i[9], const double ustar_ip1[3], const double Kinv_ip1[9],
		const AugmentedStateVector<IVPJacobians>& xf_i, const double Residual_i[3]);


	// Cosserat Rod Model IVP Integrand for Jacobian Calculation
	template <typename IVPJacobians>
	void CRMIntegrand(const double s, const AugmentedStateVector<IVPJacobians>& in_x, const CRMIntegrandParams in_Params,
		AugmentedStateDerivativeVector<IVPJacobians>& out_xdot);

	// helper functions
	// each column k of  Pnew_k = P_k + (w_k^ R_k)_3 * d, where (w_k^ * R_k)_3 is the third column of the (w_k^ * R_k) matrix ( 3x3 matrices packed in row major order )
	template<unsigned int dim>
	inline void PpluswhatR3timesD(const double P[], const double w[], const double R[], const double d, double Pnew[]);

}
