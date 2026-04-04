#include <cmath>
#include <iostream>
#include "CRM.hpp"
#include "CRM_BVPIVP_APIDeclarations.hpp"
#include "CRM_IVPJacobian.hpp"
#include "minpack.hpp"

#define M_PI 3.14159265358979323846

namespace CRMCatheterModel {


	void CRMShootingMethodBVP(CRMShootingMethodParams in_Params,
		double in_deltau0_initialguess[3], double in_ftip_initialguess[3],
		double out_deltau0[3], double out_ftip[3], int& out_localmin) {

		ContactModeType ContactMode = in_Params.ContactMode;
		int NLEq_Dim;  // Dimension of the Nonlinear Equation to Solve
		if (ContactMode == ContactModeType::FREE_TIP) {
			NLEq_Dim = 3;
		}
		else { // FIXED_TIP
			NLEq_Dim = 6;
		}
		// Call CRMSolverIVP_Prep, to pre-process parameters
		double x_0[NUM_STATES];
		for (int i = 0; i < 3; i++) x_0[i] = in_Params.p0[i];
		for (int i = 0; i < 9; i++) x_0[i + 3] = in_Params.R0[i];
		for (int i = 0; i < 3; i++) x_0[i + 3 + 9] = std::nan("0");  //  This will be overridden in CRMSolverIVP_Core // in_u0_initialguess[i];
		bool CalculateEnergy = false;		// we do not need to calculate the catheter potential energy during BVP solution
		bool FinalValueOnly = true;			// we do not need to calculate the marker locations during BVP solution
		NLEqnParams NLEParams(in_Params.no_flex_seg, in_Params.no_rigid_seg, in_Params.no_act_set, in_Params.no_locmarkers, in_Params.no_fcum_steps);

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
			NLEParams);
		NLEParams.ContactMode = ContactMode;
		mCopy_AB<3>(in_Params.TipConstraintPoint, NLEParams.TipConstraintPoint);

		// Scale parameters and call the nonlinear equation solver
		const double DUSCALE_INV = 1.0 / IVALUE_SCALE_DU;
		const double FSCALE_INV = 1.0 / IVALUE_SCALE_F;
		double* initialguessscaled = new double[NLEq_Dim];
		double* returnedparamscaled = new double[NLEq_Dim];
		if (ContactMode == ContactModeType::FREE_TIP) {
			for (int i = 0; i < 3; i++) {
				initialguessscaled[i] = DUSCALE_INV * in_deltau0_initialguess[i];
				NLEParams.TipForce[i] = in_Params.TipForce[i];  // if the catheter is not in contact, the tip force specified within in_Params needs to be used; this would not be scaled as it is not changed by the solver
			}
		}
		else { // FIXED_TIP
			for (int i = 0; i < 3; i++) {
				initialguessscaled[i] = DUSCALE_INV * in_deltau0_initialguess[i];
				initialguessscaled[i + 3] = FSCALE_INV * in_ftip_initialguess[i];
			}

		}

		int localmin = 0;

#if defined( FK_TRUSTREGION )
		double* x = new double[NLEq_Dim]; // we will create a new variable here and not use initial guess scaled since truss-region-dogleg algorithm uses the same variable for both input and output
		double* residual = new double[NLEq_Dim];
		int info;
		double tol = TRUSTREGION_TOLERANCE;
		for (int i = 0; i < NLEq_Dim; i++) x[i] = initialguessscaled[i];

#if defined  (FK_TRUSTREGION_ANALYTICALJAC)
		TrustRegionDogleg_GivenJacobian<NLEqnParams>(CRM_NLEquation, CRM_NLEquation_AnalyticalJac, 3, x, residual, tol, info, NLEParams);
#else 	
		TrustRegionDogleg(CRM_NLEquation, NLEq_Dim, x, residual, tol, info, NLEParams);
#endif

		localmin = (info == 1) ? 0 : (info - 1);
		for (int i = 0; i < NLEq_Dim; i++) returnedparamscaled[i] = x[i];
		delete[] residual;
		delete[] x;
#else // undefined
		exit(1);
#endif

