#include "CRM.hpp"
#include "CRM_BVPIVP_APIDeclarations.hpp"
#include "CRM_IVP_NumericalIntegrationTemplates.hpp"
#include "CRM_IVPJacobian.hpp"
#include "CRM_IVPJacobian_InternalAPI.hpp"

using Eigen::Matrix;
using Eigen::RowMajor;

namespace CRMCatheterModel {


	std::tuple<MatrixXd, MatrixXd, MatrixXd, MatrixXd, MatrixXd> CRMSolverIVPJacobian(
		CRMShootingMethodParams in_Params,
		double in_deltau0[3], double in_ftip[3],
		bool in_FinalValueOnly,
		double out_x_N[NUM_STATES], double out_MomentResidual[3]) {

		double x_0[NUM_STATES];
		for (int i = 0; i < 3; i++) x_0[i] = in_Params.p0[i];
		for (int i = 0; i < 9; i++) x_0[i + 3] = in_Params.R0[i];
		for (int i = 0; i < 3; i++) x_0[i + 3 + 9] = std::nan("0");  //  This will be overridden in CRMSolverIVP_CoreWithJacobian

		CRMIVPCoreParams CoreParams(in_Params.no_flex_seg, in_Params.no_rigid_seg, in_Params.no_act_set, in_Params.no_locmarkers, in_Params.no_fcum_steps);
		bool CalculateEnergy = false;
		bool FinalValueOnly = true;

		AugmentedStateVector<IVPJacobiansFull> x_N;

		CRMSolverIVP_Prep(
			in_Params.no_flex_seg, in_Params.no_rigid_seg, in_Params.no_act_set, in_Params.no_locmarkers, in_Params.no_fcum_steps,
			x_0, in_Params.IntegrationStepSize,
			in_Params.Li, in_Params.dlambdainv,
			in_Params.SegmentTypes,
			in_Params.SegEndLambdas, in_Params.LocMarkerLambdas,
			in_Params.rho,
			in_Params.K, in_Params.Kinv, in_Params.ustar,
			in_Params.ActMass,
			in_Params.CoilAlignmentTurnAreaMatrix,
			in_Params.MagMoment, in_Params.fcumlambda,
			in_Params.B0, in_Params.g,
			CalculateEnergy,
			FinalValueOnly,
			CoreParams);

		// We need to pass deltau_0 as input argument as the values in CoreParams will be overriden with the values provided in the input arguments - functionality needed for solving Boundary Value Problems (BVP)

		CRMSolverIVP_CoreWithJacobian(CoreParams, in_deltau0, in_ftip, x_N, out_MomentResidual);

		// NO NEED FOR CRMSolverIVP_Return
		mCopy_AB<3>(x_N._p, out_x_N + 0);
		mCopy_AB<9>(x_N._R, out_x_N + 3);
		mCopy_AB<3>(x_N._u, out_x_N + 3 + 9);

		// for simplicity, define an alias
		constexpr unsigned int Cs = CURRENT_ACT_VECTOR_DIM;

		//using EMT33 = Eigen::Map<Matrix<double, 3, 3, RowMajor>, Eigen::Unaligned, Eigen::Stride<Cs, 1> >;

		__EMT<3, 3> JIVP_p_u0(x_N._p_u0);
		__EMT<3, 3> JIVP_ws_u0(x_N._ws_u0);
		__EMT<3, 3> JIVP_u_u0(x_N._u_u0);

		__EMT<3, 3> JIVP_p_ft(x_N._p_ft);
		__EMT<3, 3> JIVP_ws_ft(x_N._ws_ft);
		__EMT<3, 3> JIVP_u_ft(x_N._u_ft);

		Matrix<double, 3, Cs + 1, RowMajor> JIVP_p_z;
		Matrix<double, 3, Cs + 1, RowMajor> JIVP_ws_z;
		Matrix<double, 3, Cs + 1, RowMajor> JIVP_u_z;
		// we will need to flip the order of actuators in zc, since IVP Core calculations use proximal to distal ordering
		//_p_z
		__EMT<3, Cs> JIVP_p_zc_reverse(x_N._p_zc);
		__EVT<3> JIVP_p_zl(x_N._p_zl);
		for (int i = 0; i < NUM_ACT_SET; i++) JIVP_p_z.template middleCols<3>(3 * i) = JIVP_p_zc_reverse.template middleCols<3>(((NUM_ACT_SET - 1) - i) * 3);
		JIVP_p_z.template rightCols<1>() = JIVP_p_zl;
		//_R_z
		__EMT<3, Cs> JIVP_ws_zc_reverse(x_N._ws_zc);
		__EVT<3> JIVP_ws_zl(x_N._ws_zl);
		for (int i = 0; i < NUM_ACT_SET; i++) JIVP_ws_z.template middleCols<3>(3 * i) = JIVP_ws_zc_reverse.template middleCols<3>(((NUM_ACT_SET - 1) - i) * 3);
		JIVP_ws_z.template rightCols<1>() = JIVP_ws_zl;
		//_u_z
		__EMT<3, Cs> JIVP_u_zc_reverse(x_N._u_zc);
		__EVT<3> JIVP_u_zl(x_N._u_zl);
		for (int i = 0; i < NUM_ACT_SET; i++) JIVP_u_z.template middleCols<3>(3 * i) = JIVP_u_zc_reverse.template middleCols<3>(((NUM_ACT_SET - 1) - i) * 3);
		JIVP_u_z.template rightCols<1>() = JIVP_u_zl;

		// for debugging
		//std::cout << "---- x_N ---- \n" << x_N << std::endl;

		MatrixXd JIVP_u_u0_pinv = JIVP_u_u0.completeOrthogonalDecomposition().pseudoInverse();
		MatrixXd JBVP_p_z = JIVP_p_z - JIVP_p_u0 * JIVP_u_u0_pinv * JIVP_u_z;
		MatrixXd JBVP_ws_z = JIVP_ws_z - JIVP_ws_u0 * JIVP_u_u0_pinv * JIVP_u_z;

		MatrixXd JBVP_p_ft = JIVP_p_ft - JIVP_p_u0 * JIVP_u_u0_pinv * JIVP_u_ft;
		MatrixXd JBVP_ws_ft = JIVP_ws_ft - JIVP_ws_u0 * JIVP_u_u0_pinv * JIVP_u_ft;
		MatrixXd Jft_z = -JBVP_p_ft.completeOrthogonalDecomposition().pseudoInverse() * JBVP_p_z;  // Is this the best option in Eigen ???

		return { JBVP_p_z, JBVP_ws_z, JBVP_p_ft, JBVP_ws_ft, Jft_z };
	}



