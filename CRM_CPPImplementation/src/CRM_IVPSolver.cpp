#include "CRM.hpp"
#include "CRM_BVPIVP_APIDeclarations.hpp"
#include "CRM_IVP_NumericalIntegrationTemplates.hpp"

namespace CRMCatheterModel {


	void CRMSolverIVP(
		int32_t in_no_flex_seg,
		int32_t in_no_rigid_seg,
		int32_t in_no_act_set,
		int32_t in_no_locmarkers,
		int32_t in_no_fcum_steps,
		double in_x_0[NUM_STATES], double in_IntegrationStepSize,
		double in_Li, double in_dlambdainv,
		CatheterSegmentType in_SegmentTypes[/*no_segments=no_flex_seg+no_rigid_seg*/],
		double in_SegEndLambdas[/*no_segments=no_flex_seg+no_rigid_seg*/], double in_LocMarkerLambdas[/*no_locmarkers*/],
		double in_rho[/*no_segments=no_flex_seg+no_rigid_seg*/],
		double in_K[/*no_flex_seg*/][9], double in_Kinv[/*no_flex_seg*/][9], double in_ustar[/*no_flex_seg*/][3],
		double in_ActMass[/*no_act_set*/],
		double in_CoilAlignmentTurnAreaMatrix[/*no_act_set*/][9],
		double in_MagMoment[/*no_act_set*/][3], double in_fcumlambda[/*in_no_fcum_steps + 1*/][3], double in_ftip[3],
		double in_B0[3], double in_g[3],
		bool in_CalculateEnergy,
		bool in_FinalValueOnly,
		double out_x_N[NUM_STATES], double out_MomentResidual[3],
		double& out_PotentialEnergy, 
		double out_p_atLocMarkers[/*no_locmarkers*/][3],
		double out_p_atActuators[/*in_params.no_act_set*/][3],
		double out_R_atActuators[/*in_params.no_act_set*/][9]
	) {

		int32_t in_no_segments = in_no_flex_seg + in_no_rigid_seg;
		CRMIVPCoreParams CoreParams(in_no_flex_seg, in_no_rigid_seg, in_no_act_set, in_no_locmarkers, in_no_fcum_steps);
		double x_0[NUM_STATES];
		mCopy_AB<NUM_STATES>(in_x_0, x_0);
		StateVector x_N;
		double deltau_0[3];

		// copy to local variable
		double MagMoment[NUM_ACT_SET][3];
		mCopy_ABm<NUM_ACT_SET, 3>(in_MagMoment, MagMoment);
		double Li = in_Li;

		CRMSolverIVP_Prep(
			in_no_flex_seg, in_no_rigid_seg, in_no_act_set, in_no_locmarkers, in_no_fcum_steps,
			x_0, in_IntegrationStepSize,
			Li, in_dlambdainv,
			in_SegmentTypes, 
			in_SegEndLambdas, in_LocMarkerLambdas,
			in_rho,
			in_K, in_Kinv, in_ustar,
			in_ActMass,
			in_CoilAlignmentTurnAreaMatrix,
			MagMoment, in_fcumlambda,
			in_B0, in_g,
			in_CalculateEnergy,
			in_FinalValueOnly,
			CoreParams);

		// We need to pass deltau_0 as input argument as the u0 values in CoreParams will be overriden with the values provided in the input arguments - functionality needed for solving Boundary Value Problems (BVP)
		// deltau0 = u0 - ustar0
		if (CoreParams.SegmentTypes[CoreParams.StartSegmentIndex] == CatheterSegmentType::FLEXIBLE) {
			for (int i = 0; i < 3; i++) deltau_0[i] = in_x_0[3 + 9 + i] - CoreParams.ustar[CoreParams.FlexActIndex[CoreParams.StartSegmentIndex]][i];
		}
		else { // rigid, or rigid with actuator, so ustar=0
			for (int i = 0; i < 3; i++) deltau_0[i] = in_x_0[3 + 9 + i];
		}

		CRMSolverIVP_Core(CoreParams, deltau_0, in_ftip, x_N, out_MomentResidual, out_PotentialEnergy, out_p_atLocMarkers, out_p_atActuators, out_R_atActuators);

		// NO NEED FOR CRMSolverIVP_Return
		mCopy_AB<3>(x_N._p, out_x_N + 0);
		mCopy_AB<9>(x_N._R, out_x_N + 3);
		mCopy_AB<3>(x_N._u, out_x_N + 3 + 9);

	}


	void CRMSolverIVP(CRMShootingMethodParams in_Params,
		double in_deltau0[3], double in_ftip[3],
		bool in_CalculateEnergy,
		bool in_FinalValueOnly,
		double out_x_N[NUM_STATES], double out_MomentResidual[3],
		double& out_PotentialEnergy, 
		double out_p_atLocMarkers[/*in_Params.no_locmarkers*/][3],
		double out_p_atActuators[/*in_params.no_act_set*/][3],
		double out_R_atActuators[/*in_params.no_act_set*/][9] ) {

		double x_0[NUM_STATES];
		for (int i = 0; i < 3; i++) x_0[i] = in_Params.p0[i];
		for (int i = 0; i < 9; i++) x_0[i + 3] = in_Params.R0[i];
		for (int i = 0; i < 3; i++) x_0[i + 3 + 9] = std::nan("0");  //  This will be overridden in CRMSolverIVP_Core    //in_u0[i];

		CRMIVPCoreParams CoreParams(in_Params.no_flex_seg, in_Params.no_rigid_seg, in_Params.no_act_set, in_Params.no_locmarkers, in_Params.no_fcum_steps);

		StateVector x_N;

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
			in_CalculateEnergy,
			in_FinalValueOnly,
			CoreParams);

