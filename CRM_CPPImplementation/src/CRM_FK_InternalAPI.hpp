#pragma once

namespace CRMCatheterModel {

	// This is the free space model - called by CRM_ForwardKinematics()
	//    this function should not be called independently
	VectorXd CRM_ForwardKinematics_FreeSpace(const VectorXd in_x, CRMForwardKinematicsData Params, double& out_PotentialEnergy, int& out_localmin);

	// This is the contact model - called by CRM_ForwardKinematics()
	//    this function should not be called independently
	VectorXd CRM_ForwardKinematics_Contact(const VectorXd in_x, CRMForwardKinematicsData Params, double& out_PotentialEnergy, int& out_localmin);


	//
	// This is the API for the Forward Kinematics Jacobian for the Free Space Model - called by CRM_FKJacobian_Numerical()
	//    this function should not be called independently
	//       (note that this function returns the hybrid manipulator Jacobian + extras, not the Jacobian of the FK map)
	//
	// Enumerated type defining catheter contact mode.  
	enum class FKFreeJacobianType { ACTUATION_ONLY, ACTUATION_AND_TIP_FORCE };
	// ACTUATION_ONLY: Jacobian is calculated only w.r.t. actuation inputs
	// ACTUATION_AND_TIP_FORCE: Jacobian is calculated only w.r.t. actuation inputs and tip force
	MatrixXd CRM_FKJacobian_FreeSpace(
		const VectorXd in_x,	// Forward Kinematics input vector (see in_x of CRM_ForwardKinematics)
		VectorXd& out_y,		// Forward Kinematics output vector (see out_y of CRM_ForwardKinematics)
		CRMForwardKinematicsData in_Params,	// Container for parameter data to be sent to Forward Kinematics
		FKFreeJacobianType mode,			// flag to indicate if the Jacobian needs to be calculate w.r.t, actuation inputs only, or alos the tip force
		int& localmin);						// returns 0 if successful; !=0 if algorithms is stuck at a local minimum, or cannot make further progress 
											//		(returned for forward kinematics calculation at in_x)
		// output matrix rows are packed dp[0..2]; ws[0..2]; du0[0..2]
		// output matrix columns are packed (3 x NUM_ACT_SET + 1), actuation currents + inserted length; ( + 3 ), tip force (in spatial coordinates) - only for ACTUATION_AND_TIP_FORCE case
		//	the first 3+3=6 rows of the output matrix gives the Hybrid Manipulator Jacobian
		//          see Murray, Li, Sastry Intro to Robotics Book for the definition of the Manipulator Jacobian, and the Hybrid Manipulator Jacobian
		//              and how it is different from the Jacobian of the FK map
		//
		// if mode == FKFreeJacobianType::ACTUATION_ONLY
		// output matrix:
		//    (3 + 9 + 3) x (3 x NUM_ACT_SET + 1) 
		//		dp/di	:	\partial p / \partial in_x					:	partial derivatives of the tip position in spatial coordinates w.r.t. inputs
		//      ws		:	( ( \partial R / \partial in_x ) * R^T )v	:	term corresponding to the spatial angular velocity of the tip
		//		du0/di	:	\partial u0 / \partial in_x					:	partial derivative of curvature at the base of the catheter w.r.t. inputs
		// if mode == FKFreeJacobianType::ACTUATION_AND_TIP_FORCE
		// output matrix:
		//    (3 + 9 + 3) x (3 x NUM_ACT_SET + 1 + 3) 
		//		[	dp/di	dp/dftip
		//			ws/di	ws/dftip
		//			du0/di	du0/dftip   ]
		//


	//
	// these are internally called by TrustRegionDogleg within CRM_ForwardKinematics_Contact()
	//
	struct  CRMContactEquationParams : CRMForwardKinematicsData {
		double	Actuation[3 * NUM_ACT_SET + 1];
	};

	void CRM_Contact_Equation(double in_x[], double out_y[], CRMContactEquationParams Params);

	void CRM_Contact_Equation_AnalyticalJac(double in_x[], double out_y[], double out_fjac[], CRMContactEquationParams Params);

}
