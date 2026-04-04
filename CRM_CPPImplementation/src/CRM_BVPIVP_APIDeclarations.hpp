#pragma once
#include <cmath>
#include "CRM_MatrixOperations.hpp"
#include "CRM_StateVector_Definitions.hpp"

//
// ---------------------------------------------------------
// This file contains the declarations for the internally used 
//    components and interfaces for the 
//    Cosserot Rod Model of the Catheter
// ---------------------------------------------------------
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

	// we will use c++ type traits to translate State Vector type to the corresponding State Derivative Vector type
	template <typename T> struct StDerivativeVect_trait { typedef void type; };
	template<> struct StDerivativeVect_trait<StateVector> { typedef StateDerivativeVector type; };
	template<> struct StDerivativeVect_trait<AugmentedStateVector<IVPJacobiansMini>> { typedef AugmentedStateDerivativeVector<IVPJacobiansMini> type; };
	template<> struct StDerivativeVect_trait<AugmentedStateVector<IVPJacobiansFull>> { typedef AugmentedStateDerivativeVector<IVPJacobiansFull> type; };
	template<typename T> using StDerivativeVectType = typename StDerivativeVect_trait<T>::type;

	// helper to be able to match state vector types
	template <class T> using expr_type = std::remove_cv_t<std::remove_reference_t<T>>;

	// Structure for passing parameters to the CRM Shooting Method Boundary Value Problem Solver CRMShootingMethodBVP
	class CRMShootingMethodParams {
	public:
		CRMShootingMethodParams(int32_t no_flex, int32_t no_rigid, int32_t no_act, int32_t no_loc, int32_t no_fcum);
		CRMShootingMethodParams(const CRMShootingMethodParams& t);	// perform deep copy of dynamic arrays - not just copy the pointers to the those arrays
		~CRMShootingMethodParams();

		int32_t no_flex_seg;						// Number of flexible segments
		int32_t no_rigid_seg;						// Number of rigid segments (including actuator segments and non-actuator rigid segments)
		int32_t no_act_set;							// Number of actuator sets
		int32_t no_segments;						// Total number of segments
		int32_t no_locmarkers;						// Number of localization markers
		int32_t no_fcum_steps;						// Number of steps used in calculating fcumlambda (cumulative forces); number of entries in fcumlambda is (no_fcum_steps+1)

		double	IntegrationStepSize;
		double 	dlambdainv;

		double 	B0[3];
		double	g[3];
		double	R0[9];
		double	p0[3];

		ContactModeType ContactMode;
		double	TipConstraintPoint[3];
		double	TipForce[3];

		double 	Li;

		//	For all parameters below, segments and actuator units are numbered/ordered from the tip of the catheter towards the base
		//  DISTAL TO PROXIMAL ORDERING
		CatheterSegmentType* SegmentTypes;						// [no_segments]
		double 	(*SegEndLambdas);								// [no_segments]
		double	(*rho);											// [no_segments]
		double 	(*K)[9];										// [no_flex_seg]
		double 	(*Kinv)[9];										// [no_flex_seg]
		double 	(*ustar)[3];									// [no_flex_seg]
		double	(*ActMass);										// [no_act_set]
		double 	(*MagMoment)[3];								// [no_act_set]
		double  (*CoilAlignmentTurnAreaMatrix)[9];				// [no_act_set]
		double	*LocMarkerLambdas;								// [no_locmarkers]
		double 	(*fcumlambda)[3];								// [no_fcum_steps+1]

	protected:
		void allocate_memory();
		bool memory_allocated = false;							// true if memory for the dynamic array were allocate by the class constructor
	};

	// Construct Shooting Method Parameter Set from Catheter Model and Configuration Parameters
	CRMShootingMethodParams CRMConstructShootingMethodParamSet(
		CRMCatheterModelParams CathParams, CatheterConfiguration CathConfig,
		double InsertionLength, double ActuationCurrents[NUM_ACT_SET][3],
		ContactModeType ContactMode,
		double TipConstraintPoint[3], double TipForce[3],
		double IntegrationStepSize);

	// Update the Shooting Method Parameter Set for a new set of Actuation Currents
	void CRMUpdateShootingMethodParamSetWithNewActuationCurrents(
		CRMShootingMethodParams& ShootingParams,
		double ActuationCurrents[NUM_ACT_SET][3]);


	//
	// ---------------------------------------------------------
	//        Cosserat Rod Model - Integrator for Solving the Initial Value Problem
	// ---------------------------------------------------------
	//

	// CRMSolverIVP API which uses CRMShootinMethodParams data structure
	//    note that in_ftip[] will be used, ignoring the value in_Params.TipForce[]
	void CRMSolverIVP(CRMShootingMethodParams in_Params,
		double in_deltau0[3], double in_ftip[3],
		bool in_CalculateEnergy,
		bool in_FinalValueOnly,
		double out_x_N[NUM_STATES], double out_MomentResidual[3],
		double& out_PotentialEnergy, 
		double out_p_atLocMarkers[/*in_Params.no_locmarkers*/][3],
		double out_p_atActuators[/*in_params.no_act_set*/][3],
		double out_R_atActuators[/*in_params.no_act_set*/][9]
	);

	// CRMSolverIVP API which exposes all of the individual parameters
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
	);

	// INPUT PARAMETERS
	//	In all input parameters, segments, markers, and actuator units etc are numbered/ordered from the tip of the catheter towards the base (distal to proximal)
	//int32_t	no_locmarkers;								// Number of localization markers
	//double 	x_0[NUM_STATES];							// Initial value of the state at the entry point of the catheter
	//														//  States are packed p[0..2],R[0..8],u[0..2]  (R: 3x3 matrix stored in row major order R11 R12 R13 R21 R22 R23 R31 R32 R33)
	//double 	Li;											// Inserted Length (length of the catheter from the entry point to the tip)
	//double 	dlambdainv;									// Reciprocal of \Delta \lambda (= \Delta s) used in discretizing fcum  // derived quantity ( dlambdainv = 1 / (Lf/NUM_FCUM) = NUM_FCUM/Lf ), Lf: functional length of the catheter
	//double 	SegEndLambdas[NUM_SEGMENTS];				// Array of lambda values for segment endpoints; NUM_SEGMENTS long array
	//double	LocMarkerLambdas[];							// Array of lambda values for localization markers; no_locmarkers long array
	//double 	K[NUM_FLEX_SEG][9];		  					// Catheter Rigidity Matrix; (NUM_FLEX_SEG)x9 long array, (NUM_FLEX_SEG) 3x3 matrices stored in row major order
															//		Actuator segments are assumed to be rigid
															//		In the future, we may want to just pass the parameters and construct the matrix inside
	//double 	Kinv[NUM_FLEX_SEG][9];  					// Inverses of K matrices; (NUM_FLEX_SEG)x9 long array, (NUM_FLEX_SEG) 3x3 matrices stored in row major order  // derived quantity
	// -UNUSED- double 	Kdot[NUM_FLEX_SEG][9]; 					// Derivative of K;  we assume Kdot=0.0
	//double 	ustar[NUM_FLEX_SEG][3];						// Local curvature in unloaded configuration for each of the flexible segments; (NUM_FLEX_SEG)x3 long array, (NUM_FLEX_SEG) 3x1 vectors
															//		Actuator segments are assumed to be straight
	// -UNUSED- double 	ustardot[NUM_FLEX_SEG][3];				// Derivative of local curvature in unloaded configuration; we assume ustardot=0.0 since our rest shape model is piecewise constant curvature
	//double 	MagMoment[NUM_ACT_SET][3];					// Actuator magnetization moments; NUM_ACT_SETx3 long array, NUM_ACT_SET 3x1 vectors; MagMoment = CoilAlignMat * CoilTurnAreaMat * ActuationCurrentVector
	//double 	fcumlambda[NUM_FCUM_LAMBDA+1][3];			// (NUM_FCUM_LAMBDA+1)x3 array (grouped by 3 doubles) storing cumulative external force (excluding tip force) integrated from \lambda = index * \Delta\lambda 
															//    to the catheter tip (\lambda=0) - in spatial (catheter base, i.e., entry port, frame) coordinates
															//    fcumlambda is parameterized by \lambda, distance from the tip, not s, distance from the entry point
															//    \lambda = Li - s
	//double	ftip[3];									// External point force (in spatial coordinates) applied at the tip of the catheter (\lambda = 0)
	//double 	B0[3];										//  B0 field vector of the MRI scanner (in spatial coordinates)
	//bool		CalculateEnergy;							// Flag used to indicate if catheter potential energy should be calculated (true) or not (false) 
	//bool  	FinalValueOnly;								// Flag used to indicate if only final value (xf) is returned (true) or if Marker Locations are returned as well (false)// OUTPUT VALUES
	//	All output values are reported as numbered/ordered from the tip of the catheter towards the base (distal to proximal)
	//double 	x_N[NUM_STATES];							// Final value of the state at the distal tip of the catheter
	//														//    States are packed u[0..2],R[0..8],p[0..2]  (R: 3x3 matrix stored in row major order R11 R12 R13 R21 R22 R23 R31 R32 R33)
	//double 	out_MomentResidual[3];						// Residual moment at the tip of the catheter, for use in boundary value problem solution
	//double 	p_atLocMarkers[][3];						// no_locmarkers x 3 array of positions of markers (ordered distal to proximal) at lambda coordinates given in LocMarkerLambdas  --- filled if FinalValueOnly is false
	//double	out_R_atActuators[][9];						// no_act_set x 9 array of actuations coil orientations (ordered distal to proximal)
															//		Each R gives the transformation from local catheter coil frame to the spatial frame (R_sc) and is a 3x3 matrix stored in row major order R11 R12 R13 R21 R22 R23 R31 R32 R33
															//      Note that only values for coils that are inside the chamber (distal to the entry point) are filled and valid


	//  Parameter Set used to call CRMSolverIVP_Core
	class CRMIVPCoreParams {
	public:
		CRMIVPCoreParams(int32_t no_flex, int32_t no_rigid, int32_t no_act, int32_t no_loc, int32_t no_fcums);
		CRMIVPCoreParams(const CRMIVPCoreParams& t);		// Perform a deep copy of dynamically allocated arrays - not just copy the pointers to those arrays
		~CRMIVPCoreParams();

		int32_t no_flex_seg;						// Number of flexible segments
		int32_t no_rigid_seg;						// Number of rigid segments (including actuator segments and non-actuator rigid segments)
		int32_t no_act_set;							// Number of actuator sets
		int32_t no_segments;						// Total number of segments
		int32_t no_locmarkers;						// Number of localization markers
		int32_t no_fcum_steps;						// Number of steps used in calculating fcumlambda (cumulative forces); number of entries in fcumlambda is (no_fcum_steps+1)

		//  These are the intermediate variables that will be used to call CRMSolverIVP_Core
		double xi[NUM_STATES];  					// Initial value of the state for the next segment to be integrated
													//  States are packed p[0..2],R[0..8],u[0..2]  (R: 3x3 matrix stored in row major order R11 R12 R13 R21 R22 R23 R31 R32 R33)
		double InsertedLength;						// Inserted Length (length of the catheter from the entry point to the tip)
		double dlambdainv; 							// reciprocal of dlambda (lambda stepsize used in discretizing fcumlambda)
		double B0[3];								// B0 field vector of the MRI scanner (in spatial coordinates)
		double g[3];								// gravity vector (in spatial coordinates)
		//	For all parameters below, segments and actuator units are numbered/ordered from the base of the catheter towards the tip (proximal to distal)
		//     Note that this ordering is the REVERSE of the ordering used in catheter specifications etc.
		CatheterSegmentType* SegmentTypes;			//[no_segments] List of Segment Types for each of the catheter segments
		int32_t *FlexActIndex;						//[no_segments] For each of the flexible and actuator segments, index of the corresponding entry in the related flex segment (e.g., K)  and actuation (e.g., MagMoment) arrays are stored here
		int32_t StartSegmentIndex;					// Index of the segment where the integration to solve IVP will start -- the segment located at the entry point; note that segment indices start at 0
		double *SegBounds;							//[no_segments + 1] The s values as each of the segment boundaries  (For no_segments segments, there are no_segments+1 boundaries)
		int32_t *SegSteps;							//[no_flex_seg] Number of integration steps in each flexible catheter segment
		double	*rho;								//[no_segments] Length Density (mass per unit length) of the flexible catheter substrate (tubing), excludes actuator mass but includes everything else
		double (*K)[9]; 							//[no_flex_seg] Catheter Rigidity Matrices;
		double (*Kinv)[9];							//[no_flex_seg] Inverses of K Matrices
		double (*ustar)[3];							//[no_flex_seg] Local curvature in unloaded configuration for each of the flexible segments
		double (*ActMass); 							//[no_act_set] Actuator masses (Na*1 long array)
		double (*MagMoment)[3]; 					//[no_act_set] Actuator magnetization moments in body coordinates; Na*3 long array, Na 3x1 vectors; MagMoment = CoilAlignMat * CoilTurnAreaMat * ActuationCurrentVector
		double (*CoilAlignmentTurnAreaMatrix)[9];	//[no_act_set] The product CoilAlignMat * CoilTurnAreaMat for each of the actuators; Na long array of 3x3 matrices (stored in row major order)
		double (*p_atActuators)[3];					//[no_act_set] no_act_set x 3 array for temporary storage of coil positions - this is dummy storage to avoid dynamic memory allocation during CRM_NLEquations evaluations, values stored here will not be used
		double (*R_atActuators)[9];					//[no_act_set] no_act_set x 9 array for temporary storage of coil orientations - this is dummy storage to avoid dynamic memory allocation during CRM_NLEquations evaluations, values stored here will not be used
		bool   CalculateEnergy;						// Falg used to indicate that the catheter potential energy (strain+magnetic+gravitational) should to be calculated
		bool   FinalValueOnly;						// Flag used to indicate if only final value (xf) is returned (true) or if Marker Locations are returned as well (false)
		int	   NextLocMarker;						// Next Localization Marker to be computed
		double *LocMarkers;							// no_locmarkers long array of the s values of each of the localization markers (markers not yet inserted into the catheter would have a negative s value
		double (*p_atLocMarkers)[3];				// no_locmarkers x 3 array for positions of markers (ordered proximal to distal)
													//   only the entries 0..NextLocMarker-1 are filled
		double (*fcumlambda)[3];					// 3*(no_fcum_steps+1) by 1 array (grouped by 3 doubles) storing cumulative external force (excluding tip force) integrated from \lambda = index * \Delta\lambda to the catheter tip (\lambda=0) - in spatial (catheter base frame) coordinates

	protected:
		void allocate_memory();
		bool memory_allocated = false;				// true if memory for the dynamic array were allocate by the class constructor
	};


	// Preparation of CRMIVPCoreParams for subsequent call to CRMSolverIVP_Core
	void CRMSolverIVP_Prep(
		int32_t in_no_flex_seg,
		int32_t in_no_rigid_seg,
		int32_t in_no_act_set,
		int32_t in_no_locmarkers,
		int32_t in_no_fcum_steps,
		double in_x_0[NUM_STATES], double in_IntegrationStepSize,
		double in_Li, double in_dlambdainv,
		const CatheterSegmentType in_SegmentTypes[/*no_segments=no_flex_seg+no_rigid_seg*/],
		double in_SegEndLambdas[/*no_segments=no_flex_seg+no_rigid_seg*/], double in_LocMarkerLambdas[/*no_locmarkers*/],
		double in_rho[/*in_no_flex_seg+in_no_rigid_seg*/],
		double in_K[/*in_no_flex_seg*/][9], double in_Kinv[/*in_no_flex_seg*/][9], double in_ustar[/*in_no_flex_seg*/][3],
		double in_ActMass[/*in_no_act_set*/],
		double in_CoilAlignmentTurnAreaMatrix[/*in_no_act_set*/][9],
		double in_MagMoment[/*in_no_act_set*/][3], double in_fcumlambda[/*in_no_fcum_steps + 1*/][3],
		double in_B0[3], double in_g[3],
		bool in_CalculateEnergy,
		bool in_FinalValueOnly, 
		CRMIVPCoreParams& out_CoreParams);


	// Core Computations used in CRMSolverIVP - Integrator for Solving the Initial Value Problem
	//    parameters are prepared using CRMSolverIVP_Prep
	//    CRMSolverIVP_Core can be called multiple times by changing only u[0..2] components of xi (in_u[]) and in_ftip[]
	//    for a single execution of CRMSolverIVP_Prep - the values in in_params should not be changed by user
	void CRMSolverIVP_Core(CRMIVPCoreParams& in_params,
		double in_deltau[3], double in_ftip[3],
		StateVector& out_x_N, double out_MomentResidual[3],
		double& out_PotentialEnergy, 
		double out_p_atLocMarkers[/*in_params.no_locmarkers*/][3],
		double out_p_atActuators[/*in_params.no_act_set*/][3],
		double out_R_atActuators[/*in_params.no_act_set*/][9]);


	// support function to Propagate Boundary Condition through a Rigid Link - used by CRMSolverIVP_Core
	void CRMSolverIVP_PropagateBCThroughRigidLink(StateVector& xi_ip1, double Residual_ip1[3],
		const double RigidSegmentLength, const double MagMoment[3], const double CoilAlignmentTurnAreaMatrix[9], const double B0[3],
		const double ustar_i[3], const double K_i[9], const double ustar_ip1[3], const double Kinv_ip1[9], 
		const StateVector& xf_i, const double Residual_i[3]);
	//StateVector xi_ip1;			//  Initial value (xi) of the state for the next segment (i+1) to be integrated
	//double Residual_ip1[3];		//  Residual at the end of the rigid link -- will be returned
	//double RigidSegmentLength;	//  Length of the Rigid Segment
	//double MagMoment[3]; 			//  Actuator magnetization moments in body coordinates for the actuator on the rigid link 
									//     MagMoment = CoilAlignMat * CoilTurnAreaMat * ActuationCurrentVector
	//double B0[3];					//  B0 field vector of the MRI scanner (in spatial coordinates)
	//StateVector xf_i;				//  Final value (xf) of the state for the last segment (i) integrated
	//double Residual_i[3];			//  Residual at the end of the last segment (i)