		CRMSolverIVP_Core(CoreParams, in_deltau0, in_ftip, x_N, out_MomentResidual, out_PotentialEnergy, out_p_atLocMarkers, out_p_atActuators, out_R_atActuators);

		// NO NEED FOR CRMSolverIVP_Return
		mCopy_AB<3>(x_N._p, out_x_N + 0);
		mCopy_AB<9>(x_N._R, out_x_N + 3);
		mCopy_AB<3>(x_N._u, out_x_N + 3 + 9);

	}


	void CRMSolverIVP_Prep(
		int32_t in_no_flex_seg,
		int32_t in_no_rigid_seg,
		int32_t in_no_act_set,
		int32_t in_no_locmarkers,
		int32_t in_no_fcum_steps,
		double in_x_0[NUM_STATES], double in_IntegrationStepSize,
		double in_Li, double in_dlambdainv,
		const CatheterSegmentType in_SegmentTypes[/*in_no_flex_seg+in_no_rigid_seg*/],
		double in_SegEndLambdas[/*in_no_flex_seg+in_no_rigid_seg*/], double in_LocMarkerLambdas[/*no_locmarkers*/],
		double in_rho[/*in_no_flex_seg+in_no_rigid_seg*/],
		double in_K[/*in_no_flex_seg*/][9], double in_Kinv[/*in_no_flex_seg*/][9], double in_ustar[/*in_no_flex_seg*/][3],
		double in_ActMass[/*in_no_act_set*/],
		double in_CoilAlignmentTurnAreaMatrix[/*in_no_act_set*/][9],
		double in_MagMoment[/*in_no_act_set*/][3], double in_fcumlambda[/*in_no_fcum_steps + 1*/][3],
		double in_B0[3], double in_g[3],
		bool in_CalculateEnergy, 
		bool in_FinalValueOnly,
		CRMIVPCoreParams& out_CoreParams) {
		// Process the incoming parameters (including changing from distal-proximal order to proximal-distal order)
		//   and package them to be passed to CRMSolverIVP_Core
		// Everything is copied to local variables (inside out_CoreParams), therefore, this function
		//   can be used to transfer data from global memory to device memory for subsequent computations
		// The packaged data can be used to call CRMSolverIVP_Core multiple times by changing
		//   only u[0..2] components of xi --- other parameters should not change

		int32_t in_no_segments = in_no_flex_seg + in_no_rigid_seg;
		// local copies for variables accessed out-of-order
		double *SegEndLambdas = new double[in_no_segments];
		for (int i = 0; i < in_no_segments; i++) SegEndLambdas[i] = in_SegEndLambdas[i];

		// create aliases for variables in CoreParams
		auto& xi = out_CoreParams.xi;

		auto& SegTypes = out_CoreParams.SegmentTypes;
		auto& FlexActIndex = out_CoreParams.FlexActIndex;
		auto& SegBounds = out_CoreParams.SegBounds;
		auto& SegSteps = out_CoreParams.SegSteps;

		auto& InsertedLength = out_CoreParams.InsertedLength;
		auto& dlambdainv = out_CoreParams.dlambdainv;
		auto& rho = out_CoreParams.rho;
		auto& K = out_CoreParams.K;
		auto& Kinv = out_CoreParams.Kinv;
		auto& ustar = out_CoreParams.ustar;
		auto& ActMass = out_CoreParams.ActMass;
		auto& CoilAlignmentTurnAreaMatrix = out_CoreParams.CoilAlignmentTurnAreaMatrix;
		auto& fcumlambda = out_CoreParams.fcumlambda;
		//	auto & ftip = out_CoreParams.ftip;
		auto& CalculateEnergy = out_CoreParams.CalculateEnergy;
		auto& FinalValueOnly = out_CoreParams.FinalValueOnly;
		auto& LocMarkers = out_CoreParams.LocMarkers;

		auto& B0 = out_CoreParams.B0;
		auto& g = out_CoreParams.g;
		auto& MagMoment = out_CoreParams.MagMoment;

		auto& StartSegmentIndex = out_CoreParams.StartSegmentIndex;
		auto& NextLocMarker = out_CoreParams.NextLocMarker;
		auto& p_atLocMarkers = out_CoreParams.p_atLocMarkers;

		// local variables
		double IntegrationStepSize = in_IntegrationStepSize;
		double DeltaSInv = 1.0 / IntegrationStepSize;
		double tempdouble;

		// process parameters as needed and copy into CoreParams
		mCopy_AB<NUM_STATES>(in_x_0, xi);		// Initial value of the state for the next segment to be integrated
												//  States are packed p[0..2],R[0..8],u[0..2]  (R: 3x3 matrix stored in row major order R11 R12 R13 R21 R22 R23 R31 R32 R33)
		mCopy_AB<3>(in_B0, B0);					//  B0 field vector of the MRI scanner (in spatial coordinates)
		mCopy_AB<3>(in_g, g);					//  gravity vector (in spatial coordinates)

		dlambdainv = in_dlambdainv;				// reciprocal of dlambda (lambda stepsize used in discretizing fcumlambda)
		CalculateEnergy = in_CalculateEnergy;	// Flag used to indicate if catheter potential energy should be calculated (true) or not (false)
		FinalValueOnly = in_FinalValueOnly;		// Flag used to indicate if only final value (xf) is returned (true) or if Marker Locations are returned as well (false)
		InsertedLength = in_Li;					// Inserted Length (length of the catheter from the entry point to the tip)
		// If the catheter is inserted more than the length of the catheter, clamp it to catheter length
		if (InsertedLength > SegEndLambdas[in_no_segments - 1]) InsertedLength = SegEndLambdas[in_no_segments - 1];

		// WE ARE GOING TO REORDER SEGMENT AND ACTUATOR UNITS SO THAT THEY ARE ORDERED FROM THE INSERTION POINT TO THE TIP
		//   I.E., SWITCH TO PROXIMAL TO DISTAL ORDERING

		StartSegmentIndex = in_no_segments - 1;		// Index of the segment where the integration to solve IVP will start -- the segment located at the entry point; note that segment indices start at 0
		SegBounds[in_no_segments] = InsertedLength;	// End s value of last segment is s=InsertedLength
												// Entry point has a value of s=0 -- we may not simulate full length of the most proximal segment in the chamber
		for (int i = 0; i < in_no_segments; i++) {
			tempdouble = InsertedLength - SegEndLambdas[i];
			if (tempdouble > 0.0) {
				SegBounds[(in_no_segments - 1) - i] = tempdouble;
				StartSegmentIndex--;				// Integration will start at the previous segment
			}
			else {
				SegBounds[(in_no_segments - 1) - i] = 0.0;	// This segment boundary is still inside the sheath
			}
		}

		for (int i = 0; i < in_no_flex_seg; i++) { 		// flexible catheter segment
			// Calculate the number of integration steps based on the given IntegrationStepSize
			SegSteps[i] = int(ceil((SegBounds[2 * i + 1] - SegBounds[2 * i]) * DeltaSInv));
		}

		NextLocMarker = in_no_locmarkers;
		if (!FinalValueOnly) {
			// While changing the order and converting from lambda to s,
			//   also find the index of the first localization marker after the entry point
			//   and assign (extrapolated) locations to markers which are still inside the sheath
			for (int i = 0; i < in_no_locmarkers; i++) {
				tempdouble = InsertedLength - in_LocMarkerLambdas[i];
				LocMarkers[(in_no_locmarkers - 1) - i] = tempdouble;
				if (tempdouble > 0.0) NextLocMarker--;
				else {   // and assign (extrapolated) locations to markers which are still inside the sheath
					LocMarkerUpdate(p_atLocMarkers[(in_no_locmarkers - 1) - i], xi, tempdouble);
				}
			}
		}

		// be careful - order is reversed in SegmentTypes, FlexActIndex, K, Kinv, ustar, rho, ActMass, CoilAlignmentTurnAreaMatrix, and MagMoment
		int32_t flexcnt = 0, actcnt = 0;
		for (int i = in_no_segments - 1; i >= 0; i--) {  // we will loop backwards to identify the counts
			SegTypes[(in_no_segments - 1) - i] = in_SegmentTypes[i];
			// we are also going to store the indices of the flexible segments and the actuators
			if (in_SegmentTypes[i] == CatheterSegmentType::FLEXIBLE) FlexActIndex[(in_no_segments - 1) - i] = flexcnt++;
			else if (in_SegmentTypes[i] == CatheterSegmentType::RIGID_WITH_ACTUATOR)  FlexActIndex[(in_no_segments - 1) - i] = actcnt++;
		}
		for (int i = 0; i < in_no_segments; i++) {
			rho[(in_no_segments - 1) - i] = in_rho[i];
		}
		for (int i = 0; i < in_no_flex_seg; i++) {
			for (int j = 0; j < 9; j++) {
				K[(in_no_flex_seg - 1) - i][j] = in_K[i][j];
				Kinv[(in_no_flex_seg - 1) - i][j] = in_Kinv[i][j];
			}
			for (int j = 0; j < 3; j++) {
				ustar[(in_no_flex_seg - 1) - i][j] = in_ustar[i][j];
			}
		}
		for (int i = 0; i < in_no_act_set; i++) {
			for (int j = 0; j < 3; j++) {
				MagMoment[(in_no_act_set - 1) - i][j] = in_MagMoment[i][j];
			}
			ActMass[(in_no_act_set - 1) - i] = in_ActMass[i];
			mCopy_AB<9>(in_CoilAlignmentTurnAreaMatrix[i], CoilAlignmentTurnAreaMatrix[(in_no_act_set - 1) - i]);
		}

		for (int i = 0; i < in_no_fcum_steps + 1; i++) for (int j = 0; j < 3; j++)
			fcumlambda[i][j] = in_fcumlambda[i][j];

		delete[] SegEndLambdas; // local variable
	}


	void CRMSolverIVP_Core(CRMIVPCoreParams& in_params,
		double in_deltau[3], double in_ftip[3],
		StateVector& out_x_N, double out_MomentResidual[3],
		double &out_PotentialEnergy,
		double out_p_atLocMarkers[/*in_params.no_locmarkers*/][3],
		double out_p_atActuators[/*in_params.no_act_set*/][3],
		double out_R_atActuators[/*in_params.no_act_set*/][9]) {

		// convenience definitions
		const double Identity3x3[9] = { 1,0,0,0,1,0,0,0,1 };
		const double Zero3x3[9] = { 0,0,0,0,0,0,0,0,0 };
		const double Zero3[3] = { 0,0,0 };

		double l_zero[3] = { 0.0,0.0,0.0 };
		double h;							// integration stepsize
		double RigidSegmentLength;			// Length of the rigid segment - intermediate variable
		double HalfRigidSegmentLength;		// Half of the length of the rigid segment - intermediate variable
		double Residual_im1[3] = { 0.0,0.0,0.0 };   // moment residual from the last flexible segment processed
		double deltau[3];					// intermediate variable
		int   fsegno; 						// flexible segment no
		int32_t  actno, fsegi, fsegip1;		// actuator no, flexible segment before, flexible segment after
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
		auto& ActMass = in_params.ActMass;
		auto& rho = in_params.rho;
		auto& fcumlambda = in_params.fcumlambda;
		auto& CalculateEnergy = in_params.CalculateEnergy;
		auto& FinalValueOnly = in_params.FinalValueOnly;
		auto& LocMarkers = in_params.LocMarkers;
		auto& g = in_params.g;
		auto& B0 = in_params.B0;
		auto& MagMoment = in_params.MagMoment;
		auto& CoilAlignmentTurnAreaMatrix = in_params.CoilAlignmentTurnAreaMatrix;

		StateVector xi;  		//  Initial value of the state for the next segment to be integrated
		auto& xf = out_x_N;					//  Final value of the state for the last segment integrated
		auto& Residual = out_MomentResidual;	// Residual at the catheter tip -- will be returned

		// Positions at markers (ordered proximal to distal)
		// we cannot just pass the out_p_atLocMarkers to ABM4, since the order will need to be reversed from ABM4 output (proximal to distal) to out_p_atLocMarkers (distal to proximal)
		// we will pass in_params.p_atLocMarkers to ABM4, since the entries in p_atLocMarkers before InitialLocMarker will not be changed by ABM4; the rest of the entries are uninitialized
		auto& p_atLocMarkers = in_params.p_atLocMarkers;

		// initial conditions for the regular states
		// we need to copy xi from in_params to the local variable and update it with u[0..2] specified in in_u
		mCopy_AB<3>(in_params.xi + 0, xi._p);
		mCopy_AB<9>(in_params.xi + 3, xi._R);
		//since input is in_deltau, not in_u, we will not perform this, but instead use the initialization after we find the first flexible segment
		//mCopy_AB<3>(in_u, xi._u);

		// initialize out_PotentialEnergy
		out_PotentialEnergy = 0.0;
		// intermediate varaibles used in potential energy calculation
		double RscTB0[3];
		double DeltaPE = 0.0;
		double gTpsum, mgTpmid;

		// define the structure that will used to pass parameters to the CRMIntegrand
		CRMIntegrandParams IntegrandParams;
		// these parameters are same for all segments
		IntegrandParams.dlambdainv = dlambdainv;
		IntegrandParams.Li = InsertedLength;
		IntegrandParams.l = l_zero;  // we are assuming the distributed moment on the catheter body is zero
		IntegrandParams.no_fcum_steps = no_fcum_steps;
		IntegrandParams.fcumlambda = fcumlambda;
		IntegrandParams.ftip = ftip;
		IntegrandParams.g = g;


		// If the starting segment is a rigid segment, then we will need to move initial conditions to the start of the next flexible segment and change the StartingSegment to that segment
		while ( (SegTypes[StartSegmentIndex]==CatheterSegmentType::RIGID || SegTypes[StartSegmentIndex]==CatheterSegmentType::RIGID_WITH_ACTUATOR) 
				&& (StartSegmentIndex < no_segments) ) {
			RigidSegmentLength = SegBounds[StartSegmentIndex + 1] - SegBounds[StartSegmentIndex];	// how far we need to move along the length of the rigid segment to reach the next flexible segment
			for (int i = 0; i < 3; i++) { 										// u[0..2] and R[0..8] remain the same
				xi._p[i] = xi._p[i] + RigidSegmentLength * xi._R[i * 3 + 2];	// p[0..2] will translate along the z direction of the R matrix (3rd column)
			}
			StartSegmentIndex++;  // move the start segment to the next segment
			if (!FinalValueOnly) {
				// are there any localization markers?  If so, calculate their positions
				CalculateLocMarkers(NextLocMarker, xi, LocMarkers, SegBounds[StartSegmentIndex], p_atLocMarkers, no_locmarkers);  //	xi has already been updated
			}
		}

		// if input is in_deltau, uncomment the following code, which will make input incremental over ustar 
		if (StartSegmentIndex < no_segments) // we are making sure that all of the segments in the chamber are not rigid
			mAdd_AB<3, 1>(in_deltau, ustar[FlexActIndex[StartSegmentIndex]], xi._u);
		

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
			if (SegTypes[i] == CatheterSegmentType::FLEXIBLE) {  // Flexible Segment
				// Prepare the CRMIntegrand Parameters
				fsegno = FlexActIndex[i]; // flexible segment no

				// Calculate the actual stepsize, based on the number of steps
				h = (SegBounds[i + 1] - SegBounds[i]) / (SegSteps[fsegno] * 1.0);

				// assign the parameters that vary from segment to segment
				IntegrandParams.K = K[fsegno];
				IntegrandParams.Kinv = Kinv[fsegno];
				IntegrandParams.ustar = ustar[fsegno];
				IntegrandParams.rho = rho[i];

				// Integrate
				ABM4(xi, SegBounds[i], SegSteps[fsegno], h,
					IntegrandParams, no_locmarkers,
					CalculateEnergy,
					FinalValueOnly, LocMarkers, NextLocMarker,
					xf, DeltaPE, p_atLocMarkers);

				// add strain and gravitational potential energy
				if (CalculateEnergy) out_PotentialEnergy += DeltaPE;

				LastSegmentIsRigid = false;

				// residual = K (u1 - u1star)
				mSub_AB<3, 1>(xf._u, ustar[fsegno], deltau);
				mMult_AB<3, 3, 1>(K[fsegno], deltau, Residual);

			}
			else {  // Need to do actuation/rigid segment calculations to transfer Initial Conditions to next flexible segment
				RigidSegmentLength = (SegBounds[i + 1] - SegBounds[i]);
				if (SegTypes[i] == CatheterSegmentType::RIGID) {  // not actuated rigid segment
					MMpoint = Zero3;				// points to a zero vector
					CATAMpoint = Zero3x3;			// points to a zero matrix
				}
				else {  // this is a rigid link with an actuator coil
					actno = FlexActIndex[i];		// actuator no
					MMpoint = MagMoment[actno];		// points to MagMoment of the actuator
					CATAMpoint = CoilAlignmentTurnAreaMatrix[actno];	// points to the CoilAlignmentTurnAreaMatrix of the actuator
					mCopy_AB<9>(xi._R, out_R_atActuators[(in_params.no_act_set - 1) - actno]);		// return the position orientation of the actuation coil wrt the 
					HalfRigidSegmentLength = RigidSegmentLength * 0.5;								//    spatial frame (p_sc and R_sc at coil)
					for (int j = 0; j < 3; j++) {													//    note that out_p_atActuators and out_R_atActuators is ordered distal to proximal
						out_p_atActuators[(in_params.no_act_set - 1) - actno][j] = xi._p[j] + xi._R[j * 3 + 2] * HalfRigidSegmentLength;  // p_sc is at the midpoint of the coil
					}
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
				if ( ((i+1) >= no_segments) || (SegTypes[i+1] != CatheterSegmentType::FLEXIBLE) ) { // next segment is not flexible or we are at the last segment
					usip1point = Zero3;				// points to a zero vector
					Kinvip1point = Identity3x3;		// points to an identity matrix
				}
				else {
					fsegip1 = FlexActIndex[i + 1];
					usip1point = ustar[fsegip1];	// points to ustar of next link
					Kinvip1point = Kinv[fsegip1];	// points to Kinv of next link
				}
				CRMSolverIVP_PropagateBCThroughRigidLink(xf, Residual, RigidSegmentLength, MMpoint, CATAMpoint, B0,
					usipoint, Kipoint, usip1point, Kinvip1point, xi, Residual_im1);

				if ((CalculateEnergy) && (SegTypes[i] == CatheterSegmentType::RIGID_WITH_ACTUATOR)) {  
					// Calculate and add coil magnetic potential energy if catheter potential energy needs to be calculated
					mMult_ATB<3, 3, 1>(xi._R, B0, RscTB0);
					mMult_ATB<3, 1, 1>(RscTB0, MMpoint, &DeltaPE);
					out_PotentialEnergy -= DeltaPE;   // - Bo_c^T * \mu_c  // calculated in coil frame 
					// Calculate and add gravitational potential energy if catheter potential energy needs to be calculated
					gTpsum = 0.0;
					for (int ix = 0; ix < 3; ix++) gTpsum += g[ix] * (xf._p[ix] + xi._p[ix]);
					mgTpmid = -0.5 * gTpsum;
					out_PotentialEnergy += (ActMass[actno] + rho[i] * RigidSegmentLength) * mgTpmid; //  mass * gravity * height = (ActMass + \rho * RigidSegmentLength) * (- g^T p_mid)
				}
				if (!FinalValueOnly) {
					// are there any localization markers?  If so, calculate their positions
					CalculateLocMarkers(NextLocMarker, xf, LocMarkers, SegBounds[i + 1], p_atLocMarkers, no_locmarkers);
				}

				LastSegmentIsRigid = true;

			}
			mCopy_AB<3>(Residual, Residual_im1);  // calculated moment residual will be the Residual_im1 for the next iteration
			xi = xf; // The calculated final values will be the initial value of the next iteration
		}

		// Copy marker locations to the output
		//    note that the order is being reversed
		if (!FinalValueOnly) {
			// are there any localization markers left that need to be calculated?  If so, calculate their locations
			if (NextLocMarker < no_locmarkers) {
				// note that these markers would be beyond the last Segment bound, so we will use the last location marker lambda coordinate as the end point, and extrapolate from end state
				CalculateLocMarkers(NextLocMarker, xf, LocMarkers, LocMarkers[no_locmarkers - 1], p_atLocMarkers, no_locmarkers); 
			}
			//then copy the marker locations to the output
			for (int i = 0; i < no_locmarkers; i++) {
				for (int j = 0; j < 3; j++) {
					out_p_atLocMarkers[i][j] = p_atLocMarkers[(no_locmarkers - 1) - i][j];
				}
			}
		}

	}



	void CRMSolverIVP_PropagateBCThroughRigidLink(StateVector& xi_ip1, double Residual_ip1[3],
		const double RigidSegmentLength, const double MagMoment[3], const double CoilAlignmentTurnAreaMatrix[9], const double B0[3],
		const double ustar_i[3], const double K_i[9], const double ustar_ip1[3], const double Kinv_ip1[9],
		const StateVector& xf_i, const double Residual_i[3]) {

		double muhat[9], RscTB0[3], Tb[3], K2invResidual[3]; // intermediate variables

		// p
		for (int j = 0; j < 3; j++) {
			xi_ip1._p[j] = xf_i._p[j] + xf_i._R[j * 3 + 2] * RigidSegmentLength;
		}
		// R
		mCopy_AB<9>(xf_i._R, xi_ip1._R);
		// u
		// Tb=\mu_c \cross R_sc^T B0,s
		wHat(MagMoment, muhat);
		mMult_ATB<3, 3, 1>(xf_i._R, B0, RscTB0);
		mMult_AB<3, 3, 1>(muhat, RscTB0, Tb);
		// u2=u2star + ( K2inv K1 (u1 - u1star ) - K2inv Tb )
		mSub_AB<3, 1>(Residual_i, Tb, Residual_ip1);  // Residual_ip1 = K1 (u1 - u1star) - Tb = Residual_i - Tb
		mMult_AB<3, 3, 1>(Kinv_ip1, Residual_ip1, K2invResidual);
		mAdd_AB<3, 1>(ustar_ip1, K2invResidual, xi_ip1._u);

	}


	void CalculateLocMarkers(int& NextLocMarker, const StateVector& xnext,
		const double LocMarkers[/*no_locmarkers*/], double SegBounds_ip1,
		double p_atLocMarkers[/*no_locmarkers*/][3], int32_t no_locmarkers) {

		double tempdouble;				// intermediate variable
		while ((NextLocMarker < no_locmarkers) && (LocMarkers[NextLocMarker] <= SegBounds_ip1)) {
			tempdouble = LocMarkers[NextLocMarker] - SegBounds_ip1;				// this would be a negative value
			LocMarkerUpdate(p_atLocMarkers[NextLocMarker], xnext, tempdouble);		//  xi has already been updated, it is the end point position
			NextLocMarker++;
		}
	}


	void LocMarkerUpdate(double p[3], const StateVector& xi, double t) {
		p[0] = xi._p[0] + t * xi._R[2];
		p[1] = xi._p[1] + t * xi._R[5];
		p[2] = xi._p[2] + t * xi._R[8];
	}


	void LocMarkerUpdate(double p[3], double xi[NUM_STATES], double t) {
		p[0] = xi[0] + t * xi[5];
		p[1] = xi[1] + t * xi[8];
		p[2] = xi[2] + t * xi[11];
	}


	void CRMIntegrand(double s, const StateVector& in_x, const CRMIntegrandParams in_Params,
		StateDerivativeVector& out_xdot) {

		double Length;
		double deltalambdainv;
		double fcum[3], ustardot[3];  //  We are assuming Kdot=0.0 (K=const)

		// copy inputs and parameters to local variables
		Length = in_Params.Li;
		deltalambdainv = in_Params.dlambdainv;
		auto& K = in_Params.K;
		auto& Kinv = in_Params.Kinv;
		auto& ustar = in_Params.ustar;
		auto& l = in_Params.l;
		for (int i = 0; i < 3; i++) {
			ustardot[i] = 0.0; //in_ustardot[i]; // we assume ustardot=0.0 since our rest shape model is piecewise constant curvature
		}

		// for simplicity, create aliases
		auto& u = in_x._u;
		auto& R = in_x._R;
		auto& udot = out_xdot._u;
#ifndef ANALYTICAL_SE3_STEP
		auto& p = in_x._p;				// we will not need this for analytical calculation
		auto& pdot = out_xdot._p;		// we will not need this for analytical calculation
		auto& Rdot = out_xdot._R;		// we will not need this for analytical calculation
#endif	


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

		// calculate u_hat
		double u_hat[9];
		wHat(u, u_hat);

		// udot = ustardot - Kinv*((um*K+Kdot)*(u-ustar_s) + e3m*R'*intf + R'*l); % udot
		//
		//   e3hat*R' = [ -r12 -r22 -r32; r11 r21 r31; 0 0 0];
		double e3hatRT[9];
		e3hatRT[0] = -R[1];   e3hatRT[1] = -R[4];   e3hatRT[2] = -R[7];
		e3hatRT[3] = R[0];    e3hatRT[4] = R[3];    e3hatRT[5] = R[6];
		e3hatRT[6] = 0.0;     e3hatRT[7] = 0.0;     e3hatRT[8] = 0.0;
		double e3hatRTfcum[3];
		mMult_AB<3, 3, 1>(e3hatRT, fcum, e3hatRTfcum);			//  e3m*R'*intf
		double RTl[3];
		mMult_ATB<3, 3, 1>(R, l, RTl);								// R'*l
		double umustar[3];
		mSub_AB<3, 1>(u, ustar, umustar);							// (u-ustar_s)
		double Kumustar[3], uhatKumustar[3];
		mMult_AB<3, 3, 1>(K, umustar, Kumustar);
		mMult_AB<3, 3, 1>(u_hat, Kumustar, uhatKumustar); 		 	//(um*K+Kdot)*(u-ustar_s)  assuming Kdot=0
		double sumterm[3];
		mAdd_ABC<3, 1>(uhatKumustar, e3hatRTfcum, RTl, sumterm);	// ((um*K+Kdot)*(u-ustar_s) + e3m*R'*intf + R'*l)
		double KinvSum[3];
		mMult_AB<3, 3, 1>(Kinv, sumterm, KinvSum);					// Kinv*((um*K+Kdot)*(u-ustar_s) + e3m*R'*intf + R'*l)
		mSub_AB<3, 1>(ustardot, KinvSum, udot);					// udot = ustardot - Kinv*((um*K+Kdot)*(u-ustar_s) + e3m*R'*intf + R'*l);

#ifndef ANALYTICAL_SE3_STEP
	// we will not need these for analytical calculation
	// Rdot = R*u_hat
		mMult_AB<3, 3, 3>(R, u_hat, Rdot);
		// pdot = R*e3,
		for (int i = 0; i < 3; i++) {
			pdot[i] = R[i * 3 + 2];
		}
#endif

		//xdot(1:3) = R*e3;             % pdot
		//xdot(4:12) = reshape(R*um,9,1); % Rdot   --- Note that matlab code reshapes in column major order while we are saving in row major order
		//xdot(13:15) = ustardot - Kinv*((um*K+Kdot)*(u-ustar_s) + e3m*R'*intf + R'*l); % udot
		//  note: the sample code has matlab indexing starting from 1 to 15

	}


	//
	//
	//	NUMERICAL INTEGRATION SUPPORT FUNCTIONS
	//
	//


	void Project_State_to_Manifold(StateVector& State) {
#ifdef ANALYTICAL_SE3_STEP

		// we don't need to do anything for analytical SE3 step

#else

		// project R to SO(3)
#error("Functionality not implemented!...\n");

#endif
	}



	// definitions needed for twist exponential calculation