	template <typename IVPJacobians>
	void CRMSolverIVP_CoreWithJacobian(CRMIVPCoreParams& in_params,
		double in_deltau[3], double in_ftip[3],
		AugmentedStateVector<IVPJacobians>& out_x_N, double out_MomentResidual[3]) {

		// convenience definitions
		const double Identity3x3[9] = { 1,0,0,0,1,0,0,0,1 };
		const double Zero3x3[9] = { 0,0,0,0,0,0,0,0,0 };
		const double Zero3[3] = { 0,0,0 };

		double l_zero[3] = { 0.0,0.0,0.0 };
		double h;						// integration stepsize
		double RigidSegmentLength;		// Length of the rigid segment - intermediate variable
		double Residual_im1[3] = { 0.0,0.0,0.0 };   // moment residual from the last flexible segment processed
		double deltau[3];					// intermediate variable
		int   fsegno; 						// flexible segment no
		int32_t   actno, fsegi, fsegip1;	// actuator no, flexible segment before, flexible segment after
		const double *MMpoint, *CATAMpoint;	// pointer to current MagMoment[3] and CoilAlignmentTurnAreaMatrix[9];
		const double *Kipoint, *usipoint;	// pointer to current K_i[9] and ustar_i[3]
		const double *Kinvip1point, *usip1point;	// pointer to current K_ip1[9] and ustar_ip1[3]
		bool  LastSegmentIsRigid = false;	// Flag indicating if the last segment processed is rigid (true) or not (false)
											//    does not include the case where the segment at the entry port is rigid

		// we need to copy in_ftip to local variable
		double ftip[3];
		mCopy_AB<3>(in_ftip, ftip);
		// we will copy anything we will access more than once (or write to) to local variables
		int   StartSegmentIndex = in_params.StartSegmentIndex;
		int	  NextLocMarker = in_params.NextLocMarker;
		int	  InitialLocMarker = NextLocMarker;
		// for others, we will create aliases
		auto& no_segments = in_params.no_segments;
		auto& no_flex_seg = in_params.no_flex_seg;
		auto& no_rigid_seg = in_params.no_rigid_seg;
		auto& no_act_set = in_params.no_act_set;
		auto& no_locmarkers = in_params.no_locmarkers;
		auto& no_fcum_steps = in_params.no_fcum_steps;
		auto& SegTypes = in_params.SegmentTypes;
		auto& FlexActIndex = in_params.FlexActIndex;
		auto& SegBounds = in_params.SegBounds;
		auto& SegSteps = in_params.SegSteps;
		auto& InsertedLength = in_params.InsertedLength;
		auto& dlambdainv = in_params.dlambdainv;
		auto& K = in_params.K;
		auto& Kinv = in_params.Kinv;
		auto& ustar = in_params.ustar;
		// auto& ActMass = in_params.ActMass;	// not needed since we are not calculating potential energy during Jacobian calculations
		// auto& rho = in_params.rho;			// not needed since we are not calculating potential energy during Jacobian calculations
		auto& fcumlambda = in_params.fcumlambda;
		bool CalculateEnergy = false; // we do not need to calculate the energy for Jacobian calculation
		bool FinalValueOnly = true;   // we do not need to calculate the LocMarker positions for Jacobian calculation
		auto& LocMarkers = in_params.LocMarkers;
		// auto& g = in_params.g;				// not needed since we are not calculating potential energy during Jacobian calculations
		auto& B0 = in_params.B0;
		auto& MagMoment = in_params.MagMoment;
		auto& CoilAlignmentTurnAreaMatrix = in_params.CoilAlignmentTurnAreaMatrix;

		AugmentedStateVector<IVPJacobians> xi;  		//  Initial value of the state for the next segment to be integrated
		auto& xf = out_x_N;					//  Final value of the state for the last segment integrated
		auto& Residual = out_MomentResidual;	// Residual at the catheter tip -- will be returned
		auto& p_atLocMarkers = in_params.p_atLocMarkers; // we need to pass a buffer to ABM4 even though it will not be used

		// initial conditions for the regular states
		// we need to copy xi from in_params to the local variable and update it with u[0..2] specified in in_u
		mCopy_AB<3>(in_params.xi + 0, xi._p);
		mCopy_AB<9>(in_params.xi + 3, xi._R);
		//since input is in_deltau, not in_u, we will not perform this, but instead use the initialization after we find the first flexible segment
		//mCopy_AB<3>(in_u, xi._u);

		// initial conditions for the augmented states
		//for (int i = 0; i < 3; i++)  xi._p_p0[i * 3 + i] = 1.0;		// _p_p0 = I33
		//for (int i = 0; i < 3 * 3; i++) xi._ws_w0[i] = xi._R[i];		// _ws_w0 = R0
		for (int i = 0; i < 3; i++)  xi._u_u0[i * 3 + i] = 1.0;			// _u_u0 = I33
		if constexpr (std::is_same_v<expr_type<IVPJacobians>, expr_type<IVPJacobiansFull>>) {
			for (int i = 0; i < 3; i++) xi._p_zl[i] = xi._R[i * 3 + 2];		// _p_zl = R0_.3
		}

		// intermediate varaibles used in potential energy calculation
		double dummyDeltaPE = 0.0;

		// define the structure that will used to pass parameters to the CRMItegrand
		CRMIntegrandParams IntegrandParams = {};
		// these parameters are same for all segments
		IntegrandParams.dlambdainv = dlambdainv;
		IntegrandParams.Li = InsertedLength;
		IntegrandParams.l = l_zero;  // we are assuming the distributed moment on the catheter body is zero
		IntegrandParams.no_fcum_steps = in_params.no_fcum_steps;
		IntegrandParams.fcumlambda = fcumlambda;
		IntegrandParams.ftip = ftip;


		// IMPORTANT NOTE: most proximal segment is assumed to be always flexible
		//    and the flexible and rigid segments are assumed to be alternating
		//    most distal segment can be flexible or rigid

		// If the starting segment is a rigid segment, then we will need to move initial conditions to the start of the next flexible segment and change the StartingSegment to that segment
		if ( (SegTypes[StartSegmentIndex] == CatheterSegmentType::RIGID || SegTypes[StartSegmentIndex] == CatheterSegmentType::RIGID_WITH_ACTUATOR) ) {

			while ((SegTypes[StartSegmentIndex] == CatheterSegmentType::RIGID || SegTypes[StartSegmentIndex] == CatheterSegmentType::RIGID_WITH_ACTUATOR)
				&& (StartSegmentIndex < no_segments)) {
				RigidSegmentLength = SegBounds[StartSegmentIndex + 1] - SegBounds[StartSegmentIndex];	// how far we need to move along the length of the rigid segment to reach the next flexible segment
				for (int i = 0; i < 3; i++) { 										// u[0..2] and R[0..8] remain the same
					xi._p[i] = xi._p[i] + RigidSegmentLength * xi._R[i * 3 + 2];	// p[0..2] will translate along the z direction of the R matrix (3rd column)
				}
				StartSegmentIndex++;  // move the start segment to the next segment
			}
			// if input is in_deltau, uncomment the following code, which will make input incremental over ustar 
			if (StartSegmentIndex < no_segments) // we are making sure that all of the segments in the chamber are not rigid
				mAdd_AB<3, 1>(in_deltau, ustar[FlexActIndex[StartSegmentIndex]], xi._u);

			// initial conditions for the augmented states
			//for (int i = 0; i < 3; i++) {								// _p_w0 = [ -d * R0_2, d* R0_1, 0]  for rigid initial segment 
			//	xi._p_w0[i * 3 + 0] = - xi._R[i * 3 + 1] * RigidSegmentLength;
			//	xi._p_w0[i * 3 + 1] =   xi._R[i * 3 + 0] * RigidSegmentLength;
			//	xi._p_w0[i * 3 + 2] = 0.0;
			//}
			if constexpr (std::is_same_v<expr_type<IVPJacobians>, expr_type<IVPJacobiansFull>>) {
				for (int i = 0; i < 3; i++)  xi._ws_zl[i] = 0.0;			// _ws_zl = 0 for rigid initial segment 
				for (int i = 0; i < 3; i++)  xi._u_zl[i] = 0.0;				// _u_zl = 0 for rigid initial segment 
			}

		}
		else {

			// if input is in_deltau, uncomment the following code, which will make input incremental over ustar 
			mAdd_AB<3, 1>(in_deltau, ustar[FlexActIndex[StartSegmentIndex]], xi._u);

			// initial conditions for the augmented states
			if constexpr (std::is_same_v<expr_type<IVPJacobians>, expr_type<IVPJacobiansFull>>) {
				mMult_AB<3, 3, 1>(xi._R, xi._u, xi._ws_zl);					// _ws_zl = R * u
			}
			StateDerivativeVector xdot_temp;	// intermediate variable
			// assign the parameters that vary from segment to segment
			IntegrandParams.K = K[FlexActIndex[StartSegmentIndex]];
			IntegrandParams.Kinv = Kinv[FlexActIndex[StartSegmentIndex]];
			IntegrandParams.ustar = ustar[FlexActIndex[StartSegmentIndex]];
			CRMIntegrand(SegBounds[0] /* 0.0 */, static_cast<StateVector>(xi), IntegrandParams, xdot_temp);
			if constexpr (std::is_same_v<expr_type<IVPJacobians>, expr_type<IVPJacobiansFull>>) {
				for (int i = 0; i < 3; i++)  xi._u_zl[i] = xdot_temp._u[i];	// _u_zl = Fu(0)
			}

		}

		// for iteration - the moment residual from the previous segment is zero
		for (int i = 0; i < 3; i++) Residual_im1[i] = 0.0;

		// In case there is nothing to integrate or the rigid segment is the only segment (it is both the start and the end segment),
		//   in which case, the main integration loop will not execute, 
		//   we need to have valid return values for xf and 
		//   Residual is initialized to the 0 vector and residual is initialized to the 0 vector
		xf = xi;
		for (int i = 0; i < 3; i++) Residual[i] = 0.0;


		// Integrate each of the remaining segments
		for (int i = StartSegmentIndex; i < no_segments; i++) {
			if (SegTypes[i] == CatheterSegmentType::FLEXIBLE) {   // Flexible Segment
				// Prepare the CRMIntegrand Parameters
				fsegno = FlexActIndex[i]; // flexible segment no

				// Calculate the actual stepsize, based on the number of steps
				h = (SegBounds[i + 1] - SegBounds[i]) / (SegSteps[fsegno] * 1.0);

				// assign the parameters that vary from segment to segment
				IntegrandParams.K = K[fsegno];
				IntegrandParams.Kinv = Kinv[fsegno];
				IntegrandParams.ustar = ustar[fsegno];

				// Integrate
				ABM4(xi, SegBounds[i], SegSteps[fsegno], h,
					IntegrandParams, no_locmarkers,
					CalculateEnergy,
					FinalValueOnly, LocMarkers, NextLocMarker,
					xf, dummyDeltaPE, p_atLocMarkers);

				LastSegmentIsRigid = false;

				// residual = K (u1 - u1star)
				mSub_AB<3, 1>(xf._u, ustar[fsegno], deltau);
				mMult_AB<3, 3, 1>(K[fsegno], deltau, Residual);

			}
			else {  // Need to do actuation/rigid segment calculations to transfer Initial Conditions to next flexible segment
				RigidSegmentLength = (SegBounds[i + 1] - SegBounds[i]);
				if (SegTypes[i] == CatheterSegmentType::RIGID) {  // not actuated rigid segment
					actno = -1;						// no actuator
					MMpoint = Zero3;				// points to a zero vector
					CATAMpoint = Zero3x3;			// points to a zero matrix
				}
				else {  // this is a rigid link with an actuator coil
					actno = FlexActIndex[i];		// actuator no
					MMpoint = MagMoment[actno];		// points to MagMoment of the actuator
					CATAMpoint = CoilAlignmentTurnAreaMatrix[actno];	// points to the CoilAlignmentTurnAreaMatrix of the actuator
				}
				if (LastSegmentIsRigid) {
					usipoint = Zero3;				// points to a zero vector
					Kipoint = Identity3x3;			// points to an identity matrix
				}
				else {
					fsegi = FlexActIndex[i - 1];
					usipoint = ustar[fsegi];		// points to ustar of previous link
					Kipoint = K[fsegi];				// points to K of next link
				}
				if (((i + 1) >= no_segments) || (SegTypes[i + 1] != CatheterSegmentType::FLEXIBLE)) { // next segment is not flexible or we are at the last segment
					usip1point = Zero3;				// points to a zero vector
					Kinvip1point = Identity3x3;		// points to an identity matrix
				}
				else {
					fsegip1 = FlexActIndex[i + 1];
					usip1point = ustar[fsegip1];	// points to ustar of next link
					Kinvip1point = Kinv[fsegip1];	// points to Kinv of next link
				}
				CRMSolverIVP_PropagateBCThroughRigidLink(xf, Residual, RigidSegmentLength, actno, MMpoint, CATAMpoint, B0,
					usipoint, Kipoint, usip1point, Kinvip1point, xi, Residual_im1);

				LastSegmentIsRigid = true;

			}
			mCopy_AB<3>(Residual, Residual_im1);  // calculated moment residual will be the Residual_im1 for the next iteration
			xi = xf; // The calculated final values will be the initial value of the next iteration
		}

		// Marker locations were not calculated - so they don't need to be copied to output

	}

