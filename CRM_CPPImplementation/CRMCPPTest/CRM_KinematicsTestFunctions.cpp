#include "CRM.hpp"
#include "CRM_BVPIVP_APIDeclarations.hpp"
#include "CRM_IVPJacobian.hpp"
#include "CRM_KinematicsTestFunctions.hpp"

using Eigen::Matrix;
using Eigen::RowMajor;

namespace CRMCatheterModel {


	MatrixXd CRM_FKJacobian_BruteForce(const VectorXd in_x, VectorXd& out_y, CRMForwardKinematicsData in_Params, int& localmin) {

		CRMForwardKinematicsData Params = in_Params;					// create a local copy
		Params.FinalValueOnly = true;
		double dummyPE;
		VectorXd x_0 = in_x;									// create a local copy
		VectorXd f_0 = CRM_ForwardKinematics(x_0, Params, dummyPE, localmin);	// calculate forward kinematics at x_0
		auto f_size = f_0.size();
		auto var_size = x_0.size();
		MatrixXd Df_(f_size, var_size);
		VectorXd x_h(var_size), f_h(f_size);
		int lmin;

		// we will use the deltau0 calculated for FK at x_0 as the initial guess for all of the subsequent FK calculations
		for (int i = 0; i < 3; i++) {
			Params.deltau0_initialguess[i] = f_0(3 + 9 + i);
		}
		// in_Params.ContactMode == ContactModeType::FIXED_TIP, we will aslo use the ftip calculated for FK at x_0 as the initial guess for all of the subsequent FK calculations
		if (Params.ContactMode == ContactModeType::FIXED_TIP) {
			for (int i = 0; i < 3; i++) {
				Params.ftip_initialguess[i] = f_0(3 + 9 + 3 + i);
			}
		}
		f_0 = CRM_ForwardKinematics(x_0, Params, dummyPE, localmin);	// calculate forward kinematics at x_0 with the new initial conditions for numerical stability

		double h = NUM_JACOBIAN_CURRENT_STEPSIZE;
		double hinv = 1 / NUM_JACOBIAN_CURRENT_STEPSIZE;
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
		out_y = f_0;

		// while we are at it, let's calculate the spatial angular velocity from \dot{R}
		int JRow;
		if (Params.ContactMode == ContactModeType::FREE_TIP) {
			JRow = 3 + 3 + 3;				// pdot + ws + u_0dot
		}
		else { // FIXED_TIP
			JRow = 3 + 3 + 3 + 3;			//   ... + tip force dot
		}
		MatrixXd T(JRow, f_size);
		MatrixXd J(JRow, var_size);
		T.setZero();
		T.block(0, 0, 3, 3).setIdentity();
		T.block(6, 12, 3, 3).setIdentity();
		T.block(3, 9, 1, 3) << f_0(6), f_0(7), f_0(8);
		T.block(4, 3, 1, 3) << f_0(9), f_0(10), f_0(11);
		T.block(5, 6, 1, 3) << f_0(3), f_0(4), f_0(5);
		if (Params.ContactMode == ContactModeType::FIXED_TIP) 	T.block(9, 15, 3, 3).setIdentity();
		J = T * Df_;

		return J;

	}