#define EPS 1.0e-20   // the threshold for assuming ||u||^2 to be approximately 0, so that we should use pure translation equation


// calculate R_np1 and p_np1 analytically using twist exponential, without numerical integration
//   g_np1 = g_n * expm ( \hat{\xi}^b *h ),  where \xi^b= [ 0 0 1 u_n^T ]^T
//   g = [R p; 0 0 0 1];
	void SE3_Analytical_Step(double in_R_n[9], double in_p_n[3], double in_u_n[3], double h, double out_R_np1[9], double out_p_np1[3]) {
		double R_n[9], p_n[3], u_n[3];

		mCopy_AB<3 * 3>(in_R_n, R_n);
		mCopy_AB<3>(in_p_n, p_n);
		mCopy_AB<3>(in_u_n, u_n);

		// calculate R_np1 and p_np1 analytically, without numerical integration
		double Rdelta[9], pdelta[3];
		double umagsq, umagsqresp, umag, umagresp, unorm[3], delsumag, uu3dels[3], ImRuxv[3], ImRuxvpuuTvds[3], Rnpd[3];
		umagsq = vNormSq<3>(u_n);
		if (umagsq < EPS) {
			mCopy_AB<3 * 3>(R_n, out_R_np1);
			out_p_np1[0] = p_n[0] + R_n[2] * h;
			out_p_np1[1] = p_n[1] + R_n[5] * h;
			out_p_np1[2] = p_n[2] + R_n[8] * h;
		}
		else {
			umagsqresp = 1.0 / umagsq;
			umag = sqrt(umagsq);
			umagresp = 1.0 / umag;
			mMult_sA<3, 1>(umagresp, u_n, unorm);
			delsumag = h * umag;
			RodriguesExpanded(unorm, delsumag, Rdelta);
			mMult_sA<3, 1>(u_n[2] * h, u_n, uu3dels);
			ImRuxv[0] = Rdelta[0 * 3 + 1] * u_n[0] - Rdelta[0 * 3 + 0] * u_n[1] + u_n[1];
			ImRuxv[1] = Rdelta[1 * 3 + 1] * u_n[0] - Rdelta[1 * 3 + 0] * u_n[1] - u_n[0];
			ImRuxv[2] = Rdelta[2 * 3 + 1] * u_n[0] - Rdelta[2 * 3 + 0] * u_n[1];
			mAdd_AB<3, 1>(ImRuxv, uu3dels, ImRuxvpuuTvds);
			mMult_sA<3, 1>(umagsqresp, ImRuxvpuuTvds, pdelta);
			mMult_AB<3, 3, 3>(R_n, Rdelta, out_R_np1);
			mMult_AB<3, 3, 1>(R_n, pdelta, Rnpd);
			mAdd_AB<3, 1>(p_n, Rnpd, out_p_np1);
		}
	}