	// explicitly instantiate two versions of CRMSolverIVP_CoreWithJacobian for IVPJacobiansMini and IVPJacobiansFull
	template void CRMSolverIVP_CoreWithJacobian<IVPJacobiansMini>(CRMIVPCoreParams& in_params,
		double in_u[3], double in_ftip[3],
		AugmentedStateVector<IVPJacobiansMini>& out_x_N, double out_MomentResidual[3]);

	template void CRMSolverIVP_CoreWithJacobian<IVPJacobiansFull>(CRMIVPCoreParams& in_params,
		double in_u[3], double in_ftip[3],
		AugmentedStateVector<IVPJacobiansFull>& out_x_N, double out_MomentResidual[3]);



	// each column k of  Pnew_k = P_k + (w_k^ R)_3 * d, where (w_k^ * R)_3 is the third column of the (w_k^ * R) matrix ( 3x3 matrices packed in row major order )
	template<unsigned int dim>
	inline void PpluswhatR3timesD(const double P[], const double w[], const double R[], const double d, double Pnew[]) {
		for (int k = 0; k < dim; k++) {  // loop over columns of P
			Pnew[0 * dim + k] = P[0 * dim + k] + (-w[2 * dim + k] * R[1 * 3 + 2] + w[1 * dim + k] * R[2 * 3 + 2]) * d;
			Pnew[1 * dim + k] = P[1 * dim + k] + (w[2 * dim + k] * R[0 * 3 + 2] - w[0 * dim + k] * R[2 * 3 + 2]) * d;
			Pnew[2 * dim + k] = P[2 * dim + k] + (-w[1 * dim + k] * R[0 * 3 + 2] + w[0 * dim + k] * R[1 * 3 + 2]) * d;
		}
	}

