#include "CRM.hpp"
#include "CRM_BVPIVP_APIDeclarations.hpp"
#include "CRM_IVPJacobian.hpp"
#include "CRM_FK_InternalAPI.hpp"
#include "minpack.hpp"

using Eigen::Matrix;

namespace CRMCatheterModel {


	int CRM_ForwardKinematics(double in_x[], double out_y[], double& out_PotentialEnergy, CRMForwardKinematicsData Params) {

		int localmin;
		int X_Dim = NUM_ACT_SET * 3 + 1;	// Dimension of the Input 3 x NUM_ACT_SET + 1 insertion
		int Y_Dim;						// Dimension of the Output
		if (Params.ContactMode == ContactModeType::FREE_TIP) {
			Y_Dim = 3 + 9 + 3;					// tip position + R + u_0
		}
		else { // FIXED_TIP
			Y_Dim = 3 + 9 + 3 + 3;				//   ... + tip force
		}
		VectorXd x(X_Dim), y(Y_Dim);

		for (int i = 0; i < X_Dim; i++) x(i) = in_x[i];

		y = CRM_ForwardKinematics(x, Params, out_PotentialEnergy, localmin);

		for (int i = 0; i < Y_Dim; i++) out_y[i] = y(i);

		return localmin;
	}


	VectorXd CRM_ForwardKinematics(const VectorXd in_x, CRMForwardKinematicsData Params, double& out_PotentialEnergy, int& out_localmin) {

		if (Params.ContactMode == ContactModeType::FREE_TIP)
			return CRM_ForwardKinematics_FreeSpace(in_x, Params, out_PotentialEnergy, out_localmin);
		else // (Params.ContactMode == ContactModeType::FIXED_TIP)
			return CRM_ForwardKinematics_Contact(in_x, Params, out_PotentialEnergy, out_localmin);

	}


	VectorXd CRM_ForwardKinematics_FreeSpace(const VectorXd in_x, CRMForwardKinematicsData Params, double& out_PotentialEnergy, int& out_localmin) {

		int X_Dim = NUM_ACT_SET * 3 + 1;	// Dimension of the Input 3 x NUM_ACT_SET + 1 for inserted length
		int Y_Dim;							// Dimension of the Output
		if (Params.ContactMode == ContactModeType::FREE_TIP) {
			Y_Dim = 3 + 9 + 3;					// tip position + R + u_0
		}
		else { // FIXED_TIP
			Y_Dim = 3 + 9 + 3 + 3;				//   ... + tip force
		}
		VectorXd out_y(Y_Dim);

		double ActuationCurrents[NUM_ACT_SET][3];
		double InsertedLength;
        double deltau0_calc[3];
        double deltau0_initial[3];
		double ftip_calc[3];
		double xf[NUM_STATES];
		double residual[3];
		auto& CathParams = *(Params.CathParams);
		auto& CathConfig = *(Params.CathConfig);

		InsertedLength = in_x(NUM_ACT_SET * 3);
#ifdef INCREMENTALLY_APPLY_CURRENTS
		for (int i = 0; i < NUM_ACT_SET; i++)	for (int j = 0; j < 3; j++)	ActuationCurrents[i][j] = 0.0;  // we are strating with zero and incrementally add currents below

		CRMShootingMethodParams BVPParams = 
			CRMConstructShootingMethodParamSet(CathParams, CathConfig, InsertedLength, ActuationCurrents, Params.ContactMode, Params.TipConstraintPoint, Params.TipForce, Params.IntegrationStepSize);

		for (int j = 0; j < 3; j++) deltau0_initial[j] = Params.deltau0_initialguess[j];

		// we will apply actuation currents incrementally adding one coil set at a time starting from proximal end to the distal end
		for (int i = (NUM_ACT_SET - 1); i >= 0; i--) {
			for (int j = 0; j < 3; j++)	ActuationCurrents[i][j] = in_x(i * 3 + j);
			CRMUpdateShootingMethodParamSetWithNewActuationCurrents(BVPParams, ActuationCurrents);
			CRMShootingMethodBVP(BVPParams, deltau0_initial, Params.ftip_initialguess, deltau0_calc, ftip_calc, out_localmin);
			for (int j = 0; j < 3; j++) deltau0_initial[j] = deltau0_calc[j];
		}
#else
		for (int i = 0; i < NUM_ACT_SET; i++)	for (int j = 0; j < 3; j++)	ActuationCurrents[i][j] = in_x(i * 3 + j);  // apply all of the currents

		CRMShootingMethodParams BVPParams =
			CRMConstructShootingMethodParamSet(CathParams, CathConfig, InsertedLength, ActuationCurrents, Params.ContactMode, Params.TipConstraintPoint, Params.TipForce, Params.IntegrationStepSize);

		CRMShootingMethodBVP(BVPParams, Params.deltau0_initialguess, Params.ftip_initialguess, deltau0_calc, ftip_calc, out_localmin);
#endif

		// we want to calculate the potential energy of the catheter to return
		bool CalculateEnergy = true;

		// Cosserat Rod Model - Solve the Initial Value Problem to calculate the shape of the catheter 
		CRMSolverIVP(BVPParams, deltau0_calc, ftip_calc, CalculateEnergy, Params.FinalValueOnly, xf, residual, out_PotentialEnergy, Params.ReportedMarkerPos, Params.ReportedCoilPos, Params.ReportedCoilOrient);

		for (int i = 0; i < 3; i++) {
			out_y(i) = xf[i];						// p
			out_y(3 + 9 + i) = deltau0_calc[i];		// deltau0
			if (Params.ContactMode == ContactModeType::FIXED_TIP) out_y(3 + 9 + 3 + i) = ftip_calc[i];
		}
		for (int i = 0; i < 9; i++) {
			out_y(3 + i) = xf[3 + i];				// R
		}

		return (out_y);
	}