#undef EPS

	//
	//
	//  Robotic Kinematics Related Functions
	//
	//

	void wHat(const double in_w[3], double out_what[9]) {  // what is stored as a 1-dim array in row major order

		out_what[0] = 0.0;
		out_what[1] = -in_w[2];
		out_what[2] = in_w[1];
		out_what[3] = in_w[2];
		out_what[4] = 0.0;
		out_what[5] = -in_w[0];
		out_what[6] = -in_w[1];
		out_what[7] = in_w[0];
		out_what[8] = 0.0;

	}


	void wHat(const double in_w[3], double out_what[9], unsigned int stride) {  // what is stored as a 1-dim array in row major order

		out_what[0] = 0.0;
		out_what[1] = -in_w[2 * stride];
		out_what[2] = in_w[1 * stride];
		out_what[3] = in_w[2 * stride];
		out_what[4] = 0.0;
		out_what[5] = -in_w[0 * stride];
		out_what[6] = -in_w[1 * stride];
		out_what[7] = in_w[0 * stride];
		out_what[8] = 0.0;

	}


	void vee_from_so3(const double in_what[9], double out_w[3]) {

		out_w[0] = 0.5 * (in_what[2 * 3 + 1] - in_what[1 * 3 + 2]);
		out_w[1] = 0.5 * (in_what[0 * 3 + 2] - in_what[2 * 3 + 0]);
		out_w[3] = 0.5 * (in_what[1 * 3 + 0] - in_what[0 * 3 + 1]);

	}



	void RodriguesFormula(double in_w[3], double in_theta, double out_R[9]) {

		double I3x3[9] = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };

		double w[3], what[9], whatsq[9], temp1[9], temp2[9], temp3[9];
		double c = cos(in_theta);
		double s = sin(in_theta);
		double v = 1.0 - c;

		for (int i = 0; i < 3; i++) {
			w[i] = in_w[i];
		}

		wHat(w, what);
		mMult_sA<3, 3>(s, what, temp1);
		mMult_AB<3, 3, 3>(what, what, whatsq);
		mMult_sA<3, 3>(v, whatsq, temp2);
		mAdd_AB<3, 3>(I3x3, temp1, temp3);
		mAdd_AB<3, 3>(temp2, temp3, out_R);

	}


	void RodriguesExpanded(double in_w[3], double in_theta, double out_R[9]) {

		double w1 = in_w[0];
		double w2 = in_w[1];
		double w3 = in_w[2];
		double c = cos(in_theta);
		double s = sin(in_theta);
		double v = 1.0 - c;
		double w1s = w1 * s;
		double w2s = w2 * s;
		double w3s = w3 * s;
		double w1w2v = w1 * w2 * v;
		double w1w3v = w1 * w3 * v;
		double w2w3v = w2 * w3 * v;
		out_R[0 * 3 + 0] = w1 * w1 * v + c;
		out_R[0 * 3 + 1] = w1w2v - w3s;
		out_R[0 * 3 + 2] = w1w3v + w2s;
		out_R[1 * 3 + 0] = w1w2v + w3s;
		out_R[1 * 3 + 1] = w2 * w2 * v + c;
		out_R[1 * 3 + 2] = w2w3v - w1s;
		out_R[2 * 3 + 0] = w1w3v - w2s;
		out_R[2 * 3 + 1] = w2w3v + w1s;
		out_R[2 * 3 + 2] = w3 * w3 * v + c;

	}

	//
	// CRMIVPCoreParams Class member function declarations
	//

	CRMIVPCoreParams::CRMIVPCoreParams(int32_t no_flex, int32_t no_rigid, int32_t no_act, int32_t no_loc, int32_t no_fcums) {
		no_flex_seg = no_flex;
		no_rigid_seg = no_rigid;
		no_act_set = no_act;
		no_segments = no_flex + no_rigid;
		no_locmarkers = no_loc;
		no_fcum_steps = no_fcums;
		allocate_memory();
	}

	CRMIVPCoreParams::CRMIVPCoreParams(const CRMIVPCoreParams& t) {
		no_flex_seg = t.no_flex_seg;
		no_rigid_seg = t.no_rigid_seg;
		no_act_set = t.no_act_set;
		no_segments = t.no_segments;
		no_locmarkers = t.no_locmarkers;
		no_fcum_steps = t.no_fcum_steps;

		for (int32_t ix = 0; ix < NUM_STATES; ix++) xi[ix] = t.xi[ix];
		InsertedLength = t.InsertedLength;
		dlambdainv = t.dlambdainv;
		for (int32_t ix = 0; ix < 3; ix++) B0[ix] = t.B0[ix];
		for (int32_t ix = 0; ix < 3; ix++) g[ix] = t.g[ix];

		allocate_memory();
		for (int32_t ix = 0; ix < no_segments; ix++) SegmentTypes[ix] = t.SegmentTypes[ix];
		for (int32_t ix = 0; ix < no_segments; ix++) FlexActIndex[ix] = t.FlexActIndex[ix];
		StartSegmentIndex = t.StartSegmentIndex;
		for (int32_t ix = 0; ix < no_segments + 1; ix++) SegBounds[ix] = t.SegBounds[ix];
		for (int32_t ix = 0; ix < no_flex_seg; ix++) SegSteps[ix] = t.SegSteps[ix];
		for (int32_t ix = 0; ix < no_segments; ix++) rho[ix] = t.rho[ix];
		for (int32_t ix = 0; ix < no_flex_seg; ix++) for (int32_t jx = 0; jx < 9; jx++) K[ix][jx] = t.K[ix][jx];
		for (int32_t ix = 0; ix < no_flex_seg; ix++) for (int32_t jx = 0; jx < 9; jx++) Kinv[ix][jx] = t.Kinv[ix][jx];
		for (int32_t ix = 0; ix < no_flex_seg; ix++) for (int32_t jx = 0; jx < 3; jx++) ustar[ix][jx] = t.ustar[ix][jx];
		for (int32_t ix = 0; ix < no_act_set; ix++) ActMass[ix] = t.ActMass[ix];
		for (int32_t ix = 0; ix < no_act_set; ix++) for (int32_t jx = 0; jx < 3; jx++) MagMoment[ix][jx] = t.MagMoment[ix][jx];
		for (int32_t ix = 0; ix < no_act_set; ix++) for (int32_t jx = 0; jx < 9; jx++) CoilAlignmentTurnAreaMatrix[ix][jx] = t.CoilAlignmentTurnAreaMatrix[ix][jx];
		for (int32_t ix = 0; ix < no_act_set; ix++) for (int32_t jx = 0; jx < 3; jx++) p_atActuators[ix][jx] = t.p_atActuators[ix][jx];
		for (int32_t ix = 0; ix < no_act_set; ix++) for (int32_t jx = 0; jx < 9; jx++) R_atActuators[ix][jx] = t.R_atActuators[ix][jx];
		CalculateEnergy = t.CalculateEnergy;
		FinalValueOnly = t.FinalValueOnly;
		NextLocMarker = t.NextLocMarker;
		for (int32_t ix = 0; ix < no_locmarkers; ix++) LocMarkers[ix] = t.LocMarkers[ix];
		if (!FinalValueOnly)
			for (int32_t ix = 0; ix < no_locmarkers; ix++) for (int32_t jx = 0; jx < 3; jx++) p_atLocMarkers[ix][jx] = t.p_atLocMarkers[ix][jx];
		for (int32_t ix = 0; ix < no_fcum_steps + 1; ix++) for (int32_t jx = 0; jx < 3; jx++) fcumlambda[ix][jx] = t.fcumlambda[ix][jx];

	}

	CRMIVPCoreParams::~CRMIVPCoreParams() {
		if (memory_allocated) {
			delete[] SegmentTypes;
			delete[] FlexActIndex;
			delete[] SegBounds;
			delete[] SegSteps;
			delete[] rho;
			delete[] K;
			delete[] Kinv;
			delete[] ustar;
			delete[] ActMass;
			delete[] MagMoment;
			delete[] CoilAlignmentTurnAreaMatrix;
			delete[] p_atActuators;
			delete[] R_atActuators;
			delete[] LocMarkers;
			delete[] p_atLocMarkers;
			delete[] fcumlambda;
		}
	}

	void CRMIVPCoreParams::allocate_memory() {
		SegmentTypes = new CatheterSegmentType[no_segments]();
		FlexActIndex = new int32_t[no_segments]();
		SegBounds = new double[no_segments + 1]();
		SegSteps = new int32_t[no_flex_seg]();
		rho = new double[no_segments]();
		K = new double[no_flex_seg][9]();
		Kinv = new double[no_flex_seg][9]();
		ustar = new double[no_flex_seg][3]();
		ActMass = new double[no_act_set]();
		MagMoment = new double[no_act_set][3]();
		CoilAlignmentTurnAreaMatrix = new double[no_act_set][9]();
		p_atActuators = new double[no_act_set][3]();
		R_atActuators = new double[no_act_set][9]();
		LocMarkers = new double[no_locmarkers]();
		p_atLocMarkers = new double[no_locmarkers][3]();
		fcumlambda = new double[no_fcum_steps + 1][3]();
		memory_allocated = true;
	}







}