	MatrixXd CRM_FKJacobian_From_IVP_Numerical(const VectorXd in_x, const VectorXd& in_FKouty, CRMForwardKinematicsData in_Params) {

		auto& CathParams = *(in_Params.CathParams);
		auto& CathConfig = *(in_Params.CathConfig);
		double ActuationCurrents[NUM_ACT_SET][3];
		double InsertedLength;
		for (int i = 0; i < NUM_ACT_SET; i++)	for (int j = 0; j < 3; j++)	ActuationCurrents[i][j] = in_x(i * 3 + j);
		InsertedLength = in_x(NUM_ACT_SET * 3);

		CRMShootingMethodParams BVPParams = 
			CRMConstructShootingMethodParamSet(CathParams, CathConfig, InsertedLength, ActuationCurrents, in_Params.ContactMode, in_Params.TipConstraintPoint, in_Params.TipForce, in_Params.IntegrationStepSize);

		Matrix<double, 3 + 9 + 3 + 3 + 3 * NUM_ACT_SET + 1, 1> x0, xh;
		Matrix<double, 3 + 9 + 3, 1> y, y0;
		Matrix<double, 3 + 9 + 3, 3 + 9 + 3 + 3 * NUM_ACT_SET + 1 + 3> J_reversezc;

		if (in_Params.ContactMode == ContactModeType::FREE_TIP) {
			x0.template head<3 + 9 + 3>() = in_FKouty;
			for (int i = 0; i < 3; i++) x0(3 + 9 + 3 + i) = in_Params.TipForce[i];
		}
		else {
			x0.template head<3 + 9 + 3 + 3>() = in_FKouty;

		}
		x0.template segment<3 * NUM_ACT_SET + 1>(3 + 9 + 3 + 3) = in_x;

		y0 = CRMSolverIVPWrapper(x0, BVPParams);
		double h;
		for (int i = 0; i < x0.rows(); i++) {
			xh = x0;
			if (i < 3) h = NUM_JACOBIAN_BASEPOSITION_STEPSIZE;
			else if (i < 3 + 9) h = NUM_JACOBIAN_BASEROTATION_STEPSIZE;
			else if (i < 3 + 9 + 3) h = NUM_JACOBIAN_BASECURVATURE_STEPSIZE;
			else if (i < 3 + 9 + 3 + 3) h = NUM_JACOBIAN_TIPFORCE_STEPSIZE;
			else if (i < 3 + 9 + 3 + 3 + 3 * NUM_ACT_SET) h = NUM_JACOBIAN_CURRENT_STEPSIZE;
			else h = NUM_JACOBIAN_INSERTIONLENGTH_STEPSIZE;
			xh(i) += h;
			y = CRMSolverIVPWrapper(xh, BVPParams);
			J_reversezc.col(i) = (y - y0) / h;
		}

		Matrix<double, 3 + 9 + 3, 3 + 9 + 3 + 3 * NUM_ACT_SET + 1 + 3> JIVP_x_phi;
		JIVP_x_phi.template leftCols<3 + 9 + 3>() = J_reversezc.template leftCols<3 + 9 + 3>();
		// reorder ft
		JIVP_x_phi.template rightCols<3>() = J_reversezc.template middleCols<3>(3 + 9 + 3);
		// we will need to flip the order of actuators in zc, since IVP calculations use proximal to distal ordering
		for (int i = 0; i < NUM_ACT_SET; i++) JIVP_x_phi.template middleCols<3>(3 + 9 + 3 + 3 * i) = J_reversezc.template middleCols<3>(3 + 9 + 3 + 3 + 3 * ((NUM_ACT_SET - 1) - i));
		JIVP_x_phi.template middleCols<1>(3 + 9 + 3 + 3 * NUM_ACT_SET) = J_reversezc.template rightCols<1>();

		// let's calculate the FK Jacobians
		// for simplicity, define an alias
		constexpr unsigned int Cs = CURRENT_ACT_VECTOR_DIM;

		// while we are at it, let's calculate the spatial angular velocity from \dot{R}
		Matrix<double, 9, 1> _R = in_FKouty.template segment<9>(3);
		Matrix<double, 3, 9, RowMajor> T;
		T.setZero();
		T.block(0, 6, 1, 3) << _R(3), _R(4), _R(5);
		T.block(1, 0, 1, 3) << _R(6), _R(7), _R(8);
		T.block(2, 3, 1, 3) << _R(0), _R(1), _R(2);

		Matrix<double, 3, 3, RowMajor> JIVP_p_u0 = JIVP_x_phi.template block<3, 3>(0, 3 + 9);
		Matrix<double, 3, Cs + 1, RowMajor> JIVP_p_z = JIVP_x_phi.template block<3, Cs + 1>(0, 3 + 9 + 3);
		Matrix<double, 3, 3, RowMajor> JIVP_p_ft = JIVP_x_phi.template block<3, 3>(0, 3 + 9 + 3 + Cs + 1);
		Matrix<double, 3, 3, RowMajor> JIVP_u_u0 = JIVP_x_phi.template block<3, 3>(3 + 9, 3 + 9);
		Matrix<double, 3, Cs + 1, RowMajor> JIVP_u_z = JIVP_x_phi.template block<3, Cs + 1>(3 + 9, 3 + 9 + 3);
		Matrix<double, 3, 3, RowMajor> JIVP_u_ft = JIVP_x_phi.template block<3, 3>(3 + 9, 3 + 9 + 3 + Cs + 1);
		Matrix<double, 3, 3, RowMajor> JIVP_ws_u0 = T * JIVP_x_phi.template block<9, 3>(3, 3 + 9);// _R_u0;
		Matrix<double, 3, 3, RowMajor> JIVP_ws_ft = T * JIVP_x_phi.template block<9, 3>(3, 3 + 9 + 3 + Cs + 1);// _R_ft;
		Matrix<double, 3, Cs + 1, RowMajor> JIVP_ws_z = T * JIVP_x_phi.template block<9, Cs + 1>(3, 3 + 9 + 3);// _R_z;

		MatrixXd JIVP_u_u0_pinv = JIVP_u_u0.completeOrthogonalDecomposition().pseudoInverse();
		MatrixXd JBVP_p_z = JIVP_p_z - JIVP_p_u0 * JIVP_u_u0_pinv * JIVP_u_z;
		MatrixXd JBVP_ws_z = JIVP_ws_z - JIVP_ws_u0 * JIVP_u_u0_pinv * JIVP_u_z;

		MatrixXd JBVP_p_ft = JIVP_p_ft - JIVP_p_u0 * JIVP_u_u0_pinv * JIVP_u_ft;
		MatrixXd Jft_z = -JBVP_p_ft.completeOrthogonalDecomposition().pseudoInverse() * JBVP_p_z;  // Is this the best option in Eigen ???


		if (in_Params.ContactMode == ContactModeType::FREE_TIP) {
			MatrixXd JBVP_pws_z(6, JBVP_p_z.cols());
			JBVP_pws_z.topRows(3) = JBVP_p_z;
			JBVP_pws_z.bottomRows(3) = JBVP_ws_z;
			return JBVP_pws_z;
		}
		else { // (in_Params.ContactMode == ContactModeType::FIXED_TIP)
			return Jft_z;
		}

	}