		if (ContactMode == ContactModeType::FREE_TIP) {
			for (int i = 0; i < 3; i++) {
				out_deltau0[i] = IVALUE_SCALE_DU * returnedparamscaled[i];
				out_ftip[i] = in_Params.TipForce[i];  // if it is free-tip, return the tip force specified within in_Params
			}
		}
		else { // FIXED_TIP
			for (int i = 0; i < 3; i++) {
				out_deltau0[i] = IVALUE_SCALE_DU * returnedparamscaled[i];
				out_ftip[i] = IVALUE_SCALE_F * returnedparamscaled[i + 3];
			}
		}
		out_localmin = localmin;
		delete[] initialguessscaled;
		delete[] returnedparamscaled;
	}


	void CRMShootingMethodBVP(
		int32_t in_no_flex_seg,								// Number of flexible segments
		int32_t in_no_rigid_seg,							// Number of rigid segments (including actuator segments and non-actuator rigid segments)
		int32_t in_no_act_set,								// Number of actuator sets
		int32_t	in_no_locmarkers,							// Number of localization markers
		ContactModeType in_ContactMode,						// Enumerated type defining catheter contact mode.  in_ContactMode == FREE_TIP if the catheter is not in contact with a surface, FIXED_TIP if catheter tip is constrained to be at TipContraintPoint
		double 	in_deltau0_initialguess[3],					// Initial guess for the local \delta curvature vector at the entry point ( u0 = deltau0 + ustar0 )
		double 	in_ftip_initialguess[3],					// Initial guess for the tip force (\lambda=0), used when in_ContactMode == FIXED_TIP
		double	in_InsertedLength, 							// Inserted Length (length of the catheter from the entry point to the tip)
		double 	in_ActuationCurrents[/*NUM_ACT_SET*/][3], 	// Actuation current for each of the actuation coils
															//    actuator units are numbered/ordered from the tip of the catheter towards the base (distal to proximal)
		double	in_TipConstraintPoint[3],					// The spatial coordinates of the point where the catheter tip is constrained to be (used if in_ContactMode == FIXED_TIP)
		double	in_TipForce[3],								// External point force (in spatial coordinates) applied at the tip of the catheter (\lambda = 0) - this value will not be used if in_ContactMode == FIXED_TIP 
		double	in_IntegrationStepSize,						// Stepsize used in numerical integration along the length of the catheter
		// Catheter Configuration Parameters
		double 	in_B0[3],									// B0 field vector of the MRI scanner (in spatial coordinates)
		double 	in_g[3],									// Gravity vector (in spatial coordinates)
		double 	in_p0[3],									// Catheter entry port position (in spatial coordinates)
		double 	in_R0[9],									// Catheter orientation at the entry port (relative to the spatial frame); 3x3 matrix stored in row major order
		// Catheter Model Parameters
		//	For all parameters below, segments and actuator units are numbered/ordered from the tip of the catheter towards the base (distal to proximal)
		double 	in_SegLengths[/*NUM_SEGMENTS*/],			// Array of segment lengths; NUM_SEGMENTS long array
		double	in_LocMarkers[/*no_locmarkers*/],			// Array of localization marker locations (in lambda coordinates); in_no_locmarkers long array
		double	in_InnerRadius[/*NUM_FLEX_SEG*/],			// Inner radii of the flexible catheter segments
		double	in_OuterRadius[/*NUM_FLEX_SEG*/],			// Outer radii of the flexible catheter segments
		double	in_YoungsModulus[/*NUM_FLEX_SEG*/],			// Youngs Moduli of the flexible catheter segments
		double	in_ShearModulus[/*NUM_FLEX_SEG*/],			// Shear Moduli of the flexible catheter segments
		double 	in_ustar[/*NUM_FLEX_SEG*/][3],				// Local curvature in unloaded configuration for each of the flexible segments; (NUM_FLEX_SEG)*3 long array, (NUM_FLEX_SEG) 3x1 vectors
															//		Actuator segments are assumed to be straight
		double 	in_CoilAlignmentAngles[/*NUM_ACT_SET*/][2],	// Coil Alignment Angles; NUM_ACT_SET*2 long array, for each actuator set, the angle for the first coil is relative to x axis, and the angle for the second coil is relative to y axis
		double 	in_CoilTurnAreaMat[/*NUM_ACT_SET*/][9], 	// Coil Turn Area matrices; NUM_ACT_SET*9 long array, NUM_ACT_SET 3x3 matrices stored in row major order
		double 	in_rho[/*NUM_SEGMENTS*/],					// Length density (mass per unit length) of the flexible catheter substrate (tubing); (NUM_SEGMENTS) long array
		double 	in_ActMass[/*NUM_ACT_SET*/],				// Actuator segment masses, does not include the flexible substrate; (NUM_ACT_SET) long array
		// Outputs
		double 	out_deltau0[3], 							// Local delta curvature vector at the entry point ( u0 = deltau0 + ustar0 ) calculated through the solution of BVP
		double 	out_ftip[3],								// Tip force calculated through the solution of BVP (\lambda=0) --- used when in_ContactMode == FIXED_TIP 
		int& out_localmin									// out_localmin!=0 if algorithms is stuck at a local minimum, or cannot make further progress
	) {

		// Copy input parameters to local variables --- needed for dataflow optimization
		CRMCatheterModelParams CathParams(in_no_flex_seg, in_no_rigid_seg, in_no_act_set, in_no_locmarkers);
		CatheterConfiguration CathConfig;
		CRMShootingMethodBVP_Prep(in_no_flex_seg, in_no_rigid_seg, in_no_act_set, in_no_locmarkers,in_B0, in_g, in_p0, in_R0,
			in_SegLengths, in_LocMarkers, in_InnerRadius, in_OuterRadius, in_YoungsModulus, in_ShearModulus,
			in_ustar, in_CoilAlignmentAngles, in_CoilTurnAreaMat, in_rho, in_ActMass,
			CathParams, CathConfig);

		double InsertedLength = in_InsertedLength;
		double ActuationCurrents[NUM_ACT_SET][3];
		mCopy_AB<NUM_ACT_SET * 3>(&(in_ActuationCurrents[0][0]), &(ActuationCurrents[0][0]));
		double IntegrationStepSize = in_IntegrationStepSize;
		// Calculate Shooting Method Parameter Set from model and configuration parameters
		CRMShootingMethodParams ShootingParams = CRMConstructShootingMethodParamSet(CathParams, CathConfig,
			InsertedLength, ActuationCurrents, in_ContactMode, in_TipConstraintPoint, in_TipForce,
			IntegrationStepSize);

		CRMShootingMethodBVP(ShootingParams, in_deltau0_initialguess, in_ftip_initialguess, out_deltau0, out_ftip, out_localmin);

	}


	void CRM_NLEquation(double in_x[], double out_y[], NLEqnParams Params) {

		StateVector x_N;
		double MomentResidual[3];
		double dummyPE;		// dummy variable for potential energy - value will not be used

		double deltau_0[3], ftip[3];
		// don't forget to scale parameters before passing to the CRMSolverIVP
		if (Params.ContactMode == ContactModeType::FREE_TIP) {
			for (int i = 0; i < 3; i++) {
				deltau_0[i] = IVALUE_SCALE_DU * in_x[i];
				ftip[i] = Params.TipForce[i];  // for free-tip, this parameter is not given by the nonlinear equation solver, and hence, does not need to be scaled
			}
		}
		else { // FIXED_TIP
			for (int i = 0; i < 3; i++) {
				deltau_0[i] = IVALUE_SCALE_DU * in_x[i];
				ftip[i] = IVALUE_SCALE_F * in_x[i + 3];
			}
		}
		// We will only call the IVP_Core, since preprocessing is already done
		CRMSolverIVP_Core(Params, deltau_0, ftip, x_N, MomentResidual, dummyPE, Params.p_atLocMarkers, Params.p_atActuators, Params.R_atActuators);

		// don't forget to scale parameters before returning to the nonlinear equation solver
		if (Params.ContactMode == ContactModeType::FREE_TIP) {
			for (int i = 0; i < 3; i++) {
				out_y[i] = RESIDUAL_SCALE_M * MomentResidual[i];
			}
		}
		else { // FIXED_TIP
			for (int i = 0; i < 3; i++) {
				out_y[i] = RESIDUAL_SCALE_M * MomentResidual[i];
				out_y[i + 3] = RESIDUAL_SCALE_P * (x_N._p[i] - Params.TipConstraintPoint[i]);
			}
		}

	}


	void CRM_NLEquation_AnalyticalJac(double in_x[], double out_y[], double out_fjac[], NLEqnParams Params) {

		AugmentedStateVector<IVPJacobiansMini> x_N;
		double MomentResidual[3], kJuu0[9];

		double deltau_0[3], ftip[3];
		// don't forget to scale parameters before passing to the CRMSolverIVP
		if (Params.ContactMode == ContactModeType::FREE_TIP) {
			for (int i = 0; i < 3; i++) {
				deltau_0[i] = IVALUE_SCALE_DU * in_x[i];
				ftip[i] = Params.TipForce[i];  // for free-tip, this parameter is not given by the nonlinear equation solver, and hence, does not need to be scaled
			}
		}
		else { // FIXED_TIP
			for (int i = 0; i < 3; i++) {
				deltau_0[i] = IVALUE_SCALE_DU * in_x[i];
				ftip[i] = IVALUE_SCALE_F * in_x[i + 3];
			}
		}
		// We will only call the IVP_Core, since preprocessing is already done
		CRMSolverIVP_CoreWithJacobian(Params, deltau_0, ftip, x_N, MomentResidual);

		// don't forget to scale parameters before returning to the nonlinear equation solver
		if (Params.ContactMode == ContactModeType::FREE_TIP) {
			for (int i = 0; i < 3; i++) {
				out_y[i] = RESIDUAL_SCALE_M * MomentResidual[i];
			}
			if ( Params.SegmentTypes[Params.no_segments - 1] == CatheterSegmentType::FLEXIBLE ) { // if last segment is flexible
				mMult_AB<3, 3, 3>(Params.K[Params.no_flex_seg - 1], x_N._u_u0, kJuu0);
			}
			else {  // if last segment is rigid
				mCopy_AB<3 * 3>(x_N._u_u0, kJuu0);
			}
			for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) out_fjac[i + j * 3] = RESIDUAL_SCALE_M * kJuu0[i * 3 + j] * IVALUE_SCALE_DU;  // Note that minpack uses Fortran style column-major ordering, not C sytle row-major ordering

		}
		else { // FIXED_TIP
			for (int i = 0; i < 3; i++) {
				out_y[i] = RESIDUAL_SCALE_M * MomentResidual[i];
				out_y[i + 3] = RESIDUAL_SCALE_P * (x_N._p[i] - Params.TipConstraintPoint[i]);
			}
			std::cerr << "Functionality Not Implemented!...\n";
			exit(1);
		}


	}



	void CRMShootingMethodBVP_Prep(
		int32_t in_no_flex_seg,
		int32_t in_no_rigid_seg,
		int32_t in_no_act_set,
		int32_t in_no_locmarkers,
		double in_B0[3], double in_g[3], double in_p0[3], double in_R0[9],
		double in_SegLengths[/*no_segments=no_flex_seg+no_rigid_seg*/], double in_LocMarkers[/*in_no_locmarkers*/],
		double in_InnerRadius[/*no_flex_seg*/], double in_OuterRadius[/*no_flex_seg*/],
		double in_YoungsModulus[/*no_flex_seg*/], double in_ShearModulus[/*no_flex_seg*/],
		double in_ustar[/*no_flex_seg*/][3],
		double in_CoilAlignmentAngles[/*no_act_set*/][2], double in_CoilTurnAreaMat[/*no_act_set*/][9],
		double in_rho[/*no_segments=no_flex_seg+no_rigid_seg*/], double in_ActMass[/*no_act_set*/], 
		CRMCatheterModelParams& CathParams, CatheterConfiguration& CathConfig) {

		int32_t in_no_segments = in_no_flex_seg + in_no_rigid_seg;
		for (int i = 0; i < in_no_segments; i++) CathParams.SegLengths[i] = in_SegLengths[i];
		for (int i = 0; i < in_no_segments; i++) CathParams.rho[i] = in_rho[i];
		for (int i = 0; i < in_no_locmarkers; i++) CathParams.LocMarkers[i] = in_LocMarkers[i];
		for (int i = 0; i < in_no_flex_seg; i++) CathParams.InnerRadius[i] = in_InnerRadius[i];
		for (int i = 0; i < in_no_flex_seg; i++) CathParams.OuterRadius[i] = in_OuterRadius[i];
		for (int i = 0; i < in_no_flex_seg; i++) CathParams.YoungsModulus[i] = in_YoungsModulus[i];
		for (int i = 0; i < in_no_flex_seg; i++) CathParams.ShearModulus[i] = in_ShearModulus[i];
		for (int i = 0; i < in_no_flex_seg; i++) for (int j = 0; j < 3; j++) CathParams.ustar[i][j] = in_ustar[i][j];
		for (int i = 0; i < in_no_act_set; i++) CathParams.ActMass[i] = in_ActMass[i];
		for (int i = 0; i < in_no_act_set; i++) for (int j = 0; j < 2; j++) CathParams.CoilAlignmentAngles[i][j] = in_CoilAlignmentAngles[i][j];
		for (int i = 0; i < in_no_act_set; i++) for (int j = 0; j < 9; j++) CathParams.CoilTurnAreaMat[i][j] = in_CoilTurnAreaMat[i][j];
		mCopy_AB<3>(in_B0, CathConfig.B0);
		mCopy_AB<3>(in_g, CathConfig.g);
		mCopy_AB<3>(in_p0, CathConfig.p0);
		mCopy_AB<9>(in_R0, CathConfig.R0);

	}

	CRMShootingMethodParams  CRMConstructShootingMethodParamSet(
		CRMCatheterModelParams CathParams, CatheterConfiguration CathConfig,
		double InsertionLength, double ActuationCurrents[NUM_ACT_SET][3],
		ContactModeType ContactMode,
		double TipConstraintPoint[3], double TipForce[3],
		double IntegrationStepSize) {

		// calculate no_fcum_steps
		double length = 0.0;
		double* SegEndLambdas = new double[CathParams.no_segments];  // allocate a temporary storage to store SegEndLambdas
		for (int i = 0; i < CathParams.no_segments; i++) {
			length += CathParams.SegLengths[i];
			SegEndLambdas[i] = length;
		}
		int32_t no_fcum_steps = int32_t(ceil(length / FCUM_DLAMBDA));

		// construct ShootingParams
		CRMShootingMethodParams ShootingParams(CathParams.no_flex_seg, CathParams.no_rigid_seg, CathParams.no_act_set, CathParams.no_locmarkers, no_fcum_steps);

		ShootingParams.IntegrationStepSize = IntegrationStepSize;
		double dlambda = length / double(no_fcum_steps);
		ShootingParams.dlambdainv = 1.0 / dlambda;

		mCopy_AB<3>(CathConfig.B0, ShootingParams.B0);
		mCopy_AB<3>(CathConfig.g, ShootingParams.g);
		mCopy_AB<9>(CathConfig.R0, ShootingParams.R0);
		mCopy_AB<3>(CathConfig.p0, ShootingParams.p0);

		ShootingParams.ContactMode = ContactMode;
		mCopy_AB<3>(TipConstraintPoint, ShootingParams.TipConstraintPoint);
		mCopy_AB<3>(TipForce, ShootingParams.TipForce);

		ShootingParams.Li = MIN(MAX(InsertionLength, 0), length);

		int32_t actcnt = 0;
		int32_t* ActNos = new int32_t[CathParams.no_segments];
		for (int i = 0; i < CathParams.no_segments; i++) {
			ShootingParams.SegmentTypes[i] = CathParams.SegmentTypes[i];
			if (CathParams.SegmentTypes[i] == CatheterSegmentType::RIGID_WITH_ACTUATOR) ActNos[i] = actcnt++;
			else ActNos[i] = -1;
		}

		// copy SegEndLambdas from temporary storage to ShootingParams.SegEndLambdas
		for (int i = 0; i < CathParams.no_segments; i++) ShootingParams.SegEndLambdas[i] = SegEndLambdas[i];
		delete[] SegEndLambdas; // delete temporary storage

		// copy rho values
		for (int i = 0; i < CathParams.no_segments; i++) {
			ShootingParams.rho[i] = CathParams.rho[i];
		}

		double oR, iR, mI, pmI, E, G, K[9], Kinv[9];
		for (int i = 0; i < CathParams.no_flex_seg; i++) {
			oR = CathParams.OuterRadius[i];
			iR = CathParams.InnerRadius[i];
			E = CathParams.YoungsModulus[i];
			G = CathParams.ShearModulus[i];
			mI = 0.25 * M_PI * (POW4(oR) - POW4(iR));	// Area moment of inertia along x - axis
			pmI = 0.5 * M_PI * (POW4(oR) - POW4(iR));	// Polar moment of inertia of area
			K[0] = E * mI;		K[1] = 0.0;			K[2] = 0.0;
			K[3] = 0.0;			K[4] = E * mI;		K[5] = 0.0;
			K[6] = 0.0;			K[7] = 0.0;			K[8] = G * pmI;
			Kinv[0] = 1.0 / (E * mI);		Kinv[1] = 0.0;				Kinv[2] = 0.0;
			Kinv[3] = 0.0;				Kinv[4] = 1.0 / (E * mI);		Kinv[5] = 0.0;
			Kinv[6] = 0.0;				Kinv[7] = 0.0;				Kinv[8] = 1.0 / (G * pmI);
			mCopy_AB<9>(K, ShootingParams.K[i]);
			mCopy_AB<9>(Kinv, ShootingParams.Kinv[i]);
		}
		for (int i = 0; i < CathParams.no_flex_seg; i++) for (int j = 0; j < 3; j++) ShootingParams.ustar[i][j] = CathParams.ustar[i][j];
		double CoilAlignMat[9], c0, s0, c1, s1;
		double tempf[3];
		for (int i = 0; i < CathParams.no_act_set; i++) {
			ShootingParams.ActMass[i] = CathParams.ActMass[i];
			c0 = cos(CathParams.CoilAlignmentAngles[i][0]);
			s0 = sin(CathParams.CoilAlignmentAngles[i][0]);
			c1 = cos(CathParams.CoilAlignmentAngles[i][1]);
			s1 = sin(CathParams.CoilAlignmentAngles[i][1]);
			mMult_AB<3, 3, 1>(CathParams.CoilTurnAreaMat[i], ActuationCurrents[i], tempf);
			CoilAlignMat[0] = c0;	CoilAlignMat[1] = -s1;	CoilAlignMat[2] = 0.0;
			CoilAlignMat[3] = s0;	CoilAlignMat[4] = c1;	CoilAlignMat[5] = 0.0;
			CoilAlignMat[6] = 0.0;	CoilAlignMat[7] = 0.0;	CoilAlignMat[8] = 1.0;
			mMult_AB<3, 3, 1>(CoilAlignMat, tempf, ShootingParams.MagMoment[i]);
			mMult_AB<3, 3, 3>(CoilAlignMat, CathParams.CoilTurnAreaMat[i], ShootingParams.CoilAlignmentTurnAreaMatrix[i]);
		}

		ShootingParams.fcumlambda[0][0] = 0.0;
		ShootingParams.fcumlambda[0][1] = 0.0;
		ShootingParams.fcumlambda[0][2] = 0.0;
		double cumpos = 0.0, lastpos = 0.0, mass = 0.0, segstart, segend, currpos;
		int32_t curr_segment = 0, last_segment = 0;
		// calculation loop
		mass = 0.0;
		for (int32_t i = 1; i < no_fcum_steps + 1; i++) {
			cumpos = i * dlambda;
			while ((cumpos > ShootingParams.SegEndLambdas[curr_segment]) && (curr_segment < CathParams.no_segments)) curr_segment++;		// let's find the current segment number,
			for (int32_t j = last_segment; j <= curr_segment; j++) {
				if (j == 0) segstart = 0.0; else segstart = ShootingParams.SegEndLambdas[j - 1];
				segend = ShootingParams.SegEndLambdas[j];
				currpos = MIN(ShootingParams.SegEndLambdas[j], cumpos);
				mass += CathParams.rho[j] * (currpos - lastpos); // we will add the partial mass of the flexible substrate
				if (CathParams.SegmentTypes[j] == CatheterSegmentType::RIGID_WITH_ACTUATOR) {
					if (segend == segstart) mass += CathParams.ActMass[ActNos[j]];  // point mass, since segment_length == 0 
					else mass += CathParams.ActMass[ActNos[j]] * (currpos - lastpos) / (segend - segstart); // we will add the partial mass of the actuator
				}
				lastpos = currpos;
			}
			last_segment = curr_segment;
			mMult_sA<3, 1>(mass, CathConfig.g, ShootingParams.fcumlambda[i]);
		}

		delete[] ActNos;

		for (int i = 0; i < CathParams.no_locmarkers; i++) ShootingParams.LocMarkerLambdas[i] = CathParams.LocMarkers[i];

		return ShootingParams;
	}


	void CRMUpdateShootingMethodParamSetWithNewActuationCurrents(
		CRMShootingMethodParams &ShootingParams,
		double ActuationCurrents[NUM_ACT_SET][3]) {

		for (int i = 0; i < ShootingParams.no_act_set; i++) {
			mMult_AB<3, 3, 1>(ShootingParams.CoilAlignmentTurnAreaMatrix[i], ActuationCurrents[i], ShootingParams.MagMoment[i]);
		}

	}

	//
	// CRMCatheterModelParams Class member function declarations
	//

	CRMCatheterModelParams::CRMCatheterModelParams(int32_t no_flex, int32_t no_rigid, int32_t no_act, int32_t no_loc) {
		no_flex_seg = no_flex;
		no_rigid_seg = no_rigid;
		no_act_set = no_act;
		no_segments = no_flex + no_rigid;
		no_locmarkers = no_loc;
		allocate_memory();
	}

	CRMCatheterModelParams::CRMCatheterModelParams(const CRMCatheterModelParams& t) {

		no_flex_seg = t.no_flex_seg;
		no_rigid_seg = t.no_rigid_seg;
		no_act_set = t.no_act_set;
		no_segments = t.no_segments;
		no_locmarkers = t.no_locmarkers;

		allocate_memory();

		for (size_t ix = 0; ix < no_segments; ix++) SegmentTypes[ix] = t.SegmentTypes[ix];
		for (size_t ix = 0; ix < no_segments; ix++) SegLengths[ix] = t.SegLengths[ix];
		for (size_t ix = 0; ix < no_segments; ix++) rho[ix] = t.rho[ix];
		for (size_t ix = 0; ix < no_flex_seg; ix++) InnerRadius[ix] = t.InnerRadius[ix];
		for (size_t ix = 0; ix < no_flex_seg; ix++) OuterRadius[ix] = t.OuterRadius[ix];
		for (size_t ix = 0; ix < no_flex_seg; ix++) YoungsModulus[ix] = t.YoungsModulus[ix];
		for (size_t ix = 0; ix < no_flex_seg; ix++) ShearModulus[ix] = t.ShearModulus[ix];
		for (size_t ix = 0; ix < no_flex_seg; ix++) for (size_t jx = 0; jx < 3; jx++) ustar[ix][jx] = t.ustar[ix][jx];
		for (size_t ix = 0; ix < no_act_set; ix++) for (size_t jx = 0; jx < 2; jx++) CoilAlignmentAngles[ix][jx] = t.CoilAlignmentAngles[ix][jx];
		for (size_t ix = 0; ix < no_act_set; ix++) for (size_t jx = 0; jx < 9; jx++) CoilTurnAreaMat[ix][jx] = t.CoilTurnAreaMat[ix][jx];
		for (size_t ix = 0; ix < no_act_set; ix++) ActMass[ix] = t.ActMass[ix];
		for (size_t ix = 0; ix < no_locmarkers; ix++) LocMarkers[ix] = t.LocMarkers[ix];

	}

	CRMCatheterModelParams::~CRMCatheterModelParams() {
		if (memory_allocated) {
			delete[] SegmentTypes;
			delete[] SegLengths;
			delete[] LocMarkers;
			delete[] InnerRadius;
			delete[] OuterRadius;
			delete[] YoungsModulus;
			delete[] ShearModulus;
			delete[] ustar;
			delete[] CoilAlignmentAngles;
			delete[] CoilTurnAreaMat;
			delete[] rho;
			delete[] ActMass;
		}
	}

	void CRMCatheterModelParams::allocate_memory() {

		SegmentTypes = new CatheterSegmentType[no_segments]();
		SegLengths = new double[no_segments]();
		rho = new double[no_segments]();
		InnerRadius = new double[no_flex_seg]();
		OuterRadius = new double[no_flex_seg]();
		YoungsModulus = new double[no_flex_seg]();
		ShearModulus = new double[no_flex_seg]();
		ustar = new double[no_flex_seg][3]();
		CoilAlignmentAngles = new double[no_act_set][2]();
		CoilTurnAreaMat = new double[no_act_set][9]();
		ActMass = new double[no_act_set]();
		LocMarkers = new double[no_locmarkers]();

		memory_allocated = true;
	}


	//
	// CRMShootingMethodParams Class member function declarations
	//

	CRMShootingMethodParams::CRMShootingMethodParams(int32_t no_flex, int32_t no_rigid, int32_t no_act, int32_t no_loc, int32_t no_fcum) {
		no_flex_seg = no_flex;
		no_rigid_seg = no_rigid;
		no_act_set = no_act;
		no_segments = no_flex_seg + no_rigid_seg;
		no_locmarkers = no_loc;
		no_fcum_steps = no_fcum;
		allocate_memory();
	}

	CRMShootingMethodParams::CRMShootingMethodParams(const CRMShootingMethodParams& t) {

		no_flex_seg = t.no_flex_seg;
		no_rigid_seg = t.no_rigid_seg;
		no_act_set = t.no_act_set;
		no_segments = t.no_segments;
		no_locmarkers = t.no_locmarkers;
		no_fcum_steps = t.no_fcum_steps;

		IntegrationStepSize = t.IntegrationStepSize;
		dlambdainv = t.dlambdainv;
		for (int32_t ix = 0; ix < 3; ix++) B0[ix] = t.B0[ix];
		for (int32_t ix = 0; ix < 3; ix++) g[ix] = t.g[ix];
		for (int32_t ix = 0; ix < 9; ix++) R0[ix] = t.R0[ix];
		for (int32_t ix = 0; ix < 3; ix++) p0[ix] = t.p0[ix];
		ContactMode = t.ContactMode;
		for (int32_t ix = 0; ix < 3; ix++) TipConstraintPoint[ix] = t.TipConstraintPoint[ix];
		for (int32_t ix = 0; ix < 3; ix++) TipForce[ix] = t.TipForce[ix];
		Li = t.Li;

		allocate_memory();
		for (int32_t ix = 0; ix < no_segments; ix++) SegmentTypes[ix] = t.SegmentTypes[ix];
		for (int32_t ix = 0; ix < no_segments; ix++) SegEndLambdas[ix] = t.SegEndLambdas[ix];
		for (int32_t ix = 0; ix < no_segments; ix++) rho[ix] = t.rho[ix];
		for (int32_t ix = 0; ix < no_flex_seg; ix++) for (int32_t jx = 0; jx < 9; jx++) K[ix][jx] = t.K[ix][jx];
		for (int32_t ix = 0; ix < no_flex_seg; ix++) for (int32_t jx = 0; jx < 9; jx++) Kinv[ix][jx] = t.Kinv[ix][jx];
		for (int32_t ix = 0; ix < no_flex_seg; ix++) for (int32_t jx = 0; jx < 3; jx++) ustar[ix][jx] = t.ustar[ix][jx];
		for (int32_t ix = 0; ix < no_act_set; ix++) ActMass[ix] = t.ActMass[ix];
		for (int32_t ix = 0; ix < no_act_set; ix++) for (int32_t jx = 0; jx < 3; jx++) MagMoment[ix][jx] = t.MagMoment[ix][jx];
		for (int32_t ix = 0; ix < no_act_set; ix++) for (int32_t jx = 0; jx < 9; jx++) CoilAlignmentTurnAreaMatrix[ix][jx] = t.CoilAlignmentTurnAreaMatrix[ix][jx];
		for (int32_t ix = 0; ix < no_locmarkers; ix++) LocMarkerLambdas[ix] = t.LocMarkerLambdas[ix];
		for (int32_t ix = 0; ix < no_fcum_steps + 1; ix++) for (int32_t jx = 0; jx < 3; jx++) fcumlambda[ix][jx] = t.fcumlambda[ix][jx];
	}

	CRMShootingMethodParams::~CRMShootingMethodParams() {
		if (memory_allocated) {
			delete[] SegmentTypes;
			delete[] SegEndLambdas;
			delete[] rho;
			delete[] K;
			delete[] Kinv;
			delete[] ustar;
			delete[] ActMass;
			delete[] MagMoment;
			delete[] CoilAlignmentTurnAreaMatrix;
			delete[] LocMarkerLambdas;
			delete[] fcumlambda;
		}
	}

	void CRMShootingMethodParams::allocate_memory() {
		SegmentTypes = new CatheterSegmentType[no_segments]();
		SegEndLambdas = new double[no_segments]();
		rho = new double[no_segments]();
		K = new double[no_flex_seg][9]();
		Kinv = new double[no_flex_seg][9]();
		ustar = new double[no_flex_seg][3]();
		ActMass = new double[no_act_set]();
		MagMoment = new double[no_act_set][3]();
		CoilAlignmentTurnAreaMatrix = new double[no_act_set][9]();
		LocMarkerLambdas = new double[no_locmarkers]();
		fcumlambda = new double[no_fcum_steps + 1][3]();
		memory_allocated = true;
	}




}
