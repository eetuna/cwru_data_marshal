#pragma once

namespace CRMCatheterModel {

	//
	// This is the API for the brute force numerical calculation of the Forward Kinematics Jacobian 
	//    i.e., the contact Jacobian in calculated by numerical differentiation of the contact FK model, 
	//       not by employing the psedoinverse of the Jacobian of the free space FK
	//    this function is provided for debugging purposes - should not be called independently as it is exremely slow
	//    it works for bothFREE_TIP and FIXED_TIP modes, but does not calculate jacobian with respect to ftip in FREE_TIP mode
	//      (note that this function returns the hybrid manipulator Jacobian + extras, not the Jacobian of the FK map)
	//
	MatrixXd CRM_FKJacobian_BruteForce(
		const VectorXd in_x,	// Forward Kinematics input vector (see in_x of CRM_ForwardKinematics above)
		VectorXd& out_y,		// Forward Kinematics output vector (see out_y of CRM_ForwardKinematics above)
		CRMForwardKinematicsData in_Params,	// Container for parameter data to be sent to Forward Kinematics
		int& localmin);						// returns 0 if successful; !=0 if algorithms is stuck at a local minimum, or cannot make further progress 
											//		(returned for forward kinematics calculation at in_x)
		// output matrix:
		//    (3+9+3) x (3 x NUM_ACT_SET + 1) matrix if Params.ContactMode == ContactModeType::FREE_TIP
		//    (3+9+3+3) x (3 x NUM_ACT_SET + 1) matrix if Params.ContactMode == ContactModeType::FIXED_TIP		
		//    outputs are packed dp/di[0..2]; ws[0..2]; du0/di[0..2] (; dftip/di[0..2])
		//		dpdi	:	\partial p / \partial in_x			 :	partial derivatives of the tip position in spatial coordinates w.r.t. inputs
		//      ws		:	( \partial R / \partial in_x ) * R^T :	term corresponding to the spatial angular velocity of the tip
		//		du0/di	:	\partial u0 / \partial in_x			 :	partial derivative of curvature at the base of the catheter w.r.t. inputs
		//		dftip/di:	\partial ftip / partial in_x		 :	partial derivative of the tip force in spatial coordinates w.r.t. inputs (if Params.ContactMode == ContactModeType::FIXED_TIP)
		//	  the first 3+3=6 rows of the output matrix gives the Hybrid Manipulator Jacobian
		//          see Murray, Li, Sastry Intro to Robotics Book for the definition of the Manipulator Jacobian, and the Hybrid Manipulator Jacobian
		//              and how it is different from the Jacobian of the FK map


	//
	// This function calculates the Forward Kinematics Jacobian by numerically taking derivatives of IVP
	//    this function is provided purely for debugging purposes - should not be used or called independently
	//
	MatrixXd CRM_FKJacobian_From_IVP_Numerical(const VectorXd in_x, const VectorXd& in_FKouty, CRMForwardKinematicsData in_Params);
	// helper function for CRM_FK_Jacobian_From_IVP_Numerical
	VectorXd CRMSolverIVPWrapper(VectorXd in, CRMShootingMethodParams in_Params);

}
