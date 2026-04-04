#pragma once

#include <eigen5/Eigen/Dense>
using Eigen::MatrixXd;
using Eigen::VectorXd;
using Eigen::Vector3d;
using Eigen::Matrix3d;

namespace CRMCatheterModel {

#define ANALYTICAL_SE3_STEP

#define NUM_STATES 15   // u[0..2],R[0..9],p[0..2]
#ifndef NUM_ACT_SET
#define NUM_ACT_SET 2								// Number of actuator sets
#endif
	constexpr double FCUM_DLAMBDA = 1.0;		// Stepsize that will be used when calculating cumulative forces (fcumlambda): the actual stepsize will be CATHETER_LENGTH/ceil(CATHETER_LENGTH/FCUM_DLAMBDA)

// if defined, the actuation currents will be incrementally applied starting from proximal coil sets to distal coil sets to help with convergence
#define INCREMENTALLY_APPLY_CURRENTS

// Regularization scales used for Nonlinear Solver
	constexpr double IVALUE_SCALE_DU = 1.0;		//(1.0)			// the variable used in Nonlinear Solver is multiplied with this scale to calculate delta_u (delta curvature) that will be used in IVP
	constexpr double IVALUE_SCALE_F = 1.0;		//(100.0)		// the variable used in Nonlinear Solver is multiplied with this scale to calculate ftip (tip force) that will be used in IVP
	constexpr double RESIDUAL_SCALE_M = 1.0;	//(1.0)			// the residual for tip moment coming out of the IVP will be multiplied with this scale to return to the Nonlinear Solver
	constexpr double RESIDUAL_SCALE_P = 1.0;	//(0.01)		// the residual for tip position error coming out of the IVP will be multiplied with this scale to return to the Nonlinear Solver

	// NL Solver method selection
#define FK_TRUSTREGION								// Forward kinematics (inner loop - i.e., free space): Trust Region Method with the numerical jacobian
#define FK_TRUSTREGION_ANALYTICALJAC				//    Use the analytical jacobin in forward kinematics trustregion solution 
#define CONTACT_TRUSTREGION							// Forward kinematics (contact model): Trust Region Method with the numerical jacobian
#define CONTACT_TRUSTREGION_ANALYTICALJAC			//    Use the analytical jacobian in contact trustregion solution 
	constexpr double TRUSTREGION_TOLERANCE = 1e-5;		// 

	// Numerical Jacobian Calculation Stepsize - two orders of magnitude smaller than typical change scale
	constexpr double NUM_JACOBIAN_CURRENT_STEPSIZE = 1e-5;
	constexpr double NUM_JACOBIAN_INSERTIONLENGTH_STEPSIZE = 1e-2;
	constexpr double NUM_JACOBIAN_TIPFORCE_STEPSIZE = 1e-5;
	constexpr double NUM_JACOBIAN_BASEPOSITION_STEPSIZE = 1e-3;
	constexpr double NUM_JACOBIAN_BASEROTATION_STEPSIZE = 1e-3;
	constexpr double NUM_JACOBIAN_BASECURVATURE_STEPSIZE = 1e-6;

	// Enumerated type defining catheter segment types.  
	enum class CatheterSegmentType { FLEXIBLE, RIGID_WITH_ACTUATOR, RIGID };
	// CatheterSegmentType::FLEXIBLE			: Flexible Segment
	// CatheterSegmentType::RIGID_WITH_ACTUATOR	: Rigid segment with actuator (all actuators are assumed to be 3-axis)
	// CatheterSegmentType::RIGID				: Rigid Segment without an actuator 

	// Enumerated type defining catheter contact mode.  
	enum class ContactModeType { FREE_TIP, FIXED_TIP };
	// FREE_TIP : no contact, FIXED_TIP : catheter tip is constrained at a given point
	// Contact Mode: FREE_TIP ---  Dimension of NLEquation will be 3	// NLEquation - domain: u[0..2]; range: m_tip[0..2]  (moment at tip)
	// Contact Mode: FIXED_TIP --- Dimension of NLEquation will be 6	// NLEquation domain: u[0..2], ftip[0..2]; range: m_tip[0..2], delta_p[0..2] (tip position error)


	//
	// ---------------------------------------------------------
	//
	// Cosserat Rod Model Forward Kinematics
	//
	// ---------------------------------------------------------
	//