	// we will overload this function
	template<typename IVPJacobians>
	void CRMSolverIVP_PropagateBCThroughRigidLink(AugmentedStateVector<IVPJacobians>& xi_ip1, double Residual_ip1[3],
		const double RigidSegmentLength, const unsigned int ActNo, const double MagMoment[3], const double CoilAlignmentTurnAreaMatrix[9], const double B0[3], 
		const double ustar_i[3], const double K_i[9], const double ustar_ip1[3], const double Kinv_ip1[9],
		const AugmentedStateVector<IVPJacobians>& xf_i, const double Residual_i[3]) {

		// for debugging
		//std::cout << "~~~~~~ xf_i =    ~~~~~~ \n" << xf_i << std::endl;

		// Let's first propagate the regular state boundary conditions
		CRMSolverIVP_PropagateBCThroughRigidLink(xi_ip1, Residual_ip1, RigidSegmentLength, MagMoment, CoilAlignmentTurnAreaMatrix, B0,
			ustar_i, K_i, ustar_ip1, Kinv_ip1,
			static_cast<StateVector>(xf_i), Residual_i);

		double muhat[9]; // intermediate variables
		wHat(MagMoment, muhat);

		// for simplicity, define an alias
		constexpr unsigned int Cs = CURRENT_ACT_VECTOR_DIM;

		// p
		// dpdPhi_ip1 = dpdPhi_i + d * (ws_Phi_i^ * R)_3
		//PpluswhatR3timesD<3>(xf_i._p_p0, xf_i._ws_p0, xf_i._R, RigidSegmentLength, xi_ip1._p_p0);
		//PpluswhatR3timesD<3>(xf_i._p_w0, xf_i._ws_w0, xf_i._R, RigidSegmentLength, xi_ip1._p_w0);
		PpluswhatR3timesD<3>(xf_i._p_u0, xf_i._ws_u0, xf_i._R, RigidSegmentLength, xi_ip1._p_u0);
		if constexpr (std::is_same_v<expr_type<IVPJacobians>, expr_type<IVPJacobiansFull>>) {
			PpluswhatR3timesD<Cs>(xf_i._p_zc, xf_i._ws_zc, xf_i._R, RigidSegmentLength, xi_ip1._p_zc);
			PpluswhatR3timesD<1>(xf_i._p_zl, xf_i._ws_zl, xf_i._R, RigidSegmentLength, xi_ip1._p_zl);
			PpluswhatR3timesD<3>(xf_i._p_ft, xf_i._ws_ft, xf_i._R, RigidSegmentLength, xi_ip1._p_ft);
		}

		// R
		// dwsdPhi_ip1 = dwsdPhi_i
		//mCopy_AB<3 * 3>(xf_i._ws_p0, xi_ip1._ws_p0);
		//mCopy_AB<3 * 3>(xf_i._ws_w0, xi_ip1._ws_w0);
		mCopy_AB<3 * 3>(xf_i._ws_u0, xi_ip1._ws_u0);
		if constexpr (std::is_same_v<expr_type<IVPJacobians>, expr_type<IVPJacobiansFull>>) {
			mCopy_AB<3 * Cs>(xf_i._ws_zc, xi_ip1._ws_zc);
			mCopy_AB<3 * 1>(xf_i._ws_zl, xi_ip1._ws_zl);
			mCopy_AB<3 * 3>(xf_i._ws_ft, xi_ip1._ws_ft);
		}

		// u
		// dudPhi_ip1 = Kinv_ip1 * K_i * dudPhi_i - Kinv_ip1 dTaudPhi
		// dTaudPhi = dTaudws * dwsPhi  for all Phi except zc_link  (zc_link : actuation corresponding to current link)
		// dTaudzc_link = dTaudzc_link + dTaudws * dwsdzc_link  for zc_link
		// - Kinv_ip1 dTaudws * dwsPhi = Kinv_ip1 * mu^ * R^T * ws_Phi^ * B0


		// Kinv_ip1K_i = Kinv_ip1 * K_i
		double Kinv_ip1K_i[9];
		mMult_AB<3, 3, 3>(Kinv_ip1, K_i, Kinv_ip1K_i);

		// common terms
		// Kinv_ip1* K_i* dudPhi_i
		//mMult_AB<3, 3, 3>(Kinv_ip1K_i, xf_i._u_p0, xi_ip1._u_p0);
		//mMult_AB<3, 3, 3>(Kinv_ip1K_i, xf_i._u_w0, xi_ip1._u_w0);
		mMult_AB<3, 3, 3>(Kinv_ip1K_i, xf_i._u_u0, xi_ip1._u_u0);
		if constexpr (std::is_same_v<expr_type<IVPJacobians>, expr_type<IVPJacobiansFull>>) {
			mMult_AB<3, 3, Cs>(Kinv_ip1K_i, xf_i._u_zc, xi_ip1._u_zc);
			mMult_AB<3, 3, 1>(Kinv_ip1K_i, xf_i._u_zl, xi_ip1._u_zl);
			mMult_AB<3, 3, 3>(Kinv_ip1K_i, xf_i._u_ft, xi_ip1._u_ft);
		}

		// Kinv_ip1muhatRT = Kinv_ip1 * mu^ * R^T
		double muhatRT[9], Kinv_ip1muhatRT[9];
		mMult_ABT<3, 3, 3>(muhat, xf_i._R, muhatRT);
		mMult_AB<3, 3, 3>(Kinv_ip1, muhatRT, Kinv_ip1muhatRT);
		// define lambda function to calculate xi_ip1._u_Phi += Kinv_ip1muhatRT * ws_Phi^ * B0, for a given Phi
		auto addmKinvdTaudPhi = [&B0, &Kinv_ip1muhatRT](double* _ws_Phi, double* _u_Phi, unsigned int stride) {
			for (unsigned int j = 0; j < stride; j++) {
				double what[9], whatB0[3], Kinv_ip1muhatRTwhatB0[3];
				wHat(_ws_Phi + j, what, stride);
				mMult_AB<3, 3, 1>(what, B0, whatB0);
				mMult_AB<3, 3, 1>(Kinv_ip1muhatRT, whatB0, Kinv_ip1muhatRTwhatB0);
				for (int i = 0; i < 3; i++) _u_Phi[i * stride + j] += Kinv_ip1muhatRTwhatB0[i];
			}
		};

		// xi_ip1._u_Phi += Kinv_ip1muhatRT * ws_Phi^ * B0, for every Phi
		//addmKinvdTaudPhi(xf_i._ws_p0, xi_ip1._u_p0, 3);
		//addmKinvdTaudPhi(xf_i._ws_w0, xi_ip1._u_w0, 3);
		addmKinvdTaudPhi(xf_i._ws_u0, xi_ip1._u_u0, 3);
		if constexpr (std::is_same_v<expr_type<IVPJacobians>, expr_type<IVPJacobiansFull>>) {
			addmKinvdTaudPhi(xf_i._ws_zc, xi_ip1._u_zc, Cs);
			addmKinvdTaudPhi(xf_i._ws_zl, xi_ip1._u_zl, 1);
			addmKinvdTaudPhi(xf_i._ws_ft, xi_ip1._u_ft, 3);

			if (ActNo != -1) {
				// dTaudPhi term for the specific actuator that is on the current link
				// - Kinv_ip1 * dTaudzc_link
				//
				// mKinv_ip1dTaudzc_link = Kinv_ip1 * mdTaudzc_link = Kinv_ip1 * -(- (R^T B0)^ (CATA)) , where CATA = CoilAlignmentMatrix * CoilTurnAreaMatrix
				double RTB0[3];
				mMult_ATB<3, 3, 1>(xf_i._R, B0, RTB0);
				double RTB0hat[9];
				wHat(RTB0, RTB0hat);
				double mdTaudzc_link[3 * 3];
				mMult_AB<3, 3, 3>(RTB0hat, CoilAlignmentTurnAreaMatrix, mdTaudzc_link);
				double mKinv_ip1dTaudzc_link[3 * 3];
				mMult_AB<3, 3, 3>(Kinv_ip1, mdTaudzc_link, mKinv_ip1dTaudzc_link);
				for (int i = 0; i < 3; i++)
					for (int j = 0; j < 3; j++)
						xi_ip1._u_zc[i * Cs + (ActNo * 3 + j)] += mKinv_ip1dTaudzc_link[i * 3 + j];
			}
		}

		// for debugging
		//std::cout << "~~~~~~  xi_ip1 =   ~~~~~~\n" << xi_ip1 << std::endl;
	}