// support function for calculating location markers
	void CalculateLocMarkers(int& NextLocMarker, const StateVector& xnext,
		const double LocMarkers[/*no_locmarkers*/], double SegBounds_ip1,
		double p_atLocMarkers[/*no_locmarkers*/][3], int32_t no_locmarkers);
	//int& NextLocMarker; 							// next localization marker that needs to have an assigned value (updated value is returned)
	//double LocMarkers[/*no_locmarkers*/];			// The s values of each of the localization markers (markers not yet inserted into the catheter would have a negative s value
	//double SegBounds_ip1;							// The s value for the end of current segment = beginning of next (distal) segment boundary
	//StateVector xnext			; 					// State vector for the endpoint of current segment
	//double p_atLocMarkers[/*no_locmarkers*/][3]; 	// output: positions of markers (ordered proximal to distal)

// support function to copy location marker positions
	void LocMarkerUpdate(double p[3], const StateVector& xi, double t);

	void LocMarkerUpdate(double p[3], double xi[NUM_STATES], double t);


	//  Parameter Set used to call CRMIntegrand, Cosserat Rod Model Integrand
	struct CRMIntegrandParams {
		int32_t no_fcum_steps;						// Number of steps used in calculating fcumlambda (cumulative forces); number of entries in fcumlambda is (no_fcum_steps+1)
		double Li;									// Inserted length of the catheter (from s=0 to the tip)
		double dlambdainv;							// Reciprocal of \Delta \lambda (= \Delta s) used in discretizing fcum  ( dlambdainv = 1 / (Lf/NUM_FCUM_LAMBDA) = NUM_FCUM_LAMBDA/Lf ), Lf: functional (full) length of the catheter
		double* K;									// Catheter Rigidity Matrix at current s (3x3 matrix stored in row major order)
		double* Kinv;								// Inverse of K at current s (3x3 matrix stored in row major order)
	// -UNUSED- double *Kdot;   					// Derivative of K at current s (3x3 matrix stored in row major order);  we assume Kdot=0.0
		double* l;									// External moment at current s (world coordinates) (3x1 array)
		double* ustar;								// Local curvature in unloaded configuration at current s (3x1 array)
	// -UNUSED- double *ustardot;					// Derivative of local curvature in unloaded configuration at current s; we assume ustardot=0.0 since our rest shape model is piecewise constant curvature (3x1 array)
		double rho;									// Mass per length of the segment
		double(*fcumlambda)[3];						// (NUM_FCUM_LAMBDA+1)x3 array storing cumulative external force (exluding tip force) integrated from \lambda = index * \Delta\lambda to the catheter tip (\lambda=0)
		double* ftip;								// External point force (in spatial coordinates) applied at the tip of the catheter (\lambda = 0) (3x1 array)
		double* g;									// Gravity vector (in spatial coordinates) (3x1 array)
	};


	// Cosserat Rod Model Integrand
	void CRMIntegrand(double s, const StateVector& in_x, const CRMIntegrandParams in_Params,
		StateDerivativeVector& out_xdot);


	//
	// ---------------------------------------------------------
	//        Cosserat Rod Model - Boundary Value Problem Solver
	// ---------------------------------------------------------
	//



	// CRMShootingMethodBVP API which uses CRMShootinMethodParams data structure
	//    note that when in_ContactMode == ContactModeType::FIXED_TIP inParams.TipForce[] will not be used 
	//              when in_ContactMode == ContactModeType::FREE_TIP  in_ftip_initialguess[] will not be used, and out_ftip[] will be set to inParams.TipForce[]
	void CRMShootingMethodBVP(CRMShootingMethodParams in_Params,
		double in_deltau0_initialguess[3], double in_ftip_initialguess[3],
		double out_deltau0[3], double out_ftip[3], int& out_localmin);

	// CRMShootingMethodBVP API which exposes all of the individual parameters
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
	);

	// Preparation of NLEqnParams for subsequent call to Nonlinear Equation Solvers
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
		CRMCatheterModelParams& CathParams, CatheterConfiguration& CathConfig);

	// wrapper for equation to be solved -- needed for CRMNonlinearSolver
	struct NLEqnParams : CRMIVPCoreParams {
		NLEqnParams(int32_t no_flex, int32_t no_rigid, int32_t no_act, int32_t no_loc, int32_t no_fcums) : 
			CRMIVPCoreParams(no_flex, no_rigid, no_act, no_loc, no_fcums) {};		// input: number of localization markers
		ContactModeType ContactMode;			// Enumerated type defining catheter contact mode.  ContactMode == FREE_TIP if the catheter is not in contact with a surface, FIXED_TIP if catheter tip is constrained to be at TipContraintPoint
		double			TipConstraintPoint[3];	// The spatial coordinates of the point where the catheter tip is constrained to be (used if ContactMode == FIXED_TIP)
		double			TipForce[3];			// External point force (in spatial coordinates) applied at the tip of the catheter (\lambda = 0) (used if ContactMode == FREE_TIP)
	};

	void CRM_NLEquation(double in_x[], double out_y[], NLEqnParams Params);

	void CRM_NLEquation_AnalyticalJac(double in_x[], double out_y[], double out_fjac[], NLEqnParams Params);

	//
	// Numerical Integration Support Functions
	//

	// In place projection of the State Vector to the appropriate manifold
	//   this function is called by ABM4 after every iteration step
	void Project_State_to_Manifold(StateVector& State);

	//
	// Numerical Integration Functions
	//

	//ABM4: 4th order Adams-Bashforth Prediction and Adams-Moulton Correction Numerical Integration
	//    Note: The first three steps are calculated using RK2
	template <typename StVecType, typename ParamType>
	void ABM4(const StVecType& in_x_0, const double t_0, const int N, const double h,
		const ParamType in_Params, int32_t in_no_locmarkers,
		const bool FinalValueOnly, const double in_LocMarkers[/*in_no_locmarkers*/], int& inout_NextLocMarkerIdx,
		StVecType& out_x_N, double out_p_atLocMarkers[/*in_no_locmarkers*/][3]);

	//ABM4_step One step of 4th order Adams-Bashforth Prediction and Adams-Moulton Correction
	template <typename StVecType, typename ParamType>
	void ABM4_step(const StVecType& in_x_n, double t_n, double h,
		const StDerivativeVectType<StVecType>& in_xdot_nm1, const StDerivativeVectType<StVecType>& in_xdot_nm2, const StDerivativeVectType<StVecType>& in_xdot_nm3,
		const StVecType& in_x_nm1, const StVecType& in_x_nm2, const StVecType& in_x_nm3,
		const ParamType in_Params,
		StVecType& out_x_np1, StDerivativeVectType<StVecType>& out_xdot_n);

	//RK2_step One step of 2nd Order Runge-Kutta Integration
	template <typename StVecType, typename ParamType>
	void RK2_step(const StVecType& in_x_n, const double t_n, const double h,
		const ParamType in_Params,
		StVecType& out_x_np1, StDerivativeVectType<StVecType>& out_xdot_n);


	//
	// Robotic Kinematics Related Functions
	//

	// cross product operator
	// 1 dim array stored in row major order
	void wHat(const double in_w[3], double out_what[9]);
	// 1 dim array stored in row major order, stride: pointer increment between two consecutive entries 
	void wHat(const double in_w[3], double out_what[9], unsigned int stride);

	// cross product operator
	// extract the 3dim vector from the so(3) skew symmetric matrix stored in row major order
	void vee_from_so3(const double in_what[9], double out_w[3]);

	// Rodrigues' Formula for calculating expm(what*theta) for ||w||=1
	//    basic formula
	void RodriguesFormula(double in_w[3], double in_theta, double out_R[9]);
	//    expanded form of the formula - presumably more efficient
	void RodriguesExpanded(double in_w[3], double in_theta, double out_R[9]);

	// calculate R_np1 and p_np1 analytically using twist exponential, without numerical integration
	//   g_np1 = g_n * expm ( \hat{\xi}^b *h ),  where \xi^b= [ 0 0 1 u_n^T ]^T
	//   g = [R p; 0 0 0 1];
	void SE3_Analytical_Step(double in_R_n[9], double in_p_n[3], double in_u_n[3], double h, double out_R_np1[9], double out_p_np1[3]);


}