	// Structure for defining catheter model parameters
	class CRMCatheterModelParams {
	public:
		CRMCatheterModelParams(int32_t no_flex, int32_t no_rigid, int32_t no_act, int32_t no_loc);
		CRMCatheterModelParams(const CRMCatheterModelParams& t);	// Note that copy constructor does a deep copy - not just copy the pointers to those arrays
		~CRMCatheterModelParams();

		int32_t no_flex_seg;				// Number of flexible segments
		int32_t no_rigid_seg;				// Number of rigid segments (including actuator segments and non-actuator rigid segments)
		int32_t no_act_set;					// Number of actuator sets
		int32_t no_segments;				// Total number of segments
		int32_t no_locmarkers;				// Total number of localization markers

		//	For all parameters below, segments and actuator units are numbered/ordered from the tip of the catheter towards the base (distal to proximal)
		CatheterSegmentType* SegmentTypes;	// no_segments element array of Segment Types (of type CatheterSegmentType)
		double* SegLengths;					// [unit: mm]		no_segments element array of Segment Lengths
		double* rho;						// [unit: kg/mm]	no_segments element array of Length Density (mass per unit length) of the flexible catheter substrate (tubing)
		double* InnerRadius;				// [unit: mm]		no_flex_seg element array of Inner Radii of the flexible catheter segments
		double* OuterRadius;				// [unit: mm]		no_flex_seg element array of Outer Radii of the flexible catheter segments
		double* YoungsModulus;				// [unit: MPa]		no_flex_seg element array of Young's Moduli of the flexible catheter segments
		double* ShearModulus;				// [unit: MPa]		no_flex_seg element array of Shear Moduli of the flexible catheter segments
		double (*ustar)[3];					// [unit: rad/mm]	(no_flex_seg) x 3 array of local curvature in unloaded configuration for each of the flexible segments; (NUM_FLEX_SEG) 3x1 vectors
											//						Actuator segments are assumed to be straight
		double* ActMass;					// [unit: kg]		no_act_set element array of Actuator Segment Masses, does not include the flexible substrate
		double (*CoilAlignmentAngles)[2];	// [unit: rad]		no_act_set x 2 array of Coil Alignment Angles; for each actuator set, the angle for the first coil is relative to x axis, and the angle for the second coil is relative to y axis; (NUM_ACT_SET) 2x1 vectors
		double (*CoilTurnAreaMat)[9]; 		// [unit: 1000 mm^2]no_act_set x 9 array of Coil Turn Area matrices; (NUM_ACT_SET) 3x3 matrices, where each matrix is stored in row-major order  [20 turns x 10mm^2 = 200 mm^2 turns.area will be repoted as 0.2]
		double* LocMarkers;					// [unit: mm]		no_locmarkers element array of localization marker locations (in lambda coordinates, distances measured from the tip)

	protected:
		void allocate_memory();
		bool memory_allocated = false;						// true if memory for the dynamic array were allocate by the class constructor
	};

	// Structure for defining catheter configuration parameters
	struct CatheterConfiguration {
		double 	B0[3];		// [unit: Tesla]		B0 field vector of the MRI scanner (in spatial coordinates)
		double 	g[3];		// [unit: N/kg = m/s^2]	Gravity vector (in spatial coordinates)
		double 	p0[3];		// [unit: mm]			Catheter entry port position (in spatial coordinates)
		double 	R0[9];		// [unit: none]			Catheter orientation at the entry port (relative to the spatial frame); 3x3 matrix stored in row major order
	};

	// Container for parameter data to be sent to Forward Kinematics
	struct CRMForwardKinematicsData {
		CRMCatheterModelParams* CathParams;		// physical parameters of the catheter
		CatheterConfiguration* CathConfig;		// catheter configuration in space
		ContactModeType ContactMode;			// contact mode
		double	TipConstraintPoint[3];			// [unit: mm]		spatial coordinates of the point that the catheter tip is constrained to - only used if ContactMode == ContactModeType::FIXED_TIP
		double	TipForce[3];					// [unit: N]		force applied at the tip of the catheter in spatial coordinates - only used if ContactMode == ContactModeType::FREE_TIP
		double	deltau0_initialguess[3];		// [unit: rad/mm]	initial guess for the \delta curvature at the base of the catheter ( u0 = deltau0 + ustar0 )
		double	ftip_initialguess[3];			// [unit: N]		initial guess for the tip force in spatial coordinates - only used if ContactMode == ContactModeType::FIXED_TIP
		double	IntegrationStepSize;			// [unit: mm]		length stepsize used in numerical integration performed as part of IVP calculations (part of BVP)
		bool	FinalValueOnly;					// Flag used to indicate if only final value is returned (true) or if Marker Locations are returned as well (false)
		double  (*ReportedMarkerPos)[3];		// [unit: mm]		pointer to an output array where the calculated marker locations will be stored
		double	(*ReportedCoilPos)[3];			// [unit: mm]		pointer to an output array where the calculated coil positions will be stored (center point of the coil)
		double  (*ReportedCoilOrient)[9];		// [unit: none]		pointer to an output array where the calculated coil orientations will be stored
	};