	VectorXd CRM_ForwardKinematics_Contact(const VectorXd in_x, CRMForwardKinematicsData Params, double& out_PotentialEnergy, int& out_localmin) {

		int X_Dim = NUM_ACT_SET * 3 + 1;	// Dimension of the Input 3 x NUM_ACT_SET + 1 insertion
		int Y_Dim = 3 + 9 + 3 + 3;			// Dimension of the Output: // tip position + R + deltau_0 + tip force
		VectorXd out_y(Y_Dim);

		//
		// We will solve the nonlinear equations constraining the tip to the given tip position by varying the tip force
		//

		CRMContactEquationParams NLEParams;
		NLEParams.CRMForwardKinematicsData::operator= (Params);				// CRMForwardKinematicsData portion of NLEParams comes from Params
		for (int i = 0; i < X_Dim; i++) NLEParams.Actuation[i] = in_x(i);	// Actutation part of the NLEParams comes from in_x
		NLEParams.ContactMode = ContactModeType::FREE_TIP;					// We will use this to solve FREE_TIP model

		const double FSCALE_INV = 1.0 / IVALUE_SCALE_F;
		double initialguessscaled[3];
		double returnedparamscaled[3];
		for (int i = 0; i < 3; i++) initialguessscaled[i] = FSCALE_INV * Params.ftip_initialguess[i];

#if defined( CONTACT_TRUSTREGION )
		double x[3]; // we will create a new variable here and not use initialguessscaled since truss-region-dogleg algorithm uses the same variable for both input and output
		double residual[3];
		int info;
		double tol = TRUSTREGION_TOLERANCE;
		for (int i = 0; i < 3; i++) x[i] = initialguessscaled[i];

#if defined  (CONTACT_TRUSTREGION_ANALYTICALJAC)
		TrustRegionDogleg_GivenJacobian(CRM_Contact_Equation, CRM_Contact_Equation_AnalyticalJac, 3, x, residual, tol, info, NLEParams);
#else 	
		TrustRegionDogleg(CRM_Contact_Equation, 3, x, residual, tol, info, NLEParams);
#endif

		for (int i = 0; i < 3; i++) returnedparamscaled[i] = x[i];
		out_localmin = (info == 1) ? 0 : (info - 1);
#else // undefined
		exit(1);
#endif

		//
		// Once we know the contact force, we can run the FK again in FREE_TIP mode using the contact force as the tip force
		//
		int ignore;  // we are returning localmin that is given by the original NL equations solution
		VectorXd temp_out;
		for (int i = 0; i < 3; i++) NLEParams.TipForce[i] = IVALUE_SCALE_F * returnedparamscaled[i];
		temp_out = CRM_ForwardKinematics(in_x, NLEParams, out_PotentialEnergy, ignore);
		out_y.head(15) = temp_out;
		for (int i = 0; i < 3; i++) out_y(15 + i) = IVALUE_SCALE_F * returnedparamscaled[i];  // tip force for output comes from the solution above

		return out_y;
	}


