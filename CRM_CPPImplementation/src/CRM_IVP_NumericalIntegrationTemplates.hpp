#pragma once

//
//
//	NUMERICAL INTEGRATION FUNCTIONS for IVP SOLVER
//
//

namespace CRMCatheterModel {

	//function [x_1toN] = ABM4(x_0, t_0, N, h, Integrand, initmethod)
	template <typename StVecType, typename ParamType>
	void ABM4(const StVecType& in_x_0, const double t_0, const int N, const double h,
		const ParamType in_Params, int32_t in_no_locmarkers,
		const bool CalculateEnergy,
		const bool FinalValueOnly, const double in_LocMarkers[], int& inout_NextLocMarkerIdx,
		StVecType& out_x_N, double& out_PotentialEnergy, double out_p_atLocMarkers[][3]) {

		using _SVT = StVecType;
		using _DVT = StDerivativeVectType<StVecType>;

		_SVT x_nm3;
		_SVT x_nm2;
		_SVT x_nm1;
		_SVT x_n(in_x_0);
		_SVT x_np1;
		double t_n;
		_DVT xdot_nm3;
		_DVT xdot_nm2;
		_DVT xdot_nm1;
		_DVT xdot_n;

		out_PotentialEnergy = 0.0;
		double Kun[3], Kunp1[3], unTKun, unp1TKun1p, unTKunp1, unp1TKun;
		int NextLocMarkerIdx = inout_NextLocMarkerIdx;
		auto& LocMarkers = in_LocMarkers;  // create an alias
		double delta_n_overh, delta_np1_overh;
		double gTpsum, mgTpmid;

		// initialize the iteration items
		t_n = t_0;

		for (int idx = 0; idx < N; idx++) {

			if (idx < 3) {  // RK2 initialization steps
				RK2_step(x_n, t_n, h, in_Params, x_np1, xdot_n);
			}
			else { 		 // ABM4 steps
				ABM4_step(x_n, t_n, h, xdot_nm1, xdot_nm2, xdot_nm3, x_nm1, x_nm2, x_nm3, in_Params, x_np1, xdot_n);
			}

			//Project_State_to_Manifold(x_np1);
			// increment "time"
			t_n = t_n + h;

			if (CalculateEnergy) {
				//
				// Elastic Potential Energy: \int u^T K u \approx ( u_n^T K u_n + ( u_n^T K u_np1 + u_np1^T K u_n ) /2 + u_np1^T K u_np1 ) /3
				//
				mMult_AB<3, 3, 1>(in_Params.K, x_n._u, Kun);
				mMult_AB<3, 3, 1>(in_Params.K, x_np1._u, Kunp1);
				mMult_ATB<3, 1, 1>(x_n._u, Kun, &unTKun);
				mMult_ATB<3, 1, 1>(x_n._u, Kunp1, &unTKunp1);
				mMult_ATB<3, 1, 1>(x_np1._u, Kun, &unp1TKun);
				mMult_ATB<3, 1, 1>(x_np1._u, Kunp1, &unp1TKun1p);
				out_PotentialEnergy += h * (unTKun + 0.5 * (unTKunp1 + unp1TKun) + unp1TKun1p) / 3.0;
				//
				// Gravitational Potential Energy: mass * gravity * height = (\rho * h) * (- g^T p_mid)
				//
				gTpsum = 0.0;
				for (int ix = 0; ix < 3; ix++) gTpsum += in_Params.g[ix] * (x_n._p[ix] + x_np1._p[ix]);
				mgTpmid = -0.5 * gTpsum;
				out_PotentialEnergy += in_Params.rho * h * mgTpmid;
			}

			if (!FinalValueOnly) {
				// are there any localization markers?  If so, calculate their positions
				// remember, t_n has already been incremented
				while ((NextLocMarkerIdx < in_no_locmarkers) && (LocMarkers[NextLocMarkerIdx] <= t_n)) {
					delta_np1_overh = (t_n - LocMarkers[NextLocMarkerIdx]) / h;
					delta_n_overh = 1.0 - delta_np1_overh;
					out_p_atLocMarkers[NextLocMarkerIdx][0] = delta_np1_overh * (x_n._p[0]) + delta_n_overh * (x_np1._p[0]);
					out_p_atLocMarkers[NextLocMarkerIdx][1] = delta_np1_overh * (x_n._p[1]) + delta_n_overh * (x_np1._p[1]);
					out_p_atLocMarkers[NextLocMarkerIdx][2] = delta_np1_overh * (x_n._p[2]) + delta_n_overh * (x_np1._p[2]);
					NextLocMarkerIdx++;
				}
			}

			// update the iteration items
			x_nm3 = x_nm2;
			x_nm2 = x_nm1;
			x_nm1 = x_n;
			x_n = x_np1;
			xdot_nm3 = xdot_nm2;
			xdot_nm2 = xdot_nm1;
			xdot_nm1 = xdot_n;

		}

		// Copy final value
		out_x_N = x_n;
		inout_NextLocMarkerIdx = NextLocMarkerIdx;

	}