	// Load Catheter Model Parameters, which provides the Physical Description of the Catheter, from file
	//
	// IMPORTANT NOTE: For now, most proximal segment is assumed to be always flexible
	//    and the flexible and rigid segments are assumed to be alternating
	//    most distal segment can be flexible or rigid
	// Items are listed in distal-to-proximal order (starting from the tip of the catheter towards the base)
	//
	CRMCatheterModelParams Load_CRMCatheterModelParams(const char* path_to_input);
	// throws a std::runtime_error if there is an error

	// Load Catheter Configuration Parameters, which specifies Catheter Configuration in spatial coordinates, from file
	//
	//   B0 field vector of the MRI scanner (in spatial coordinates) - unit: Tesla
	//   Gravity vector - unit: N/kg = kg m/s^2
	//   Catheter entry point coordinates (in spatial coordinates) - unit: mm
	//   Catheter entry point orientation (3x3 rotation matrix describing catheter entry point frame orientation relative to the spatial frame stored in row major order) - unit: unitless
	//
	CatheterConfiguration Load_CatheterConfiguration(const char* path_to_input);
	// throws a std::runtime_error if there is an error

	//useful macros
#define MAX(a,b) 	( ((b)>(a))?(b):(a) )
#define MIN(a,b) 	( ((b)<(a))?(b):(a) )
#define POW4(a)		( (a)*(a)*(a)*(a) )


//
// Cosserat Rod Model Forward Kinematics API
//

// This is the C++ array version of the main API of the Cosserat Rod Model Forward Kinematics 
//		This is a wrapper for a sequence of CRMShootingMethodBVP + CRMSolverIVP calls
//      This is a wrapper, which calls the Eigen vectorized version of the CRM_ForwardKinematics()
	int CRM_ForwardKinematics(   // return value ==0 successful; !=0 if algorithms is stuck at a local minimum, or cannot make further progress
		double in_x[],		// array of length (3 x NUM_ACT_SET + 1)  -- the +1 is for the inserted length of the catheter
							// Actuation currents for each of the actuation coil currents [NUM_ACT_SET][3] stored in row-major order 
							//    followed by the Inserted Length (length of the catheter from the entry point to the tip)
							//    actuator units are numbered/ordered from the tip of the catheter towards the base (distal to proximal)
							//  [unit: A]	Currents are in A (Ampere)
							//  [unit: mm]	Insertion length is in mm
		double out_y[],		// output vector
							//    (3+9+3)x1 vector if Params.ContactMode == ContactModeType::FREE_TIP
							//    (3+9+3+3)x1 vector if Params.ContactMode == ContactModeType::FIXED_TIP		
							//    outputs are packed p[0..2],R[0..8],deltau0[0..2](,ftip[0..2])
							//		p:			[unit: mm]		tip position in spatial coordinates
							//      R:			[unit: none]	tip orientation matrix --- 3x3 matrix stored in row major order (R11 R12 R13 R21 R22 R23 R31 R32 R33)
							//		deltau0:	[unit: rad/mm]	delta curvature at the base of the catheter ( u0 = deltau0 + ustar0 )
							//		ftip:		[unit: N]		tip force in spatial coordinates
		double& out_PotentialEnergy,		//	[unit: mJ]		The calculated potential energy (strain + magnetic + gravitational) of the catheter
		CRMForwardKinematicsData Params		// Container for parameter data to be sent to Forward Kinematics
	);