	MatrixXd CRM_FKJacobian_Numerical(const VectorXd in_x, VectorXd& out_y, CRMForwardKinematicsData in_Params, int& localmin) {

		double dummyPE;
		if (in_Params.ContactMode == ContactModeType::FREE_TIP) {
			return (CRM_FKJacobian_FreeSpace(in_x, out_y, in_Params, FKFreeJacobianType::ACTUATION_ONLY, localmin));
		}
		// else ContactModeType::FIXED_TIP
		CRMForwardKinematicsData Params = in_Params;					// create a local copy
		Params.FinalValueOnly = true;
		out_y = CRM_ForwardKinematics(in_x, Params, dummyPE, localmin);	// calculate forward kinematics at x_0 for contact model
		for (int i = 0; i < 3; i++) Params.TipForce[i] = out_y(3 + 9 + 3 + i);  // we need to use the calculated contact force as the tip force for free-space model
		int templocalmin;  // dummy variable
		VectorXd tempvector;  // dummy variable
		MatrixXd JBVP = CRM_FKJacobian_FreeSpace(in_x, tempvector, Params, FKFreeJacobianType::ACTUATION_AND_TIP_FORCE, templocalmin);
		MatrixXd JBVPp = JBVP.topRows(3);
		MatrixXd JBVPp_z = JBVPp.leftCols(JBVPp.cols() - 3);
		MatrixXd JBVPp_ftip = JBVPp.rightCols(3);
		MatrixXd J = -JBVPp_ftip.completeOrthogonalDecomposition().pseudoInverse() * JBVPp_z;  // Is this the best option in Eigen ???
		return J;
	}


	MatrixXd CRM_FKJacobian_FreeSpace(const VectorXd in_x, VectorXd& out_y, CRMForwardKinematicsData in_Params, FKFreeJacobianType mode, int& localmin) {

		double dummyPE;
		VectorXd x_0 = in_x;									// create a local copy
		CRMForwardKinematicsData Params = in_Params;					// create a local copy
		Params.FinalValueOnly = true;										// we don't need marker locations
		Params.ContactMode = ContactModeType::FREE_TIP;						// we will use Free Space model

		VectorXd f_0 = CRM_ForwardKinematics(x_0, Params, dummyPE, localmin);	// calculate forward kinematics at x_0

		double h = NUM_JACOBIAN_CURRENT_STEPSIZE;
		double hinv = 1 / NUM_JACOBIAN_CURRENT_STEPSIZE;
		auto f_size = f_0.size();
		auto var_size = x_0.size();
		auto col_size = (mode == FKFreeJacobianType::ACTUATION_ONLY) ? var_size : (var_size + 3);   // we need extra 3 columns for f_tip
		MatrixXd Df_(f_size, col_size);
		VectorXd x_h(var_size), f_h(f_size);
		int lmin;

		// we will use the deltau0 calculated for FK at x_0 as the initial guess for all of the subsequent FK calculations
		for (int i = 0; i < 3; i++) {
			Params.deltau0_initialguess[i] = f_0(12 + i);
		}
		f_0 = CRM_ForwardKinematics(x_0, Params, dummyPE, localmin);	// calculate forward kinematics at x_0 with the new initial conditions for numerical stability

		for (int j = 0; j < var_size; ++j) {
			if (j == var_size - 1) {  // last variable is the insertion length
				h = NUM_JACOBIAN_INSERTIONLENGTH_STEPSIZE;
				hinv = 1 / NUM_JACOBIAN_INSERTIONLENGTH_STEPSIZE;
			}
			x_h = x_0;  x_h(j) = x_0(j) + h;
			f_h = CRM_ForwardKinematics(x_h, Params, dummyPE, lmin);
			Df_.col(j) = (f_h - f_0) * hinv;
			// for (int k = 0; k < f_size; ++k) {
			//	Df_(k, j) = (f_h(k) - f_0(k)) / h;
			//}
		}
		double dummy;  // temporary storage
		h = NUM_JACOBIAN_TIPFORCE_STEPSIZE;
		hinv = 1 / NUM_JACOBIAN_TIPFORCE_STEPSIZE;
		for (int j = var_size, k = 0; j < col_size; j++, k++) {
			dummy = Params.TipForce[k];
			Params.TipForce[k] += h;
			f_h = CRM_ForwardKinematics(x_0, Params, dummyPE, lmin);
			Df_.col(j) = (f_h - f_0) * hinv;
			Params.TipForce[k] = dummy;
		}
		out_y = f_0;

		// while we are at it, let's calculate the spatial angular velocity from \dot{R}
		int JRow = 3 + 3 + 3;				// pdot + ws + u_0dot
		MatrixXd T(JRow, f_size);
		MatrixXd J(JRow, col_size);
		T.setZero();
		T.block(0, 0, 3, 3).setIdentity();
		T.block(6, 12, 3, 3).setIdentity();
		T.block(3, 9, 1, 3) << f_0(6), f_0(7), f_0(8);
		T.block(4, 3, 1, 3) << f_0(9), f_0(10), f_0(11);
		T.block(5, 6, 1, 3) << f_0(3), f_0(4), f_0(5);
		J = T * Df_;

		return J;

	}