	// [x_np1, xdot_n, xdot_nm1, xdot_nm2] = ABM4_step(x_n, t_n, xdot_nm1, xdot_nm2, xdot_nm3, h, Integrand)
	template <typename StVecType, typename ParamType>
	void ABM4_step(const StVecType& in_x_n, double t_n, double h,
		const StDerivativeVectType<StVecType>& in_xdot_nm1, const StDerivativeVectType<StVecType>& in_xdot_nm2, const StDerivativeVectType<StVecType>& in_xdot_nm3,
		const StVecType& in_x_nm1, const StVecType& in_x_nm2, const StVecType& in_x_nm3,
		const ParamType in_Params,
		StVecType& out_x_np1, StDerivativeVectType<StVecType>& out_xdot_n) {

		using _SVT = StVecType;
		using _DVT = StDerivativeVectType<StVecType>;

		const double P_COEFF_N = 55.0 / 24.0, P_COEFF_Nm1 = -59.0 / 24.0, P_COEFF_Nm2 = 37.0 / 24.0, P_COEFF_Nm3 = -9.0 / 24.0;  // AB4 Predictor Coefficients
		const double C_COEFF_Np1 = 9.0 / 24.0, C_COEFF_N = 19.0 / 24.0, C_COEFF_Nm1 = -5.0 / 24.0, C_COEFF_Nm2 = 1.0 / 24.0;     // AM4 Corrector Coefficients
		_SVT x_n(in_x_n);       		// from input
		_SVT x_nm1(in_x_nm1);       	// from input
		_SVT x_nm2(in_x_nm2);       	// from input
		_SVT x_nm3(in_x_nm3);       	// from input
		_SVT x_np1_hat;    				// intermediate
		_DVT xdot_np1_hat;				// intermediate
		auto& xdot_n = out_xdot_n;
		_DVT xdot_nm1(in_xdot_nm1);  	// from input
		_DVT xdot_nm2(in_xdot_nm2);  	// from input
		_DVT xdot_nm3(in_xdot_nm3);  	// from input


		//ABM4_STEP_STEP1:
		CRMIntegrand(t_n, x_n, in_Params, xdot_n);
		x_np1_hat = x_n + h * (P_COEFF_N * xdot_n + P_COEFF_Nm1 * xdot_nm1 + P_COEFF_Nm2 * xdot_nm2 + P_COEFF_Nm3 * xdot_nm3);
#ifdef ANALYTICAL_SE3_STEP
		//      calculate R_np1_hat and p_np1_hat analytically, without numerical integration
		double u_n_pred[3];
		for (int i = 0; i < 3; i++) u_n_pred[i] = (P_COEFF_N * x_n._u[i] + P_COEFF_Nm1 * x_nm1._u[i] + P_COEFF_Nm2 * x_nm2._u[i] + P_COEFF_Nm3 * x_nm3._u[i]);
		SE3_Analytical_Step(x_n._R, x_n._p, u_n_pred, h, x_np1_hat._R /*R_np1_hat*/, x_np1_hat._p /*p_np1_hat*/);
#endif
		//ABM4_STEP_STEP2:
		CRMIntegrand(t_n + h, x_np1_hat, in_Params, xdot_np1_hat);
		out_x_np1 = x_n + h * (C_COEFF_Np1 * xdot_np1_hat + C_COEFF_N * xdot_n + C_COEFF_Nm1 * xdot_nm1 + C_COEFF_Nm2 * xdot_nm2);
#ifdef ANALYTICAL_SE3_STEP
		//      calculate R_np1 and p_np1 analytically, without numerical integration
		double u_n_corr[3];
		for (int i = 0; i < 3; i++) u_n_corr[i] = (C_COEFF_Np1 * x_np1_hat._u[i] + C_COEFF_N * x_n._u[i] + C_COEFF_Nm1 * x_nm1._u[i] + C_COEFF_Nm2 * x_nm2._u[i]);
		SE3_Analytical_Step(x_n._R, x_n._p, u_n_corr, h, out_x_np1._R /*R_np1*/, out_x_np1._p /*p_np1*/);
#endif

	}


	//[x_np1, xdot_n] = RK2_step(x_n, t_n, h, Integrand)
	template <typename StVecType, typename ParamType>
	void RK2_step(const StVecType& in_x_n, const double t_n, const double h,
		const ParamType in_Params,
		StVecType& out_x_np1, StDerivativeVectType<StVecType>& out_xdot_n) {

		using _SVT = StVecType;
		using _DVT = StDerivativeVectType<StVecType>;

		_SVT x_n(in_x_n);       // from input
		_DVT k1;				// intermediate
		_DVT k2oh;				// intermediate
		_SVT x_n_p_k1o2;		// intermediate
		auto& xdot_n = out_xdot_n;// for output

		//RK2_STEP_STEP1:
		CRMIntegrand(t_n, x_n, in_Params, xdot_n);
		k1 = h * xdot_n;
		x_n_p_k1o2 = x_n + k1 * 0.5;
#ifdef ANALYTICAL_SE3_STEP
		// we will calculate R_n_p_k1o2 and p_n_p_k1o2 analytically, without numerical integration
		SE3_Analytical_Step(x_n._R, x_n._p, x_n._u, h * 0.5, x_n_p_k1o2._R, x_n_p_k1o2._p);
#endif

		//RK2_STEP_STEP2:
		CRMIntegrand(t_n + h * 0.5, x_n_p_k1o2, in_Params, k2oh);
		out_x_np1 = x_n + h * k2oh;
#ifdef ANALYTICAL_SE3_STEP
		// we will calculate R_np1 and p_np1 analytically, without numerical integration
		SE3_Analytical_Step(x_n._R, x_n._p, x_n_p_k1o2._u/*u_np1half*/, h, out_x_np1._R, out_x_np1._p);
#endif

	}


}