	// This is the Eigen vectorized version of the main API of the Cosserat Rod Forward Kinematics 
	//		This is a wrapper for a sequence of CRMShootingMethodBVP + CRMSolverIVP calls
	VectorXd CRM_ForwardKinematics(const VectorXd in_x, CRMForwardKinematicsData Params, double& out_PotentialEnergy, int& out_localmin);
	//						in_x			input vector (vector version of in_x --- see above)
	//						Params			Container for parameter data to be sent to Forward Kinematics
	//						out_PotentialEnergy		The calculated potential energy (strain + magnetic + gravitational) of the catheter
	//						out_localmin	returns 0 if successful; !=0 if algorithms is stuck at a local minimum, or cannot make further progress
	//						return			output vector (vector version of out_y --- see above)

	//
	// This is the API for the analytical calculation of the Forward Kinematics Jacobian 
	//		( uses CRMSolverIVPJacobian )
	//      (note that this function returns the hybrid manipulator Jacobian + extras, not the Jacobian of the FK map)
	//
	MatrixXd CRM_FKJacobian_Analytical(
		const VectorXd in_x,				// Forward Kinematics input vector (see in_x of CRM_ForwardKinematics above)
		const VectorXd& in_FKouty,			// Forward Kinematics output vector, corresponding to an equilibrium configuration of the catheter, passed as input (see out_y of CRM_ForwardKinematics above)
		CRMForwardKinematicsData in_Params);	// Container of the parameter data sent to Forward Kinematics
		// output matrix:
		//  if Params.ContactMode == ContactModeType::FREE_TIP
		//    output matrix is (3+3) x (3 x NUM_ACT_SET + 1 +3), packed dp/dz, dp/dft; ws_dz, ws_dft 
		//		dpdz	:	\partial p / \partial in_x			 :	partial derivatives of the tip position in spatial coordinates w.r.t. inputs (should be ~0 for ContactModeType::FIXED_TIP)
		//		dpdft	:	\partial p / \partial f_tip			 :	partial derivatives of the tip position in spatial coordinates w.r.t. tip force (should be ~0 for ContactModeType::FIXED_TIP)
		//      ws_dz	:	( \partial R / \partial in_x ) * R^T :	term corresponding to the spatial angular velocity of the tip
		//      ws_dft	:	( \partial R / \partial f_tip ) * R^T:	term corresponding to the spatial angular velocity of the tip
		//	  the first 3+3=6 rows of the output matrix gives the Hybrid Manipulator Jacobian
		//          see Murray, Li, Sastry Intro to Robotics Book for the definition of the Manipulator Jacobian, and the Hybrid Manipulator Jacobian
		//              and how it is different from the Jacobian of the FK map
		//  if Params.ContactMode == ContactModeType::FIXED_TIP		
		//    output matrix is (3) x (3 x NUM_ACT_SET + 1) matrix packed dftip/dz[0..2]
		//		dftip/dz:	\partial ftip / partial in_x		 :	partial derivative of the tip force in spatial coordinates w.r.t. inputs
		//

	//
	// This is the API for the numerical (finite difference) calculation of the Forward Kinematics Jacobian 
	//      (note that this function returns the hybrid manipulator Jacobian + extras, not the Jacobian of the FK map)
	//
	MatrixXd CRM_FKJacobian_Numerical(
		const VectorXd in_x,	// Forward Kinematics input vector (see in_x of CRM_ForwardKinematics above)
		VectorXd& out_y,		// Forward Kinematics output vector (see out_y of CRM_ForwardKinematics above)
		CRMForwardKinematicsData in_Params,	// Container for parameter data to be sent to Forward Kinematics
		int& localmin);						// returns 0 if successful; !=0 if algorithms is stuck at a local minimum, or cannot make further progress 
											//		(returned for forward kinematics calculation at in_x)
		// output matrix:
		//  if Params.ContactMode == ContactModeType::FREE_TIP
		//    output matrix is (3+9+3) x (3 x NUM_ACT_SET + 1), packed dp/di[0..2]; ws[0..2]; du0/di[0..2] 
		//		dpdz	:	\partial p / \partial in_x			 :	partial derivatives of the tip position in spatial coordinates w.r.t. inputs (should be ~0 for ContactModeType::FIXED_TIP)
		//      ws		:	( \partial R / \partial in_x ) * R^T :	term corresponding to the spatial angular velocity of the tip
		//		du0/dz	:	\partial u0 / \partial in_x			 :	partial derivative of curvature at the base of the catheter w.r.t. inputs (note that since ustar is constant \partial u0 == \partial deltau0)
		//	  the first 3+3=6 rows of the output matrix gives the Hybrid Manipulator Jacobian
		//          see Murray, Li, Sastry Intro to Robotics Book for the definition of the Manipulator Jacobian, and the Hybrid Manipulator Jacobian
		//              and how it is different from the Jacobian of the FK map
		//  if Params.ContactMode == ContactModeType::FIXED_TIP		
		//    output matrix is (3) x (3 x NUM_ACT_SET + 1) matrix packed dftip/di[0..2]
		//		dftip/dz:	\partial ftip / partial in_x		 :	partial derivative of the tip force in spatial coordinates w.r.t. inputs
		//