	MatrixXd CRM_FKJacobian_Analytical(const VectorXd in_x, const VectorXd& in_FKouty, CRMForwardKinematicsData in_Params) {

		auto& CathParams = *(in_Params.CathParams);
		auto& CathConfig = *(in_Params.CathConfig);
		double ActuationCurrents[NUM_ACT_SET][3];
		double InsertedLength;
		for (int i = 0; i < NUM_ACT_SET; i++)	for (int j = 0; j < 3; j++)	ActuationCurrents[i][j] = in_x(i * 3 + j);
		InsertedLength = in_x(NUM_ACT_SET * 3);

		CRMShootingMethodParams BVPParams = 
			CRMConstructShootingMethodParamSet(CathParams, CathConfig, InsertedLength, ActuationCurrents, in_Params.ContactMode, in_Params.TipConstraintPoint, in_Params.TipForce, in_Params.IntegrationStepSize);

		double deltau0_calc[3];
		double ftip_calc[3];

		for (int i = 0; i < 3; i++) deltau0_calc[i] = in_FKouty(i + 3 + 9);
		if (in_Params.ContactMode == ContactModeType::FREE_TIP) {
			for (int i = 0; i < 3; i++) ftip_calc[i] = in_Params.TipForce[i];
		}
		else {
			for (int i = 0; i < 3; i++) ftip_calc[i] = in_FKouty(i + 3 + 9 + 3);
		}

		double xf[NUM_STATES];
		double residual[3];

		auto [JBVP_p_z, JBVP_ws_z, JBVP_p_ft, JBVP_ws_ft, Jft_z] = CRMSolverIVPJacobian(BVPParams, deltau0_calc, ftip_calc, false, xf, residual);

		if (in_Params.ContactMode == ContactModeType::FREE_TIP) {
			MatrixXd JBVP_pws_zft(6, JBVP_p_z.cols() + 3);
			JBVP_pws_zft.template topLeftCorner<3, 3 * NUM_ACT_SET + 1>() = JBVP_p_z;
			JBVP_pws_zft.template bottomLeftCorner<3, 3 * NUM_ACT_SET + 1 >() = JBVP_ws_z;
			JBVP_pws_zft.template topRightCorner<3, 3>() = JBVP_p_ft;
			JBVP_pws_zft.template bottomRightCorner<3, 3>() = JBVP_ws_ft;
			return JBVP_pws_zft;
		}
		else { // (in_Params.ContactMode == ContactModeType::FIXED_TIP)
			return Jft_z;
		}

	}


	void CRM_Contact_Equation(double in_x[], double out_y[], CRMContactEquationParams Params) {

		double dummyPE;
		for (int i = 0; i < 3; i++) Params.TipForce[i] = IVALUE_SCALE_F * in_x[i];			// we will use the in_x as the tip foce
		Params.ContactMode = ContactModeType::FREE_TIP;										//    and calculate FK using FREE_TIP
		double fkoutput[3 + 9 + 3];		// tip position + R + u_0							//
		CRM_ForwardKinematics(Params.Actuation, fkoutput, dummyPE, Params);					//
		for (int i = 0; i < 3; i++) out_y[i] = RESIDUAL_SCALE_P * (fkoutput[i] - Params.TipConstraintPoint[i]);	// and return the residual

	}

	void CRM_Contact_Equation_AnalyticalJac(double in_x[], double out_y[], double out_fjac[], CRMContactEquationParams Params) {

		double dummyPE;
		// we need to solve the free space equilibrium equation first to send to jacobian calculation 
		for (int i = 0; i < 3; i++) Params.TipForce[i] = IVALUE_SCALE_F * in_x[i];			// we will use the in_x as the tip foce
		Params.ContactMode = ContactModeType::FREE_TIP;										//    and calculate FK using FREE_TIP
		double fkoutput[3 + 9 + 3];		// tip position + R + u_0							//
		CRM_ForwardKinematics(Params.Actuation, fkoutput, dummyPE, Params);					//
		for (int i = 0; i < 3; i++) out_y[i] = RESIDUAL_SCALE_P * (fkoutput[i] - Params.TipConstraintPoint[i]);	// and return the residual

		Matrix<double, 3 * NUM_ACT_SET + 1, 1> u;
		Matrix<double, 15, 1> y;
		for (int i = 0; i < 3 * NUM_ACT_SET + 1; i++) u(i) = Params.Actuation[i];
		for (int i = 0; i < 3 + 9 + 3; i++) y(i) = fkoutput[i];
		Matrix<double, 6, 3 * NUM_ACT_SET + 1 + 3 > Jfull = CRM_FKJacobian_Analytical(u, y, Params);  // [p;ws] vs [zc,zl,ft]
		Matrix<double, 3, 3> sJp = RESIDUAL_SCALE_P * Jfull.template topRightCorner<3, 3>() * IVALUE_SCALE_F;
		for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) out_fjac[i + j * 3] = sJp(i, j);  // Note that minpack uses Fortran style column-major ordering, not C sytle row-major ordering

	}

}