	template <typename IVPJacobians>
	void CRMIntegrand(const double s, const AugmentedStateVector<IVPJacobians>& in_x, const CRMIntegrandParams in_Params,
		AugmentedStateDerivativeVector<IVPJacobians>& out_xdot) {

		using namespace Eigen;

		// for simplicity, define an alias
		constexpr unsigned int Cs = CURRENT_ACT_VECTOR_DIM;

		// first evaluate partial derivative of x w.r.t. s using CRMIntegrand
		CRMIntegrand(s, static_cast<StateVector>(in_x), in_Params, out_xdot);

		// for simplicity, create aliases
		auto& Length = in_Params.Li;
		auto& deltalambdainv = in_Params.dlambdainv;
		auto& K = in_Params.K;
		auto& Kinv = in_Params.Kinv;
		//__EMT<3, 3> Kinv(in_Kinv);
		auto& ustar = in_Params.ustar;
		auto& l = in_Params.l;
		//  assume ustardot = 0
		auto& x = in_x;
		auto& p = in_x._p;
		auto& R = in_x._R;
		auto& u = in_x._u;

		double fcum[3];
		// calculate interpolated value of fcum
		double lambda = Length - s;
		double ix = lambda * deltalambdainv;
		double ird_f = floor(ix); // index for round down  -- doubleing point
		if (ird_f < 0) ird_f = 0;
		int ird = (int)ird_f;	//    integer index
		double iru_f = ceil(ix); 	// index for round up  -- doubleing point
		if (iru_f > in_Params.no_fcum_steps) iru_f = in_Params.no_fcum_steps;
		int iru = (int)iru_f;	//    integer index
		double ixmird = ix - ird;    // weight for interpolation
		double irumix = iru - ix;	// weight for interpolation
		for (int i = 0; i < 3; i++) {
			fcum[i] = in_Params.fcumlambda[iru][i] * ixmird + in_Params.fcumlambda[ird][i] * irumix;
		}
		// add the tip force to fcum
		for (int i = 0; i < 3; i++) {
			fcum[i] += in_Params.ftip[i];
		}


		//
		// now evaluate partial derivative of ( \partial x / \partial \phi ) x w.r.t. s 
		//

		// intermediate terms needed
		double uhat[3 * 3];
		wHat(u, uhat);

		//
		// _p terms
		//

		// define lambda function for calculating derivative of _p_Phi
		//   _p_phi = \hat{ws_phi} * R_3
		auto dot_p_Phi = [&R](double* x_ws_Phi, double* xdot_p_Phi, unsigned int stride) {
			for (unsigned int j = 0; j < stride; j++) {
				xdot_p_Phi[0 * stride + j] = -x_ws_Phi[2 * stride + j] * R[1 * 3 + 2] + x_ws_Phi[1 * stride + j] * R[2 * 3 + 2];
				xdot_p_Phi[1 * stride + j] = x_ws_Phi[2 * stride + j] * R[0 * 3 + 2] - x_ws_Phi[0 * stride + j] * R[2 * 3 + 2];
				xdot_p_Phi[2 * stride + j] = -x_ws_Phi[1 * stride + j] * R[0 * 3 + 2] + x_ws_Phi[0 * stride + j] * R[1 * 3 + 2];
			}
		};

		//_p_p0
		//dot_p_Phi(x._ws_p0, out_xdot._p_p0,3);

		//_p_w0
		//dot_p_Phi(x._ws_w0, out_xdot._p_w0, 3);

		//_p_u0
		dot_p_Phi(x._ws_u0, out_xdot._p_u0, 3);

		if constexpr (std::is_same_v<expr_type<IVPJacobians>, expr_type<IVPJacobiansFull>>) {
			//_p_zc
			dot_p_Phi(x._ws_zc, out_xdot._p_zc, Cs);

			//_p_zl
			dot_p_Phi(x._ws_zl, out_xdot._p_zl, 1);

			//_p_ft
			dot_p_Phi(x._ws_ft, out_xdot._p_ft, 3);
		}

		//
		// _ws terms
		//

		//   _ws_Phi = [ dFR_du * _u_Phi * R^T ]V;   where V is the 'vee' operator
		//           =  R _u_Phi

		//_ws_p0
		//mMult_AB<3, 3, 3>(R, x._u_p0, out_xdot._ws_p0);

		//_ws_w0
		//mMult_AB<3, 3, 3>(R, x._u_w0, out_xdot._ws_w0);

		//_ws_u0
		mMult_AB<3, 3, 3>(R, x._u_u0, out_xdot._ws_u0);

		if constexpr (std::is_same_v<expr_type<IVPJacobians>, expr_type<IVPJacobiansFull>>) {
			//_ws_zc
			mMult_AB<3, 3, Cs>(R, x._u_zc, out_xdot._ws_zc);

			//_ws_zl
			mMult_AB<3, 3, 1>(R, x._u_zl, out_xdot._ws_zl);

			//_ws_ft
			mMult_AB<3, 3, 3>(R, x._u_ft, out_xdot._ws_ft);
		}

		//
		// _u terms
		//

		// common terms
		double Kinve3hat[3 * 3] = { Kinv[0 * 3 + 1], -Kinv[0 * 3 + 0], 0, /*;*/ Kinv[1 * 3 + 1], -Kinv[1 * 3 + 0], 0, /*;*/ Kinv[2 * 3 + 1], -Kinv[2 * 3 + 0], 0 };
		double Kinve3hatRT[3 * 3], KinvRT[3 * 3];
		mMult_ABT<3, 3, 3>(Kinve3hat, R, Kinve3hatRT);
		mMult_ABT<3, 3, 3>(Kinv, R, KinvRT);

		// define lambda function to calculate dFu_dws * _ws_Phi term of dFu_dPhi
		//   
		auto dFu_dws_dws_dPhi = [&fcum, &l, &Kinve3hatRT, &KinvRT](double* _ws_Phi, double* xdot_u_Phi, unsigned int stride) {
			double _ws_Phihat[9], _ws_Phihatfcum[3], _ws_Phihatl[3], temp[3];
			for (unsigned int j = 0; j < stride; j++) {
				wHat(_ws_Phi + j, _ws_Phihat, stride);
				mMult_AB<3, 3, 1>(_ws_Phihat, fcum, _ws_Phihatfcum);
				mMult_AB<3, 3, 1>(_ws_Phihat, l, _ws_Phihatl);
				mMult_AB<3, 3, 1>(Kinve3hatRT, _ws_Phihatfcum, temp);
				mMultAdd_AB<3, 3, 1>(KinvRT, _ws_Phihatl, temp);
				for (int i = 0; i < 3; i++)	xdot_u_Phi[i * stride + j] = temp[i];
			}
		};

		//Matrix<double, 3, 3, RowMajor> dFu_du = ;
		// FOR THIS CALCULATION, WE ARE ASSUMING THAT: K and Kinv matrices are diagonal
		auto& K0 = K[0 * 3 + 0];
		auto& K1 = K[1 * 3 + 1];
		auto& K2 = K[2 * 3 + 2];
		auto& K0inv = Kinv[0 * 3 + 0];
		auto& K1inv = Kinv[1 * 3 + 1];
		auto& K2inv = Kinv[2 * 3 + 2];
		auto& u0 = u[0];
		auto& u1 = u[1];
		auto& u2 = u[2];
		auto& u0st = ustar[0];
		auto& u1st = ustar[1];
		auto& u2st = ustar[2];
		double dFu_du[3 * 3] = { 0.0,                                          K0inv * K1 * u2 - K0inv * K2 * (u2 - u2st),   -K0inv * K2 * u1 + K0inv * K1 * (u1 - u1st), /*;*/
								 -K1inv * K0 * u2 + K1inv * K2 * (u2 - u2st),  0.0,                                          K1inv * K2 * u0 - K1inv * K0 * (u0 - u0st), /*;*/
								 K2inv * K0 * u1 - K2inv * K1 * (u1 - u1st),   -K2inv * K1 * u0 + K2inv * K0 * (u0 - u0st),  0.0 };


		//_u_p0
		//out_xdot._u_p0 = dFu_dws * _ws_p0 + dFu_du * _u_p0; 
		//dFu_dws_dws_dPhi(x._ws_p0, out_xdot._u_p0, 3);
		//mMultAdd_AB<3, 3, 3>(dFu_du, x._u_p0, out_xdot._u_p0);

		//_u_w0
		//out_xdot._u_w0 = dFu_dws * _ws_w0 + dFu_du * _u_w0; 
		//dFu_dws_dws_dPhi(x._ws_w0, out_xdot._u_w0, 3);
		//mMultAdd_AB<3, 3, 3>(dFu_du, x._u_w0, out_xdot._u_w0);

		//_u_u0
		//out_xdot._u_u0 = dFu_dws * _ws_u0 + dFu_du * _u_u0; 
		dFu_dws_dws_dPhi(x._ws_u0, out_xdot._u_u0, 3);
		mMultAdd_AB<3, 3, 3>(dFu_du, x._u_u0, out_xdot._u_u0);

		if constexpr (std::is_same_v<expr_type<IVPJacobians>, expr_type<IVPJacobiansFull>>) {
			//_u_zc
			//out_xdot._u_zc = dFu_dws * _ws_zc + dFu_du * _u_zc; 
			dFu_dws_dws_dPhi(x._ws_zc, out_xdot._u_zc, Cs);
			mMultAdd_AB<3, 3, Cs>(dFu_du, x._u_zc, out_xdot._u_zc);

			//_u_zl
			//out_xdot._u_zl = dFu_dws * _ws_zl + dFu_du * _u_zl; 
			dFu_dws_dws_dPhi(x._ws_zl, out_xdot._u_zl, 1);
			mMultAdd_AB<3, 3, 1>(dFu_du, x._u_zl, out_xdot._u_zl);

			//  dFu_dft = - Kinv * e3hat * R'
			//   (- e3hat * R') = [ r12 r22 r32; -r11 -r21 -r31; 0 0 0];
			//Matrix<double, 3, 3, RowMajor> me3hatRT{ { R[1], R[4], R[7] }, { -R[0], -R[3], -R[6] }, { 0.0, 0.0, 0.0 } };
			//Matrix<double, 3, 3, RowMajor> dFu_dft = Kinv * me3hatRT;
			double me3hatRT[3 * 3] = { R[1], R[4], R[7], /*;*/ -R[0], -R[3], -R[6], /*;*/ 0.0, 0.0, 0.0 };
			double dFu_dft[3 * 3];
			mMult_AB<3, 3, 3>(Kinv, me3hatRT, dFu_dft);

			//_u_ft
			//out_xdot._u_ft = dFu_dws * _ws_ft + dFu_du * _u_ft + dFu_dft; 
			dFu_dws_dws_dPhi(x._ws_ft, out_xdot._u_ft, 3);
			mMultAdd_AB<3, 3, 3>(dFu_du, x._u_ft, out_xdot._u_ft);
			mAdd_AB<3, 3>(out_xdot._u_ft, dFu_dft);
		}

	}


}