	//
	// ---------------------------------------------------------
	//
	// Cosserat Rod Model Catheter Class Declaration
	//
	// ---------------------------------------------------------
	//

	//
	//
	// This is the CRM_Catheter Class which encapsulates the catheter kinematics calculations and the related data 
	//
	//
	class CRM_Catheter {
	public:
		CRM_Catheter(CRMCatheterModelParams Params, CatheterConfiguration Config);
		~CRM_Catheter();

		std::tuple < Vector3d, Matrix3d, Vector3d > ForwardKinematicsFree(const VectorXd ActuationVector, const bool CalculateMarkers, double& out_PotentialEnergy, int& out_localmin);
		//  Calculates CRM forward kinematics for free tip
		//      this is the incremental version - uses curvature at the catheter base (u0) from the last forward kinematics calculation which converged (localmin==0)
		//	    as the initial guess for u0
		//      TipForce is assumed to be zero
		//  inputs:
		//		ActuationVector:	see in_x[] in CRM_ForwardKinematics above
		//      CalculateMarkers:	Flag used to indicate if marker locations (in array MarkerPos[][3]) should be updated (true) or not (false)
		//  outputs:
		//		ptip:			tip position in spatial coordinates --- 3x1 vector
		//      Rtip:			tip orientation matrix --- 3x3 matrix
		//		deltau0:		delta curvature at the base of the catheter ( u0 = deltau0 + ustar0 )
		//		out_PotentialEnergy:	the calculated potential energy (strain + magnetic + gravitational) of the catheter
		//      out_localmin:	returns 0 if successful; !=0 if algorithms is stuck at a local minimum, or cannot make further progress

		std::tuple < Vector3d, Matrix3d, Vector3d > ForwardKinematicsFree(const VectorXd ActuationVector, const Vector3d TipForce, const Vector3d deltau0_initialguess, bool CalculateMarkers, double& out_PotentialEnergy, int& out_localmin);
		//  Calculates CRM forward kinematics for free tip
		//      in this version, initial guess for delta_curvature at the catheter base (deltau0 = u0 - ustar0) is provided as input, and a non-zero TipForce can be specified
		//  see inputs and outputs for ForwardKinematicsFree() above
		//  additional inputs:
		//      TipForce:				force applied at the tip of the catheter in spatial coordinates
		//      delta u0_initialguess:	initial guess for the delta_curvature at the catheter base ( u0 = deltau0 + ustar0 )

		std::tuple < Vector3d, Matrix3d, Vector3d, Vector3d > ForwardKinematicsContact(const VectorXd ActuationVector, const Vector3d TipConstraintPoint, const bool CalculateMarkers, double& out_PotentialEnergy, int& out_localmin);
		//  Calculates CRM forward kinematics for constrained tip
		//      this is the incremental version - uses delta_curvature at the catheter base (delta_u0) and contact force at catheter tip (ftip) from 
		//		the last forward kinematics calculation which converged (localmin==0) as the initial guesses for u0 and ftip
		//  inputs:
		//		ActuationVector:	see in_x[] in CRM_ForwardKinematics above
		//		TipConstraintPoint:	spatial coordinates of the point that the catheter tip is constrained to
		//      CalculateMarkers:	Flag used to indicate if marker locations (in array MarkerPos[][3]) should be updated (true) or not (false)
		//  outputs:
		//		ptip:			tip position in spatial coordinates --- 3x1 vector
		//      Rtip:			tip orientation matrix --- 3x3 matrix
		//		deltau0:		delta curvature at the base of the catheter ( u0 = deltau0 + ustar0 )
		//		ftip:			tip force in spatial coordinates
		//		out_PotentialEnergy:	the calculated potential energy (strain + magnetic + gravitational) of the catheter
		//      out_localmin:	returns 0 if successful; !=0 if algorithms is stuck at a local minimum, or cannot make further progress

