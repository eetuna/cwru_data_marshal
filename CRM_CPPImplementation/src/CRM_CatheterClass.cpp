#include "CRM.hpp"

namespace CRMCatheterModel {


	CRM_Catheter::CRM_Catheter(CRMCatheterModelParams Params, CatheterConfiguration Config) : CathParams(Params), CathConfig(Config) {

		FKParams.CathParams = &CathParams;
		FKParams.CathConfig = &CathConfig;
		for (int i = 0; i < 3; i++)
			FKParams.TipForce[i] = FKParams.deltau0_initialguess[i] = FKParams.ftip_initialguess[i] = 0.0;
		FKParams.FinalValueOnly = true;
		MarkerPos = new double[CathParams.no_locmarkers][3];
		FKParams.ReportedMarkerPos = MarkerPos;
		CoilPos = new double[CathParams.no_act_set][3];
		FKParams.ReportedCoilPos = CoilPos;
		CoilOrient = new double[CathParams.no_act_set][9];
		FKParams.ReportedCoilOrient = CoilOrient;
		IntegrationStepSize = 0.2;
		last_deltau0.setZero();
		last_ftip.setZero();

	}

	CRM_Catheter::~CRM_Catheter() {
		delete[] MarkerPos;
		delete[] CoilPos;
		delete[] CoilOrient;
	}


	std::tuple < Vector3d, Matrix3d, Vector3d > CRM_Catheter::ForwardKinematicsFree(const VectorXd ActuationVector, const Vector3d TipForce,
		const Vector3d deltau0_initialguess, bool CalculateMarkers, double& out_PotentialEnergy, int& out_localmin) {

		FKParams.ContactMode = ContactModeType::FREE_TIP;
		FKParams.FinalValueOnly = !CalculateMarkers;
		for (int i = 0; i < 3; i++) FKParams.TipForce[i] = TipForce(i);
		for (int i = 0; i < 3; i++) FKParams.deltau0_initialguess[i] = deltau0_initialguess(i);
		last_ActuationVector = ActuationVector;
		last_FKsolution = CRM_ForwardKinematics(ActuationVector, FKParams, out_PotentialEnergy, out_localmin);
		Vector3d ptip = last_FKsolution.segment<3>(0);
		Matrix3d Rtip;
		Rtip.row(0) = last_FKsolution.segment<3>(3 + 0);
		Rtip.row(1) = last_FKsolution.segment<3>(3 + 3);
		Rtip.row(2) = last_FKsolution.segment<3>(3 + 6);
		Vector3d deltau0 = last_FKsolution.segment<3>(3 + 9);
		if (out_localmin == 0) {
			last_deltau0 = last_FKsolution.segment<3>(3 + 9);
			last_ftip = TipForce;
		}

		return { ptip, Rtip, deltau0 };

	}

	std::tuple < Vector3d, Matrix3d, Vector3d > CRM_Catheter::ForwardKinematicsFree(const VectorXd ActuationVector, const bool CalculateMarkers, double& out_PotentialEnergy, int& out_localmin) {

		Vector3d zero = { 0.0, 0.0, 0.0 };
		return ForwardKinematicsFree(ActuationVector, zero, last_deltau0, CalculateMarkers, out_PotentialEnergy, out_localmin);

	}

	std::tuple<Vector3d, Matrix3d, Vector3d, Vector3d> CRM_Catheter::ForwardKinematicsContact(const VectorXd ActuationVector, const Vector3d TipConstraintPoint,
		const Vector3d u0_initialguess, const Vector3d ftip_initialguess, const bool CalculateMarkers, double& out_PotentialEnergy, int& out_localmin) {

		FKParams.ContactMode = ContactModeType::FIXED_TIP;
		FKParams.FinalValueOnly = !CalculateMarkers;
		for (int i = 0; i < 3; i++) FKParams.TipConstraintPoint[i] = TipConstraintPoint(i);
		for (int i = 0; i < 3; i++) FKParams.deltau0_initialguess[i] = u0_initialguess(i);
		for (int i = 0; i < 3; i++) FKParams.ftip_initialguess[i] = ftip_initialguess(i);
		last_ActuationVector = ActuationVector;
		last_FKsolution = CRM_ForwardKinematics(ActuationVector, FKParams, out_PotentialEnergy, out_localmin);
		Vector3d ptip = last_FKsolution.segment<3>(0);
		Matrix3d Rtip;
		Rtip.row(0) = last_FKsolution.segment<3>(3 + 0);
		Rtip.row(1) = last_FKsolution.segment<3>(3 + 3);
		Rtip.row(2) = last_FKsolution.segment<3>(3 + 6);
		Vector3d deltau0 = last_FKsolution.segment<3>(3 + 9);
		Vector3d ftip = last_FKsolution.segment<3>(3 + 9 + 3);
		if (out_localmin == 0) {
			last_deltau0 = deltau0;
			last_ftip = ftip;
		}

		return { ptip, Rtip, deltau0, ftip };

	}

	std::tuple<Vector3d, Matrix3d, Vector3d, Vector3d> CRM_Catheter::ForwardKinematicsContact(const VectorXd ActuationVector, const Vector3d TipConstraintPoint,
		const bool CalculateMarkers, double& out_PotentialEnergy, int& out_localmin) {

		return ForwardKinematicsContact(ActuationVector, TipConstraintPoint, last_deltau0, last_ftip, CalculateMarkers, out_PotentialEnergy, out_localmin);

	}

	MatrixXd CRM_Catheter::FKJacobian_Analytical() {

		return CRM_FKJacobian_Analytical(last_ActuationVector, last_FKsolution, FKParams);

	}

}