	VectorXd CRMSolverIVPWrapper(VectorXd in, CRMShootingMethodParams in_Params) {

		CRMShootingMethodParams Params = in_Params;
		double u0_calc[3];
		double ftip_calc[3];
		double actvect[3];

		for (int i = 0; i < 3; i++) Params.p0[i] = in(i);
		for (int i = 0; i < 9; i++) Params.R0[i] = in(i + 3);
		for (int i = 0; i < 3; i++) u0_calc[i] = in(i + 3 + 9);
		for (int i = 0; i < 3; i++) ftip_calc[i] = in(i + 3 + 9 + 3);
		for (int i = 0; i < NUM_ACT_SET; i++) {
			for (int j = 0; j < 3; j++) actvect[j] = in(i * 3 + j + 3 + 9 + 3 + 3);
			mMult_AB<3, 3, 1>(Params.CoilAlignmentTurnAreaMatrix[i], actvect, Params.MagMoment[i]);
		}
		Params.Li = in(in.rows() - 1);


		double xf[NUM_STATES];
		__EVT<15> XF(xf);
		double residual[3];
		double dummyPE;
		double (*out_p_atLocMarkers)[3] = new double [in_Params.no_locmarkers][3]; // dummy
		double (*out_p_atActuators)[3] = new double[in_Params.no_act_set][3]; // dummy
		double (*out_R_atActuators)[9] = new double[in_Params.no_act_set][9]; // dummy

		CRMSolverIVP(Params, u0_calc, ftip_calc, false, true, xf, residual, dummyPE, out_p_atLocMarkers, out_p_atActuators, out_R_atActuators);

		delete[] out_p_atLocMarkers;
		delete[] out_R_atActuators;

		VectorXd result = XF;
		return result;
	}


}