		std::tuple < Vector3d, Matrix3d, Vector3d, Vector3d > ForwardKinematicsContact(const VectorXd ActuationVector, const Vector3d TipConstraintPoint, const Vector3d deltau0_initialguess, const Vector3d ftip_initialguess, const bool CalculateMarkers, double& out_PotentialEnergy, int& out_localmin);
		//  Calculates CRM forward kinematics for constrained tip
		//      in this version, initial guesses for delta_curvature at the catheter base (delta_u0), and the contact force at the catheter tip (ftip) 
		//		are provided as input
		//  see inputs and outputs for ForwardKinematicsContact() above
		//  additional inputs:
		//      deltau0_initialguess:	initial guess for the delta_curvature at the catheter base ( u0 = deltau0 + ustar0 )
		//      ftip_initialguess:		initial guess for the tip force in spatial coordinates

		MatrixXd FKJacobian_Analytical();
		//  Analytical calculation of the Forward Kinematics Jacobian 
		//  uses the actuation vector and results of the last Forward Kinematics Calculation
		//     behavior is undefined if this function is called before calculation of ForwardKinematics (Free or Contact) or 
		//	   if any of the parameters is changed since the calculation of the ForwardKinematicsFree or ForwardKinematicsContact   
		// output matrix:
		//  if last FK calculation was ForwardKinematicsFree
		//    output matrix is (3+9+3) x (3 x NUM_ACT_SET + 1 +3), packed dp/dz, dp/dft; ws_dz, ws_dft 
		//		dpdz	:	\partial p / \partial in_x			 :	partial derivatives of the tip position in spatial coordinates w.r.t. inputs (should be ~0 for ContactModeType::FIXED_TIP)
		//		dpdft	:	\partial p / \partial f_tip			 :	partial derivatives of the tip position in spatial coordinates w.r.t. tip force (should be ~0 for ContactModeType::FIXED_TIP)
		//      ws_dz	:	( \partial R / \partial in_x ) * R^T :	term corresponding to the spatial angular velocity of the tip
		//      ws_dft	:	( \partial R / \partial f_tip ) * R^T:	term corresponding to the spatial angular velocity of the tip
		//	  the first 3+3=6 rows of the output matrix gives the Hybrid Manipulator Jacobian
		//          see Murray, Li, Sastry Intro to Robotics Book for the definition of the Manipulator Jacobian, and the Hybrid Manipulator Jacobian
		//              and how it is different from the Jacobian of the FK map
		//  if last FK calculation was ForwardKinematicsContact		
		//    output matrix is (3) x (3 x NUM_ACT_SET + 1) matrix packed dftip/dz[0..2]
		//		dftip/dz:	\partial ftip / partial in_x		 :	partial derivative of the tip force in spatial coordinates w.r.t. inputs
		//      

		void ResetInitialGuesses() { last_deltau0.setZero(); last_ftip.setZero(); }
		// Assigns 0 to last_u0 and last_ftip vectors;

		CRMCatheterModelParams CathParams;				// Catheter physical parameters will be packaged into this structure
		CatheterConfiguration CathConfig;				// Catheter spatial configuration parameters will be packages into this structure
		double& IntegrationStepSize = FKParams.IntegrationStepSize;	// Stepsize used in numerical integration along the length of the catheter during IVP - unit: mm
		double (*MarkerPos)[3];							// Storage for storing localization marker positions
		double (*CoilPos)[3];							// Storage for sotring actuation coil positions
		double (*CoilOrient)[9];						// Storage for sotring actuation coil orientations

	protected:
		VectorXd last_ActuationVector;		// actuation vector from last FK solution
		VectorXd last_FKsolution;			// output vector from the last FK solution
											//    this will be used in the Jacobian calculation
		Vector3d last_deltau0;				// delta curvature at the catheter base from the last forward kinematics calculation which converged ( u0 = deltau0 + ustar0 )
											//    this will be used as the initial guess for incremental FK calculation
		Vector3d last_ftip;					// contstraint force at the catheter tip from the last forward kinematics calculation which converged
											//    this will be used as the initial guess for incremental FK caculation 
											//    when ContactMode == ContactModeType::FIXED_TIP
		CRMForwardKinematicsData FKParams;	// Container for parameter data to be sent to Forward Kinematics

	private:

	};

}