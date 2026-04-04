#include "CRM.hpp"		// This is the only header that needs to be included to use the standard CRM Kinematics APIs
using namespace CRMCatheterModel;
#include "CRMTest.h"	// The CRMTest specific stuff is here


template <typename T>
void printMatrix(T *p, int D1, int D2, const char *text) {
	std::cout << text << " --" << std::endl;
	for (int i=0; i<D1; i++) {
		for (int j=0; j<D2; j++) {
			std::cout << *(p+i*D2+j) << " ";
		}
		std::cout << std::endl;
	}
	//std::cout << "----" << std::endl;
}

template <int D1, int D2>
bool MatrixEqual(double A[D1][D2], double B[D1][D2], double eps) {
	for (int i=0; i<D1;i++)
		for (int j=0; j<D2; j++)
			if (fabs(A[i][j]-B[i][j])<eps) {} else return false;
	return true;
}

void wait_for_enter(const std::string &msg) {
    std::cout << msg << std::endl;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


int RunExample(void);
int RunFKExample(void);
int RunFKContactExample(void);
int RunCatheterClassExample(void);
int RunTests(void);
int ConstrainedTipTest(void);

int main(int argc, char** argv) {

	int FailFlag = 0;

	//RunExample();

	std::cout << std::endl << std::endl << "### Running Examples ..." << std::endl << std::endl;

	RunFKExample();
	RunFKContactExample();
	RunCatheterClassExample();

	std::cout << std::endl << std::endl << "### Running Tests ..." << std::endl << std::endl;
	
	FailFlag += RunTests();
	if (FailFlag == 0) {
		std::cout << std::endl << std::endl << "###" << std::endl;
		std::cout << "ALL TESTS PASSED!..." << std::endl;
		std::cout << "###" << std::endl;
	}
	else {
		std::cout << std::endl << std::endl << "###" << std::endl;
		std::cout << FailFlag << " TEST(S) FAILED!..." << std::endl;
		std::cout << "###" << std::endl;
	}
	return (FailFlag);



}


//
// Example showing how to use the CRM_Catheter Class
//		to calculate the catheter forward kinematics
//
int RunCatheterClassExample(void) {

	std::cout << "### RunCatheterClassExample() --- CRM Catheter Class Usage Examples... " << std::endl;
	std::cout << std::endl << "Free Space Deflection Example: " << std::endl << std::endl;

	// *** Load parameters from file
	// This step would typically needs to be executed only once
	//   Physical Description of the Catheter
	CRMCatheterModelParams CathParams = Load_CRMCatheterModelParams("../catheterdata/CatheterParameterSet_2.txt");
	//   Catheter Configuration in spatial coordinates
	CatheterConfiguration CathConfig = Load_CatheterConfiguration("../catheterdata/CatheterSpatialConfiguration_1.txt");

	// Let's instantiate the CRM_Catheter class
	CRM_Catheter Catheter(CathParams, CathConfig);

	// *** Numerical Computation Parameters
	// Stepsize used in numerical integration along the length of the catheter during IVP - unit: mm
	Catheter.IntegrationStepSize = 0.2;
	// define variable for numerical nonlinear equation solver diagnostic output
	int localmin;
	// we will not calculate the marker locations
	bool CalculateMarkers = true;

	// *** Define Variables for Control Inputs
	VectorXd control_inputs(NUM_ACT_SET * 3 + 1); // actuator currents (distal to proximal) followed by inserted length

	// *** and for Outputs
	Vector3d ptip;
	Matrix3d Rtip;
	Vector3d deltau0;
	Vector3d ftip;
	double   PotentialEnergy;

	// *** Other External variables
	// External point force (in spatial coordinates) applied at the tip of the catheter (\lambda = 0)  - unit: ??
	//   (this will be used with ForwardKinematicsFree
	Vector3d TipForce = { 0.0, 0.0, 0.0 };
	// The spatial coordinates of the point where the catheter tip is constrained to be 
	//   (this will be used with ForwardKinematicsContact)
	Vector3d TipConstraintPoint = { 6.12, 40.57, 93.96 };

	// *** Assign Control Inputs
	// convenience variables --- Inserted Length of the catheter (length of the catheter that is inside the heart chamber) - unit: mm
	double InsertedLength = 100.0; //40.0;
	// convenience variable --- Actuation currents for each of the coils for each of the coil sets - unit: A
	double ActuationCurrents[NUM_ACT_SET][3] = { {0.100, 0.100, 0.100}, { 0.100, 0.100, 0.100 } };
	// assign control input vector from convenience variables
	for (int i = 0; i < NUM_ACT_SET; i++)  for (int j = 0; j < 3; j++) control_inputs(i * 3 + j) = ActuationCurrents[i][j];
	control_inputs(NUM_ACT_SET * 3) = InsertedLength;

	//  *** The following would be used when non-incremental version of FK are used
	// Define initial guesses to be used when solving boundary value problem
	// initial guess for the delta_curvature at the catheter base ( u0 = deltau0 + ustar0 ) 
	Vector3d deltau0_initialguess = { 0.0, 0.0, 0.0 };
	// initial guess for the contstraint force at the catheter tip (this will be used when ContactMode == ContactModeType::FIXED_TIP)
	Vector3d ftip_initialguess = { 0.0, 0.0, 0.0 };

	//
	// FREE TIP
	//

	//
	// Forward Kinematics Calculation using the Catheter Class
	//

	// Cosserat Rod Model - Solve the Forward Kinematics 
	// incremental version
	std::tie(ptip, Rtip, deltau0) = Catheter.ForwardKinematicsFree(control_inputs, CalculateMarkers, PotentialEnergy, localmin);
	// or full version
	//std::tie(ptip, Rtip, deltau0) = Catheter.ForwardKinematicsFree(control_inputs, TipForce, deltau0_initialguess, CalculateMarkers, localmin);

	//display the results,
	std::cout << "FK Output -- ptip,Rtip,deltau0: \n" << ptip << "\n" << Rtip << "\n" << deltau0  << "\n";
	std::cout << "PE:" << PotentialEnergy << std::endl;
	std::cout << "localmin:" << localmin << std::endl;
	std::cout << "----" << std::endl;

	std::cout << "Localization Marker Positions" << std::endl << 
		Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(&(Catheter.MarkerPos[0][0]), Catheter.CathParams.no_locmarkers, 3) << std::endl;
	for (int32_t i = 0; i < Catheter.CathParams.no_act_set; i++) {
		std::cout << "Coil #" << i << " Position" << std::endl << Eigen::Map<Eigen::Vector<double, 3>>(Catheter.CoilPos[i]) << std::endl;
		std::cout << "Coil #" << i << " Orientation" << std::endl << Eigen::Map<Eigen::Matrix<double, 3, 3, Eigen::RowMajor>>(Catheter.CoilOrient[i]) << std::endl;
	}
	std::cout << "----" << std::endl;

	//
	// Jacobian calculation
	//

	// First, we will use the analytical Jacobian calculation using CRMSolverIVPJacobian
	MatrixXd Ja;

	// Cosserat Rod Model - Solve the Forward Kinematics and calculate the Jacobian
	Ja = Catheter.FKJacobian_Analytical(); // evaluate the Jacobian matrix J only

	std::cout << "Ja = \n" << Ja << std::endl;			// print the evaluated Jacobian matrix

	std::cout << "----" << std::endl;
	std::cout << std::endl << "Constrained Tip Deflection Example: " << std::endl << std::endl;

	//
	// We will reset initial guesses
	//   we will use the incremental version of the method
	//   but we don't want to use the "unrelated" initial guesses from the calculation above
	//
	Catheter.ResetInitialGuesses();

	//
	// CONSTRAINED TIP
	//

	//  we will use a different insertion length
	control_inputs(NUM_ACT_SET * 3) = 104.0;

	//
	// Forward Kinematics Calculation using the Catheter Class
	//

	// Cosserat Rod Model - Solve the Forward Kinematics for the Constrained Tip Case
	// incremental version
	std::tie(ptip, Rtip, deltau0, ftip) = Catheter.ForwardKinematicsContact(control_inputs, TipConstraintPoint, CalculateMarkers, PotentialEnergy, localmin);
	// or full version
	//std::tie(ptip, Rtip, deltau0, ftip) = Catheter.ForwardKinematicsContact(control_inputs, TipConstraintPoint, deltau0_initialguess, ftip_initialguess, CalculateMarkers, localmin);

	//display the results,
	std::cout << "FK Output -- p,R,deltau0,ftip: \n" << ptip << "\n" << Rtip << "\n" << deltau0 << "\n" << ftip << "\n";
	std::cout << "PE:" << PotentialEnergy << std::endl;
	std::cout << "localmin:" << localmin << std::endl;
	std::cout << "----" << std::endl;

	std::cout << "Localization Marker Positions" << std::endl <<
		Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(&(Catheter.MarkerPos[0][0]), Catheter.CathParams.no_locmarkers, 3) << std::endl;
	for (int32_t i = 0; i < Catheter.CathParams.no_act_set; i++) {
		std::cout << "Coil #" << i << " Position" << std::endl << Eigen::Map<Eigen::Vector<double, 3>>(Catheter.CoilPos[i]) << std::endl;
		std::cout << "Coil #" << i << " Orientation" << std::endl << Eigen::Map<Eigen::Matrix<double, 3, 3, Eigen::RowMajor>>(Catheter.CoilOrient[i]) << std::endl;
	}
	std::cout << "----" << std::endl;


	//
	// Jacobian calculation
	// 

	// First, we will use the analytical Jacobian calculation using CRMSolverIVPJacobian
	//MatrixXd Ja;

	// Cosserat Rod Model - Solve the Forward Kinematics and calculate the Jacobian
	Ja = Catheter.FKJacobian_Analytical(); // evaluate the Jacobian matrix J only

	std::cout << "Ja = \n" << Ja << std::endl;			// print the evaluated Jacobian matrix

	return (localmin);

}


//
// Example showing how to use the CRM_ForwardKinematics functions 
//		to calculate the catheter forward kinematics
// This is the preferred method
//
int RunFKExample(void) {

	std::cout << "### RunFKExample() --- CRM Forward Kinematics Examples... " << std::endl;
	std::cout << std::endl << "Free Space Deflection Example: " << std::endl << std::endl;

	// *** Load parameters from file
	// This step would typically needs to be executed only once
	//   Physical Description of the Catheter
	CRMCatheterModelParams CathParams = Load_CRMCatheterModelParams("../catheterdata/CatheterParameterSet_2.txt");
	//   Catheter Configuration in spatial coordinates
	CatheterConfiguration CathConfig = Load_CatheterConfiguration("../catheterdata/CatheterSpatialConfiguration_1.txt");

	// *** Other External variables
	// specify if catheter is in free space or if the catheter tip is constrained to a contact point
	ContactModeType ContactMode = ContactModeType::FREE_TIP;
	// External point force (in spatial coordinates) applied at the tip of the catheter (\lambda = 0)  - unit: ??
	//   (this will be used when ContactMode == ContactModeType::FREE_TIP)
	double TipForce[3] = { 0.0, 0.0, 0.0 };
	// The spatial coordinates of the point where the catheter tip is constrained to be 
	//   (this will be used when ContactMode == ContactModeType::FIXED_TIP)
	double TipConstraintPoint[3] = { 0.0, 0.0, 0.0 };

	// *** Control Inputs
	// Inserted Length of the catheter (length of the catheter that is inside the heart chamber) - unit: mm
	double InsertedLength = 100.0; //40.0;
	// Actuation currents for each of the coils for each of the coil sets - unit: A
	double ActuationCurrents[NUM_ACT_SET][3] = { {0.100, 0.100, 0.100}, { 0.100, 0.100, 0.100 } };

	// *** Numerical Computation Parameters
	// Stepsize used in numerical integration along the length of the catheter during IVP - unit: mm
	double IntegrationStepSize = 0.2;

	// *** Storage for storing localization marker positions and actuation coil orientations
	double (*ReportedMarkerPos)[3] = new double [CathParams.no_locmarkers][3];
	double (*ReportedCoilPos)[3] = new double[CathParams.no_act_set][3];
	double (*ReportedCoilOrient)[9] = new double[CathParams.no_act_set][9];


	// Define initial guesses to be used when solving boundary value problem
	// initial guess for the delta_curvature at the catheter base ( u0 = deltau0 + ustar0 ) 
	double deltau0_initialguess[3] = { 0.0, 0.0, 0.0 };
	// initial guess for the contstraint force at the catheter tip (this will be used when ContactMode == ContactModeType::FIXED_TIP)
	double ftip_initialguess[3] = { 0.0, 0.0, 0.0 };

	// numerical nonlinear equation solver diagnostic outputs
	int localmin;

	//
	// New Forward Kinematics Function
	//

	// The Forward Kinematics Function uses a different parameter structure than BVP
	CRMForwardKinematicsData FKParams;
	FKParams.CathConfig = &CathConfig;
	FKParams.CathParams = &CathParams;
	FKParams.ContactMode = ContactMode;
	FKParams.FinalValueOnly = false;  // we want the localization coil locations, too
	FKParams.ReportedMarkerPos = ReportedMarkerPos;
	FKParams.ReportedCoilPos = ReportedCoilPos;
	FKParams.ReportedCoilOrient = ReportedCoilOrient;
	for (int i = 0; i < 3; i++) FKParams.TipConstraintPoint[i] = TipConstraintPoint[i];
	for (int i = 0; i < 3; i++) FKParams.TipForce[i] = TipForce[i];
	for (int i = 0; i < 3; i++) FKParams.deltau0_initialguess[i] = deltau0_initialguess[i];
	for (int i = 0; i < 3; i++) FKParams.ftip_initialguess[i] = ftip_initialguess[i];
	FKParams.IntegrationStepSize = IntegrationStepSize;
	// inputs
	VectorXd control_inputs(NUM_ACT_SET * 3 + 1); // actuator currents (distal to proximal) followed by inserted length
	for (int i = 0; i < NUM_ACT_SET; i++)  for (int j = 0; j < 3; j++) control_inputs(i * 3 + j) = ActuationCurrents[i][j];
	control_inputs(NUM_ACT_SET * 3) = InsertedLength;
	// and outputs
	int outputdim;
	if (ContactMode == ContactModeType::FREE_TIP) outputdim = 15; else outputdim = 18;
	VectorXd FKsolution(outputdim); // p[0..2],R[0..8],deltau0[0..2](,ftip[0..2])  R: in row major order
	double   PotentialEnergy;

	// Multiple repetitions to more reliably measure time
	int REPS = 100;
	// Get starting timepoint 
	auto start = high_resolution_clock::now();

	// Cosserat Rod Model - Solve the Forward Kinematics 
	for (int cnt = 0; cnt < REPS; cnt++)
		FKsolution = CRM_ForwardKinematics(control_inputs, FKParams, PotentialEnergy, localmin);

	// Get ending timepoint 
	auto stop = high_resolution_clock::now();
	// Get duration. Substart timepoints to  
	// get duration. To cast it to proper unit 
	// use duration cast method 
	auto duration = duration_cast<microseconds>(stop - start);
	std::cout << std::endl << "Average time taken by FK Solution in " << REPS << " repetitions: " << duration.count() / REPS << " microseconds" << std::endl;

	//display the results,
	std::cout << "FK Output -- p,R,deltau0: \n" << FKsolution.transpose() << "\n";
	std::cout << "PE:" << PotentialEnergy << std::endl;
	std::cout << "localmin:" << localmin << std::endl;
	std::cout << "----" << std::endl;

	MatrixXd MPos = Eigen::Map<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,Eigen::RowMajor>>(&(FKParams.ReportedMarkerPos[0][0]), CathParams.no_locmarkers, 3);
	MatrixXd C0Pos = Eigen::Map<Eigen::Vector<double, 3>>(FKParams.ReportedCoilPos[0]);
	MatrixXd C0Rot = Eigen::Map<Eigen::Matrix<double, 3, 3, Eigen::RowMajor>>(FKParams.ReportedCoilOrient[0]);
	MatrixXd C1Pos = Eigen::Map<Eigen::Vector<double, 3>>(FKParams.ReportedCoilPos[1]);
	MatrixXd C1Rot = Eigen::Map<Eigen::Matrix<double, 3, 3, Eigen::RowMajor>>(FKParams.ReportedCoilOrient[1]);

	std::cout << "Localization Marker Positions" << std::endl << MPos << std::endl;
	std::cout << "Coil 0 Position and Orientation" << std::endl << C0Pos << std::endl << C0Rot << std::endl << 
					"Coil 1 Position and Orientation" << std::endl << C1Pos << std::endl << C1Rot << std::endl;
	std::cout << "----" << std::endl;

	//
	// Jacobian calculation
	//

	// First, we will use the analytical Jacobian calculation using CRMSolverIVPJacobian
	MatrixXd Ja;
	
	// Get starting timepoint 
	start = high_resolution_clock::now();

	// Cosserat Rod Model - calculate the Jacobian using the solution of the Forward Kinematics
	for (int cnt = 0; cnt < REPS; cnt++)
		Ja = CRM_FKJacobian_Analytical(control_inputs, FKsolution, FKParams); // evaluate the Jacobian matrix J only

	// Get ending timepoint 
	stop = high_resolution_clock::now();
	// Get duration. Substart timepoints to  
	// get duration. To cast it to proper unit 
	// use duration cast method 
	duration = duration_cast<microseconds>(stop - start);
	std::cout << std::endl << "Average time taken by Analytical Jacobian calculation in " << REPS << " repetitions: " << duration.count() / REPS << " microseconds" << std::endl;

	std::cout << "Ja = \n" << Ja << std::endl;			// print the evaluated Jacobian matrix

	// Then, we will use the numerical Jacobian calculation
	VectorXd outputs;								// the output vector evaluated together with Jacobian matrix below
	MatrixXd Jn;

	// Get starting timepoint 
	start = high_resolution_clock::now();

	// Cosserat Rod Model - Solve the Forward Kinematics and calculate the Jacobian
	for (int cnt = 0; cnt < REPS; cnt++)
		Jn = CRM_FKJacobian_Numerical(control_inputs, FKsolution, FKParams, localmin); // evaluate the output vector F and the Jacobian matrix J

	// Get ending timepoint 
	stop = high_resolution_clock::now();
	// Get duration. Substart timepoints to  
	// get duration. To cast it to proper unit 
	// use duration cast method 
	duration = duration_cast<microseconds>(stop - start);
	std::cout << std::endl << "Average time taken by Numerical Jacobian calculation in " << REPS << " repetitions: " << duration.count() / REPS << " microseconds" << std::endl;

	std::cout << "F = \n" << outputs << std::endl;		// print the evaluated output vector
	std::cout << "Jn = \n" << Jn << std::endl;			// print the evaluated Jacobian matrix

	// let's calculate the Jacobian error
	// let's compare the difference between the two solutions
	MatrixXd Ja_z = Ja.leftCols<3 * NUM_ACT_SET + 1>();
	double jwerror = (Jn.middleRows<3>(3)-Ja_z.bottomRows<3>()).norm();
	double jperror = (Jn.topRows<3>()-Ja_z.topRows<3>()).norm();
	// display the results
	std::cout << "||Jn_p-Ja_p||:" << jperror << "  ||Ja_p||: " << Ja.topRows<3>().norm() << "  ||Jn_w - Ja_w||:" << jwerror << std::endl;
	std::cout << "----" << std::endl;

	delete[] ReportedMarkerPos;
	delete[] ReportedCoilPos;
	delete[] ReportedCoilOrient;

	return (localmin);
}



//
// Example showing how to use the CRM_ForwardKinematics functions 
//		to calculate the catheter forward kinematics for the constrained tip case
// This is the preferred method
//
int RunFKContactExample(void) {

	std::cout << "### RunFKContactExample() --- CRM Forward Kinematics Examples... " << std::endl;
	std::cout << std::endl << "Constrained Tip Deflection Example: " << std::endl << std::endl;

	// *** Load parameters from file
	// This step would typically needs to be executed only once
	//   Physical Description of the Catheter
	CRMCatheterModelParams CathParams = Load_CRMCatheterModelParams("../catheterdata/CatheterParameterSet_2.txt");
	//   Catheter Configuration in spatial coordinates
	CatheterConfiguration CathConfig = Load_CatheterConfiguration("../catheterdata/CatheterSpatialConfiguration_1.txt");

	// *** Other External variables
	// specify if catheter is in free space or if the catheter tip is constrained to a contact point
	ContactModeType ContactMode = ContactModeType::FIXED_TIP;
	// External point force (in spatial coordinates) applied at the tip of the catheter (\lambda = 0)  - unit: ??
	//   (this will be used when ContactMode == ContactModeType::FREE_TIP)
	//   not used in the solution since we are in FIXED_TIP mode
	double TipForce[3] = { 0.0, 0.0, 0.0 };
	// The spatial coordinates of the point where the catheter tip is constrained to be 
	//   (this will be used when ContactMode == ContactModeType::FIXED_TIP)
	double TipConstraintPoint[3] = { 6.12, 40.57, 93.96 };

	// *** Control Inputs
	// Inserted Length of the catheter (length of the catheter that is inside the heart chamber) - unit: mm
	double InsertedLength = 104.0;
	// Actuation currents for each of the coils for each of the coil sets - unit: A
	double ActuationCurrents[NUM_ACT_SET][3] = { {0.100, 0.100, 0.100}, {0.100, 0.100, 0.100} }; // { {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0} }; 

	// *** Numerical Computation Parameters
	// Stepsize used in numerical integration along the length of the catheter during IVP - unit: mm
	double IntegrationStepSize = 0.2;

	// *** Storage for storing localization marker positions and actuation coil orientations
	double (*ReportedMarkerPos)[3] = new double [CathParams.no_locmarkers][3];
	double (*ReportedCoilPos)[3] = new double[CathParams.no_act_set][3];
	double (*ReportedCoilOrient)[9] = new double[CathParams.no_act_set][9];


	// Define initial guesses to be used when solving boundary value problem
	// initial guess for the curvature at the catheter base
	double deltau0_initialguess[3] = { 0.0, 0.0, 0.0 };
	// initial guess for the contstraint force at the catheter tip (this will be used when ContactMode == ContactModeType::FIXED_TIP)
	double ftip_initialguess[3] = { 0.0, 0.0, 0.0 };

	// numerical nonlinear equation solver diagnostic outputs
	int localmin;

	//
	// We will use the new Forward Kinematics Function
	//
	// The Forward Kinematics Function uses a different parameter structure than BVP
	CRMForwardKinematicsData FKParams;
	FKParams.CathConfig = &CathConfig;
	FKParams.CathParams = &CathParams;
	FKParams.ContactMode = ContactMode;
	FKParams.FinalValueOnly = false;  // we want the localization coil locations, too
	FKParams.ReportedMarkerPos = ReportedMarkerPos;
	FKParams.ReportedCoilPos = ReportedCoilPos;
	FKParams.ReportedCoilOrient = ReportedCoilOrient;
	for (int i = 0; i < 3; i++) FKParams.TipConstraintPoint[i] = TipConstraintPoint[i];
	for (int i = 0; i < 3; i++) FKParams.TipForce[i] = TipForce[i];
	for (int i = 0; i < 3; i++) FKParams.deltau0_initialguess[i] = deltau0_initialguess[i];
	for (int i = 0; i < 3; i++) FKParams.ftip_initialguess[i] = ftip_initialguess[i];
	FKParams.IntegrationStepSize = IntegrationStepSize;
	// inputs
	VectorXd control_inputs(NUM_ACT_SET * 3 + 1); // actuator currents (distal to proximal) followed by inserted length
	for (int i = 0; i < NUM_ACT_SET; i++)  for (int j = 0; j < 3; j++) control_inputs(i * 3 + j) = ActuationCurrents[i][j];
	control_inputs(NUM_ACT_SET * 3) = InsertedLength;
	// and outputs
	int outputdim;
	if (ContactMode == ContactModeType::FREE_TIP) outputdim = 15; else outputdim = 18;
	VectorXd FKsolution(outputdim); // p[0..2],R[0..8],deltau0[0..2](,ftip[0..2])  R: in row major order
	double   PotentialEnergy;

	// Multiple repetitions to more reliably measure time
	int REPS = 100;
	// Get starting timepoint 
	auto start = high_resolution_clock::now();

	// Cosserat Rod Model - Solve the Forward Kinematics for the Constrained Tip Case
	for (int cnt = 0; cnt < REPS; cnt++)
		FKsolution = CRM_ForwardKinematics(control_inputs, FKParams, PotentialEnergy, localmin);

	// Get ending timepoint 
	auto stop = high_resolution_clock::now();
	// Get duration. Substart timepoints to  
	// get duration. To cast it to proper unit 
	// use duration cast method 
	auto duration = duration_cast<microseconds>(stop - start);
	std::cout << std::endl << "Average time taken by Constrained Tip FK Solution in " << REPS << " repetitions: " << duration.count() / REPS << " microseconds" << std::endl << std::endl;

	//display the results,
	std::cout << "FK Output -- p,R,deltau0,ftip: \n" << FKsolution.transpose() << "\n";
	std::cout << "PE:" << PotentialEnergy << std::endl;
	std::cout << "localmin:" << localmin << std::endl;
	std::cout << "----" << std::endl;

	MatrixXd MPos = Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(&(FKParams.ReportedMarkerPos[0][0]), CathParams.no_locmarkers, 3);
	MatrixXd C0Pos = Eigen::Map<Eigen::Vector<double, 3>>(FKParams.ReportedCoilPos[0]);
	MatrixXd C0Rot = Eigen::Map<Eigen::Matrix<double, 3, 3, Eigen::RowMajor>>(FKParams.ReportedCoilOrient[0]);
	MatrixXd C1Pos = Eigen::Map<Eigen::Vector<double, 3>>(FKParams.ReportedCoilPos[1]);
	MatrixXd C1Rot = Eigen::Map<Eigen::Matrix<double, 3, 3, Eigen::RowMajor>>(FKParams.ReportedCoilOrient[1]);

	std::cout << "Localization Marker Positions" << std::endl << MPos << std::endl;
	std::cout << "Coil 0 Position and Orientation" << std::endl << C0Pos << std::endl << C0Rot << std::endl <<
		"Coil 1 Position and Orientation" << std::endl << C1Pos << std::endl << C1Rot << std::endl;
	std::cout << "----" << std::endl;

	//
	// Jacobian calculation
	// 

	// First, we will use the analytical Jacobian calculation using CRMSolverIVPJacobian
	MatrixXd Ja;

	// Get starting timepoint 
	start = high_resolution_clock::now();

	// Cosserat Rod Model - calculate the Jacobian using the solution of the Forward Kinematics
	for (int cnt = 0; cnt < REPS; cnt++)
		Ja = CRM_FKJacobian_Analytical(control_inputs, FKsolution, FKParams); // evaluate the Jacobian matrix J

	// Get ending timepoint 
	stop = high_resolution_clock::now();
	// Get duration. Substart timepoints to  
	// get duration. To cast it to proper unit 
	// use duration cast method 
	duration = duration_cast<microseconds>(stop - start);
	std::cout << std::endl << "Average time taken by analytical contact Jacobian calculation in " << REPS << " repetitions: " << duration.count() / REPS << " microseconds" << std::endl;

	std::cout << "Ja = \n" << Ja << std::endl;			// print the evaluated Jacobian matrix

	//
	// let's compare it against numerical Jacobian calculation
	VectorXd outputs;								// the output vector evaluated together with Jacobian matrix below
	MatrixXd Jn;

	// Get starting timepoint 
	start = high_resolution_clock::now();

	// Cosserat Rod Model - Solve the Forward Kinematics and calculate the Jacobian
	for (int cnt = 0; cnt < REPS; cnt++)
		Jn = CRM_FKJacobian_Numerical(control_inputs, outputs, FKParams, localmin); // evaluate the output vector F and the Jacobian matrix J

	// Get ending timepoint 
	stop = high_resolution_clock::now();
	// Get duration. Substart timepoints to  
	// get duration. To cast it to proper unit 
	// use duration cast method 
	duration = duration_cast<microseconds>(stop - start);
	std::cout << std::endl << "Average time taken by numerical contact Jacobian calculation in " << REPS << " repetitions: " << duration.count() / REPS << " microseconds" << std::endl;

	std::cout << "F = \n" << outputs << std::endl;		// print the evaluated output vector
	std::cout << "Jn = \n" << Jn << std::endl;			// print the evaluated Jacobian matrix

	//
	// Let's also compare it to the brute force calculation
	MatrixXd Jbf;

	// Get starting timepoint 
	start = high_resolution_clock::now();

	// Cosserat Rod Model - Solve the Forward Kinematics and calculate the Jacobian
	for (int cnt = 0; cnt < REPS; cnt++)
		Jbf = CRM_FKJacobian_BruteForce(control_inputs, outputs, FKParams, localmin); // evaluate the output vector F and the Jacobian matrix J

	// Get ending timepoint 
	stop = high_resolution_clock::now();
	// Get duration. Substart timepoints to  
	// get duration. To cast it to proper unit 
	// use duration cast method 
	duration = duration_cast<microseconds>(stop - start);
	std::cout << std::endl << "Average time taken by the brute force numerical contact Jacobian calculation in " << REPS << " repetitions: " << duration.count() / REPS << " microseconds" << std::endl;

	std::cout << "F = \n" << outputs << std::endl;		// print the evaluated output vector
	std::cout << "Jbf = \n" << Jbf << std::endl;			// print the evaluated Jacobian matrix
	std::cout << "Ja-Jbf_ftip = \n" << (Ja - Jbf.bottomRows(3)) << std::endl;			// print the evaluated Jacobian matrix

	// let's compare the difference between the two solutions
	double jerror = (Jbf.bottomRows(3) - Ja).norm();
	double jperror = (Jbf.topRows(3)).norm();
	// display the results
	std::cout << "||Ja-Jbf_ftip||:" << jerror << "  ||J||: " << Ja.norm() << "  ||Jbf_p||:" << jperror << std::endl;
	std::cout << "----" << std::endl;

	delete[] ReportedMarkerPos;
	delete[] ReportedCoilPos;
	delete[] ReportedCoilOrient;

	return (localmin);
}



//
// Example showing how to use the CRMShootingMethodBVP and CRMSolverIVP functions
//		to calculate the catheter forward kinematics
//
int RunExample(void) {

	std::cout << "### RunExample() --- CRM Forward Kinematics Examples... " << std::endl;
	std::cout << std::endl << "Free Space Deflection Example: " << std::endl << std::endl;

	// *** Load parameters from file
	// This step would typically needs to be executed only once
	//   Physical Description of the Catheter
	CRMCatheterModelParams CathParams = Load_CRMCatheterModelParams("../catheterdata/CatheterParameterSet_2.txt");
	//   Catheter Configuration in spatial coordinates
	CatheterConfiguration CathConfig = Load_CatheterConfiguration("../catheterdata/CatheterSpatialConfiguration_1.txt");

	// *** Other External variables
	// specify if catheter is in free space or if the catheter tip is constrained to a contact point
	ContactModeType ContactMode = ContactModeType::FREE_TIP;
	// External point force (in spatial coordinates) applied at the tip of the catheter (\lambda = 0)  - unit: ??
	//   (this will be used when ContactMode == ContactModeType::FREE_TIP)
	double TipForce[3] = { 0.0, 0.0, 0.0 };
	// The spatial coordinates of the point where the catheter tip is constrained to be 
	//   (this will be used when ContactMode == ContactModeType::FIXED_TIP)
	double TipConstraintPoint[3] = { 0.0, 0.0, 0.0 };

	// *** Control Inputs
	// Inserted Length of the catheter (length of the catheter that is inside the heart chamber) - unit: mm
	double InsertedLength = 104.0;
	// Actuation currents for each of the coils for each of the coil sets - unit: A
	double ActuationCurrents[NUM_ACT_SET][3] = { {0.100, 0.100, 0.100}, {0.100, 0.100, 0.100} };

	// *** Numerical Computation Parameters
	// Stepsize used in numerical integration along the length of the catehter during IVP - unit: mm
	double IntegrationStepSize = 0.2;

	// Define initial guesses to be used when solving boundary value problem
	// initial guess for the delta_curvature at the catheter base ( u0 = deltau0 + ustar0 ) 
	double deltau0_initialguess[3] = { 0.0, 0.0, 0.0 };
	// initial guess for the contstraint force at the catheter tip (this will be used when ContactMode == ContactModeType::FIXED_TIP)
	double ftip_initialguess[3] = { 0.0, 0.0, 0.0 };

	// Declare the output variables for BVP
	// calculated curvature at the catheter base
	double deltau0_calc[3];
	// calculated contstraint force at the catheter tip (this will be used when ContactMode == ContactModeType::FIXED_TIP)
	double ftip_calc[3];
	// numerical nonlinear equation solver diagnostic outputs
	int localmin;

	// Define the variable of convenience used to package the arguments passed to BVP Solver
	// Populate the variable of convenience for conveniently passing lots of arguments to the BVP Solver
	//   this step would typicall need to be executed every time BVP needs to be calculated as actuation variables would change at every time step
	CRMShootingMethodParams BVPParams =
		CRMConstructShootingMethodParamSet(CathParams, CathConfig, InsertedLength, ActuationCurrents, ContactMode, TipConstraintPoint, TipForce, IntegrationStepSize);

	// Cosserat Rod Model - Solve the Boundary Value Problem to calculate the equilibrium configuration of the catheter 
	CRMShootingMethodBVP(BVPParams, deltau0_initialguess, ftip_initialguess, deltau0_calc, ftip_calc, localmin);

	// Declare input variables for IVP
	// catheter shape state at the entry point of the catheter
	//   states are packed p[0..2], R[0..8], u[0..2](R: 3x3 matrix stored in row major order R11 R12 R13 R21 R22 R23 R31 R32 R33)
	double x0[NUM_STATES];
	for (int i = 0; i < 3; i++) x0[i] = CathConfig.p0[i];
	for (int i = 0; i < 9; i++) x0[i + 3] = CathConfig.R0[i];
	for (int i = 0; i < 3; i++) x0[i + 3 + 9] = deltau0_calc[i];

	// Declare output variables
	// catheter shape state at the entry point of the catheter
	//   states are packed p[0..2], R[0..8], u[0..2](R: 3x3 matrix stored in row major order R11 R12 R13 R21 R22 R23 R31 R32 R33)
	double xf[NUM_STATES];
	// moment residual at the catheter tip - this should converge to {0,0,0} if the catheter is at its equilibrium configuration
	double residual[3];
	// calculated potential energy of the catheter
	double PotentialEnergy;
	// spatial coordinates of the localization markers
	double(*ReportedMarkerPos)[3] = new double[CathParams.no_locmarkers][3];
	// spatial position and orientations of the actuation coils
	double(*CoilPositions)[3] = new double[CathParams.no_act_set][3];
	double(*CoilOrientations)[9] = new double[CathParams.no_act_set][9];

	// Cosserat Rod Model - Solve the Initial Value Problem to calculate the shape of the catheter 
	CRMSolverIVP(BVPParams, deltau0_calc, ftip_calc, true, false, xf, residual, PotentialEnergy, ReportedMarkerPos, CoilPositions, CoilOrientations);

	// Print outputs
	std::cout << "localmin  " << localmin << std::endl;
	printMatrix(deltau0_calc, 1, 3, "Calculated delta curvature at base");
	double u0calc[3];
	mAdd_AB<3, 1>(deltau0_calc, CathParams.ustar[0], u0calc);
	printMatrix(u0calc, 1, 3, "Calculated curvature at base");
	printMatrix(residual, 1, 3, "Moment residual at tip");
	printMatrix(ftip_calc, 1, 3, "Calculated Tip Force");
	printMatrix(&(xf[0]), 1, 3, "Catheter Tip Position");
	printMatrix(&(xf[3]), 1, 9, "Catheter Tip Orientation");
	std::cout << "PE:" << PotentialEnergy << std::endl;
	std::cout << "----" << std::endl;

	// Cosserat Rod Model - Solve the Initial Value Problem to calculate the IVP Jacobian 
	//MatrixXd JBVP_p_z, JBVP_ws_z, Jft_z;

	auto [JBVP_p_z, JBVP_ws_z, JBVP_p_ft, JBVP_ws_ft, Jft_z] = CRMSolverIVPJacobian(BVPParams, deltau0_calc, ftip_calc, false, xf, residual);

	std::cout << "JBVP_p_z = " << std::endl << JBVP_p_z << std::endl;
	std::cout << "JBVP_ws_z = " << std::endl << JBVP_ws_z << std::endl;
	std::cout << "JBVP_p_ft = " << std::endl << JBVP_p_ft << std::endl;
	std::cout << "JBVP_ws_ft = " << std::endl << JBVP_ws_ft << std::endl;
	std::cout << "Jft_z = " << std::endl << Jft_z << std::endl;

	std::cout << "----" << std::endl;

	delete[] ReportedMarkerPos;
	delete[] CoilPositions;
	delete[] CoilOrientations;

	return (localmin);
}


int RunTests (void) {

	int FailFlag = 0;

	std::cout << "######## CRM Tools Test Results " << std::endl;

	//
	//  Test code for CRMSolverIVP  (Solver for Initial Value Problem)
	//
	std::cout << "### CRMSolverIVP Test: " << std::endl;

	// set up the test problem inputs
	double IntegrationStepSize = 0.2;
	double InsertedLength = 104.02;
	double B0[3] = { 0.0, 3.0, 0.0 };			// B0 field vector of the MRI scanner (in spatial coordinates)
	double g[3] = { 0.0, 0.0, 9.81 };							// gravity vector (in spatial coordinates)
	double x0[NUM_STATES] = { 0.0,0.0,0.0,   1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0,   0.0, 0.0, 0.0 };
	bool CalculateEnergy = true;
	bool FinalStepOnly = false;
	const int32_t Nc = 3;				// Number of flexible catheter segments
	const int32_t Nr = 2; 				// Number of rigid segments (including actuators)
	const int32_t Na = 2; 				// Number of actuator sets
	const int32_t Ns = 5;  				// NumSegments: Ns = 2 * Na + 1   // derived quantity
	const int32_t Nm = 5;							// Number of Markers
	const int32_t Nfc = 104;						// Number of subsegments used in calculation of the fcumIVP
	//	For all parameters below, segments and actuator units are numbered/ordered from the tip of the catheter towards the base
	CatheterSegmentType SegTypes[Ns] = { CatheterSegmentType::FLEXIBLE, CatheterSegmentType::RIGID_WITH_ACTUATOR, CatheterSegmentType::FLEXIBLE, CatheterSegmentType::RIGID_WITH_ACTUATOR, CatheterSegmentType::FLEXIBLE };
	// Array of lambda values for segment endpoints; Ns long array
	double SegEnds[Ns] = { 10.72, 26.86, 42.255, 57.865, 104.02 };
	// Array of lambda values for marker locations
	double MarkerLoc[Nm] = { 2.18, 18.79, 50.06, 101.52, 104.02 };
	// Catheter Length Density Vector
	double rho[Ns] = { 2.8814e-7, 2.8814e-7, 2.8814e-7, 2.8814e-7, 2.8814e-7 };
	// Catheter Rigidity Matrix; (Na+1)*9 long array, (Na+1) 3x3 matrices stored in row major order
	double Klist[Nc][9] = { {22.8304161859862, 0.0, 0.0, 0.0, 22.8304161859862, 0.0, 0.0, 0.0, 20.2125442625320}, {22.8304161859862, 0.0, 0.0, 0.0, 22.8304161859862, 0.0, 0.0, 0.0, 20.2125442625320}, {22.8304161859862, 0.0, 0.0, 0.0, 22.8304161859862, 0.0, 0.0, 0.0, 20.2125442625320} };
	// Inverses of K matrices; (Na+1)*9 long array, (Na+1) 3x3 matrices stored in row major order  // derived quantity
	double Kinvlist[Nc][9] = { {0.0438012164059375, 0.0, 0.0, 0.0, 0.0438012164059375, 0.0, 0.0, 0.0, 0.0494742268470230}, {0.0438012164059375, 0.0, 0.0, 0.0, 0.0438012164059375, 0.0, 0.0, 0.0, 0.0494742268470230}, {0.0438012164059375, 0.0, 0.0, 0.0, 0.0438012164059375, 0.0, 0.0, 0.0, 0.0494742268470230} };
	// Local curvature in unloaded configuration for each of the flexible segments; (Na+1)*3 long array, (Na+1) 3x1 vectors
	double ustarlist[Nc][3] = { 0.000127537459505030, -0.000173238845806410, 0.0, 0.00566574659169662, -0.00822266255995812, 0.0, -0.00250030480901699, 0.00188434148765547, 0.0 }; //{-0.0025, 0.0019, 0.0, 0.0057, -0.0082, 0.0, 0.00013, -0.00017, 0.0};
	// Actuator Mass vector
	double ActMass[Na] = { 7.7736e-5, 8.0157e-5 };
	// The product of CoilAlignmentMatrix and TurnAreaMatrix
	double CoilAlignmentTurnAreaMatrix[2][9] = { { 1.44, 0.0, 0.0, 0.0, 1.3851, 0.0, 0.0, 0.0, 1.60 } , { 1.65, 0.0, 0.0, 0.0, 1.326, 0.0, 0.0, 0.0, 1.56 } };
	// Actuator magnetization moments; Na*3 long array, Na 3x1 vectors; MagMoment = CoilAlignMat * CoilTurnAreaMat * ActuationCurrentVector
	double MagMomentlist[Na][3] = { 0.0, 0.0, 0.1600, 0.0, 0.0, 0.0 }; 
	// Reciprocal of \Delta s (= \Delta \lambda) used in discretizing fcum  ( deltasinv = 1 / (L/N) = N/L)
	double dlambdainv = 1.0;
	double FullLength = SegEnds[Ns - 1];
	// 3*(N+1) by 1 array (grouped by 3 doubles) storing cumulative external force integrated from \lambda = index * \Delta\lambda to the catheter tip (\lambda=0)
	double fcumIVP[Nfc + 1][3] = {};
	double ftip[3] = { 0.0, 0.0, 0.0 };

	// declare the output variables
	double xf[NUM_STATES];
	double residual[3];
	double PotentialEnergy;
	double ReportedMarkerPos[Nm][3];
	// spatial positions and orientations of the actuation coils
	double ReportedCoilPositions[Na][3];
	double ReportedCoilOrientations[Na][9];

	// Cosserat Rod Model - Integrator for Solving the Initial Value Problem
	CRMSolverIVP(Nc, Nr, Na, Nm, Nfc, x0, IntegrationStepSize, InsertedLength, dlambdainv, SegTypes, SegEnds, MarkerLoc, rho, Klist, Kinvlist, ustarlist, ActMass, CoilAlignmentTurnAreaMatrix, MagMomentlist, fcumIVP, ftip, B0, g, CalculateEnergy, FinalStepOnly, xf, residual, PotentialEnergy, ReportedMarkerPos, ReportedCoilPositions, ReportedCoilOrientations);

	// compare the results to the reference values
	double ExpectedStates[NUM_STATES] = { -5.497054669396932, -5.570783408417309, 103.0917629459139, 0.982564773234176, 0.029114762388711, -0.183626784558004, -0.094107711560958, 0.929675871807347, -0.356155178150316, 0.160344017817671, 0.367226228689942, 0.916206686783898, 0.0225, -0.0024, -0.0074 }; //  ???f, ???f, ???f,  ???f, ???f, ???f, ???f, ???f, ???f, ???f, ???f, ???f,   ???f, ???f, ???f };
	double ExpectedMarkerPos[Nm][3] = { {-5.1038, -4.8439, 101.0747}, {-2.4420, -1.9709, 84.9666}, {0.0000, 0.0000, 53.9600}, {0.0000, 0.0000, 2.5000}, {0, 0, 0} };
	double errorv2[NUM_STATES], ssqerrorv2;
	double errorv3[Nm][3], ssqerrorv3;

	ssqerrorv2 = 0.0;
	for (int i = 0; i < NUM_STATES; i++) ssqerrorv2 += pow((errorv2[i] = ExpectedStates[i] - xf[i]), 2);
	ssqerrorv3 = 0.0;
	for (int i = 0; i < Nm; i++)
		for (int j = 0; j < 3; j++)
			ssqerrorv3 += pow((errorv3[i][j] = ExpectedMarkerPos[i][j] - ReportedMarkerPos[i][j]), 2);


	printMatrix(ExpectedStates, 1, NUM_STATES, "ExpectedStates");
	printMatrix(xf, 1, NUM_STATES, "xf");
	printMatrix(errorv2, 1, NUM_STATES, "errorv2");
	std::cout << "Error Norm:" << sqrt(ssqerrorv2) << std::endl;
	std::cout << "----" << std::endl;
	printMatrix(&(ExpectedMarkerPos[0][0]), Nm, 3, "ExpectedMarkerPos");
	printMatrix(&(ReportedMarkerPos[0][0]), Nm, 3, "ReportedMarkerPos");
	printMatrix(&(errorv3[0][0]), Nm, 3, "errorv3");
	std::cout << "Error Norm:" << sqrt(ssqerrorv3) << std::endl;
	std::cout << "PE:" << PotentialEnergy << std::endl;
	std::cout << "----" << std::endl;

	if ((sqrt(ssqerrorv2) < 0.2) && (sqrt(ssqerrorv3) < 0.2)) {
		std::cout << "Test passed for Inserted Length =" << InsertedLength << std::endl;
		std::cout << "###" << std::endl;
	}
	else {
		std::cout << "TEST FAIL: " << "Results do not match!" << std::endl;
		std::cout << "###" << std::endl;
		FailFlag++;
	}


	//
	//  Test code for CRMShootingMethodBVP (Shooting method for solving the boundary value problem)
	//

	std::cout << "### CRMShootingMethodBVP Test: " << std::endl;
	std::cout << std::endl << "Free Space Deflection Test: " << std::endl;

	// *** Load parameters from file
	// This step would typically needs to be executed only once
	//   Physical Description of the Catheter
	CRMCatheterModelParams CathParams = Load_CRMCatheterModelParams("../catheterdata/CatheterParameterSet_2.txt");
	//   Catheter Configuration in spatial coordinates
	CatheterConfiguration CathConfig = Load_CatheterConfiguration("../catheterdata/CatheterSpatialConfiguration_1.txt");

	double InLength = 104.0;
	double ActuationCurrents[NUM_ACT_SET][3] = { {0.1, 0.1, 0.1}, {0.1, 0.1, 0.1} };
	ContactModeType ContactMode = ContactModeType::FREE_TIP;
	double TipConstraintPoint[3] = { 0.0, 0.0, 0.0 };
	double TipForce[3] = { 0.0, 0.0, 0.0 };

	// setup the input parameters
	CRMShootingMethodParams BVPParams = 
		CRMConstructShootingMethodParamSet(CathParams, CathConfig, InLength, ActuationCurrents, ContactMode, TipConstraintPoint, TipForce, IntegrationStepSize);

	double deltau0_initialguess[3]={0.0, 0.0, 0.0};
	double ftip_initialguess[3] = { 0.0, 0.0, 0.0 };

	// declare the output variables
    double deltau0_calc[3];
	double ftip_calc[3];
	int localmin;
	double residualReal[3], residualref[3], residualinitial[3];
	double (*RefMarkerPos)[3] = new double [CathParams.no_locmarkers][3];
	double (*InitialMarkerPos)[3] = new double [CathParams.no_locmarkers][3];
	double RefCoilPositions[Na][3];
	double RefCoilOrientations[Na][9];
	double InitialCoilPositions[Na][3];
	double InitialCoilOrientations[Na][9];

	int REPS = 100;

	// Get starting timepoint 
	auto start = high_resolution_clock::now();

    // Cosserat Rod Model - Solve the Boundary Value Problem to calculate the equilibrium configuration of the catheter 
	for (int cnt=0; cnt<REPS; cnt++)
		CRMShootingMethodBVP (	BVPParams, deltau0_initialguess, ftip_initialguess, deltau0_calc, ftip_calc, localmin	);

	// Get ending timepoint 
	auto stop = high_resolution_clock::now();

	// Get duration. Substart timepoints to  
	// get duration. To cast it to proper unit 
	// use duration cast method 
	auto duration = duration_cast<microseconds>(stop - start);

	std::cout << std::endl << "Average time taken by BVP Solution in " << REPS << " repetitions: " << duration.count()/REPS << " microseconds" << std::endl;

    double error3[3], expectedu0[3]={0.0, 0.0, 0.0};
	double xfReal[NUM_STATES];
	double deltau0_ref[3] = { 0.0, 0.0, 0.0 };
	double ReportedPE, InitialPE, RefPE;

	CRMSolverIVP(BVPParams, deltau0_calc, ftip_calc, true, false, xfReal, residualReal, ReportedPE, ReportedMarkerPos, ReportedCoilPositions, ReportedCoilOrientations );

	CRMSolverIVP(BVPParams, deltau0_initialguess, ftip_calc, true, false, xfReal, residualinitial, InitialPE, InitialMarkerPos, InitialCoilPositions, InitialCoilOrientations );

	for(int i=0; i<3; i++) deltau0_ref[i]=-ustarlist[0][i];
	CRMSolverIVP(BVPParams, deltau0_ref, ftip_calc, true, false, xfReal, residualref, RefPE, RefMarkerPos, RefCoilPositions, RefCoilOrientations );


	std::cout << "localmin  " << localmin << std::endl;
	double u0calc[3];
	mAdd_AB<3, 1>(deltau0_calc, ustarlist[0], u0calc);
	mSub_AB<3,1>(u0calc, expectedu0, error3 );

	//printMatrix(expectedu0,1,3,"Expected u0");
	printMatrix(u0calc,1,3,"Calculated u0");
	//printMatrix(error3,1,3,"error");
	printMatrix(residualReal,1,3,"residual");
	printMatrix(residualinitial,1,3,"residualinitial");
	printMatrix(residualref,1,3,"residualref");
	//std::cout << "Error Norm:" << sqrt(vNormSq<3>(error3)) << std::endl;
	std::cout << "Residual Norm:" << sqrt(vNormSq<3>(residualReal)) << std::endl;
	std::cout << "----" << std::endl;
	std::cout << "PE: Reported ; Initial ; Ref " << ReportedPE << " ; " << InitialPE << " ; " << RefPE << std::endl;
	std::cout << "----" << std::endl;
	printMatrix(&(ReportedMarkerPos[0][0]), CathParams.no_locmarkers, 3, "ReportedMarkerPos");
	std::cout << "----" << std::endl;

	delete[] RefMarkerPos;
	delete[] InitialMarkerPos;


	if (!localmin && (sqrt(vNormSq<3>(residualReal)) < 1e-3)) {
		std::cout << "Test passed for Inserted Length =" << InsertedLength << std::endl;
		std::cout << "###" << std::endl;
	}
	else {
		std::cout << "TEST FAIL: " << "Results do not match!" << std::endl;
		std::cout << "###" << std::endl;
		FailFlag++;
	}


	//
	//  Test code for CRMShootingMethodBVP under tip constraint
	//
	FailFlag += ConstrainedTipTest();	


	if (FailFlag==0) {
		std::cout << std::endl << std::endl << "###" << std::endl;
		std::cout << "ALL TESTS PASSED!..." << std::endl;
		std::cout << "###" << std::endl;
	}
	else {
		std::cout << std::endl << std::endl << "###" << std::endl;
		std::cout << FailFlag << " TEST(S) FAILED!..." << std::endl;
		std::cout << "###" << std::endl;
	}
	return (FailFlag);


}



//
// Code to test constrained tip calculations
//    uses the CRM_ForwardKinematics functions to calculate the catheter forward kinematics
//
int ConstrainedTipTest(void) {

	std::cout << std::endl << "Constrained Tip Deflection Test: " << std::endl << std::endl;
	int localmin = 0;
	int FailFlag = 0;

	// *** Load parameters from file
	// This step would typically needs to be executed only once
	//   Physical Description of the Catheter
	CRMCatheterModelParams CathParams = Load_CRMCatheterModelParams("../catheterdata/CatheterParameterSet_2.txt");
	//   Catheter Configuration in spatial coordinates
	CatheterConfiguration CathConfig = Load_CatheterConfiguration("../catheterdata/CatheterSpatialConfiguration_1.txt");

	// *** Numerical Computation Parameters
	// Stepsize used in numerical integration along the length of the catheter during IVP - unit: mm
	double IntegrationStepSize = 0.2;

	// *** Control Inputs
	// Inserted Length of the catheter (length of the catheter that is inside the heart chamber) - unit: mm
	double InsertedLength = 104.0;
	// Actuation currents for each of the coils for each of the coil sets - unit: A
	double ActuationCurrents[NUM_ACT_SET][3] = { {0.100, 0.100, 0.100}, {0.100, 0.100, 0.100} }; // { {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0} }; 
	// The Forward Kinematics Function uses a different parameter structure than BVP for inputs
	double control_inputs[NUM_ACT_SET * 3 + 1];  // actuator currents (distal to proximal) followed by inserted length
	for (int i = 0; i < NUM_ACT_SET; i++)  for (int j = 0; j < 3; j++) control_inputs[i * 3 + j] = ActuationCurrents[i][j];
	control_inputs[NUM_ACT_SET * 3] = InsertedLength;

	// *** Storage for storing localization marker positions and coil orientations
	double (*ReportedMarkerPos)[3] = new double [CathParams.no_locmarkers][3];
	double (*ReportedCoilPos)[3] = new double[CathParams.no_act_set][3];
	double (*ReportedCoilOrient)[9] = new double[CathParams.no_act_set][9];

	// STEP 1 - We will apply a test tip force in the free-tip mode to find the equilibrium configuration of the catheter,
	//      which will be then used as the constraint tip position to see if we can get this test force back

	// *** Other External variables
	// specify if catheter is in free space or if the catheter tip is constrained to a contact point
	ContactModeType ContactMode_Test = ContactModeType::FREE_TIP;
	// External point force (in spatial coordinates) applied at the tip of the catheter (\lambda = 0)  - unit: ??
	//   (this will be used when ContactMode == ContactModeType::FREE_TIP)
	double TipForce_Test[3] = { 0.002, 0.0, 0.0 };
	// The spatial coordinates of the point where the catheter tip is constrained to be 
	//   (this will be used when ContactMode == ContactModeType::FIXED_TIP)
	//   not used in the test
	double TipConstraintPoint_Test[3] = { 0.0, 0.0, 0.0 };

	// Define initial guesses to be used when solving boundary value problem
	// initial guess for the curvature at the catheter base
	double deltau0_initialguess_Test[3] = { 0.0, 0.0, 0.0 };
	// initial guess for the contstraint force at the catheter tip (this will be used when ContactMode == ContactModeType::FIXED_TIP)
	//   not used in the test
	double ftip_initialguess_Test[3] = { 0.0, 0.0, 0.0 };

	//
	// We will use the new Forward Kinematics Function
	//

	// The Forward Kinematics Function uses a different parameter structure than BVP
	CRMForwardKinematicsData FKParams_Test;
	FKParams_Test.CathConfig = &CathConfig;
	FKParams_Test.CathParams = &CathParams;
	FKParams_Test.ContactMode = ContactMode_Test;
	FKParams_Test.FinalValueOnly = false;  // we want the localization coil locations, too
	FKParams_Test.ReportedMarkerPos = ReportedMarkerPos;
	FKParams_Test.ReportedCoilPos = ReportedCoilPos;
	FKParams_Test.ReportedCoilOrient = ReportedCoilOrient;
	for (int i = 0; i < 3; i++) FKParams_Test.TipConstraintPoint[i] = TipConstraintPoint_Test[i];
	for (int i = 0; i < 3; i++) FKParams_Test.TipForce[i] = TipForce_Test[i];
	for (int i = 0; i < 3; i++) FKParams_Test.deltau0_initialguess[i] = deltau0_initialguess_Test[i];
	for (int i = 0; i < 3; i++) FKParams_Test.ftip_initialguess[i] = ftip_initialguess_Test[i];
	FKParams_Test.IntegrationStepSize = IntegrationStepSize;

	// allocate memory for output variables
	double output_values_Test[15]; // 18 for FIXED_TIP // p[0..2],R[0..8],u0[0..2](,ftip[0..2])  R: in row major order
	double   PotentialEnergy;

	// side calculation - for zero tip force FREE_TIP
	for (int i = 0; i < 3; i++) FKParams_Test.TipForce[i] = 0.0;
	double output_values_ZeroTipForce[18]; // 18 for FIXED_TIP // p[0..2],R[0..8],u0[0..2](,ftip[0..2])  R: in row major order
	// Cosserat Rod Model - Solve the Forward Kinematics 
	localmin = CRM_ForwardKinematics(control_inputs, output_values_ZeroTipForce, PotentialEnergy, FKParams_Test);
	for (int i = 0; i < 3; i++) output_values_ZeroTipForce[i + 15] = 0.0;
	// go back to actual test tip force
	for (int i = 0; i < 3; i++) FKParams_Test.TipForce[i] = TipForce_Test[i];
	printMatrix(output_values_ZeroTipForce, 1, 18, "FK Output for the zero tip force case & ftip-- p,R,u0,ftip");
	std::cout << "PE:" << PotentialEnergy << std::endl;
	std::cout << "localmin:" << localmin << std::endl;
	std::cout << "----" << std::endl;

	// Cosserat Rod Model - Solve the Forward Kinematics 
	localmin = CRM_ForwardKinematics(control_inputs, output_values_Test, PotentialEnergy, FKParams_Test);

	// display the results, together with tip force
	double output_values_Test_augmented[18];
	for (int i = 0; i < 15; i++) output_values_Test_augmented[i] = output_values_Test[i];
	for (int i = 0; i < 3; i++) output_values_Test_augmented[i + 15] = TipForce_Test[i];
	printMatrix(output_values_Test_augmented, 1, 18, "FK Output for the test case & ftip-- p,R,u0,ftip");
	std::cout << "PE:" << PotentialEnergy << std::endl;
	std::cout << "localmin:" << localmin << std::endl;
	std::cout << "----" << std::endl;

	// STEP 2 - We will use the tip position calculated in STEP 1 as the tip constraint point
	//      and solve for the Forward Kinematics of the catheter using FIXED_TIP contact mode

	// *** Other External variables
	// specify if catheter is in free space or if the catheter tip is constrained to a contact point
	ContactModeType ContactMode_Solve = ContactModeType::FIXED_TIP;
	// External point force (in spatial coordinates) applied at the tip of the catheter (\lambda = 0)  - unit: ??
	//   (this will be used when ContactMode == ContactModeType::FREE_TIP)
	//   not used in the solution since we are in FIXED_TIP mode
	double TipForce_Solve[3] = { 0.0, 0.0, 0.0 };
	// The spatial coordinates of the point where the catheter tip is constrained to be 
	//   (this will be used when ContactMode == ContactModeType::FIXED_TIP)
	//   we will use the tip position calculated in STEP 1 
	double TipConstraintPoint_Solve[3]; for (int i = 0; i < 3; i++) TipConstraintPoint_Solve[i] = output_values_Test[i];

	// Define initial guesses to be used when solving boundary value problem
	// initial guess for the curvature at the catheter base
	double deltau0_initialguess_Solve[3] = { 0.0, 0.0, 0.0 };
	// initial guess for the contstraint force at the catheter tip (this will be used when ContactMode == ContactModeType::FIXED_TIP)
	double ftip_initialguess_Solve[3] = { 0.0, 0.0, 0.0 };

	// let's see if the algorithm converges when we give the actual answer
	//mCopy_AB<3>(&(output_values_Test_augmented[12]), u0_initialguess_Solve);
	//mCopy_AB<3>(&(output_values_Test_augmented[15]), ftip_initialguess_Solve);

	//
	// We will use the new Forward Kinematics Function
	//

	// The Forward Kinematics Function uses a different parameter structure than BVP
	CRMForwardKinematicsData FKParams_Solve;
	FKParams_Solve.CathConfig = &CathConfig;
	FKParams_Solve.CathParams = &CathParams;
	FKParams_Solve.ContactMode = ContactMode_Solve;
	FKParams_Solve.FinalValueOnly = false;  // we want the localization coil locations, too
	FKParams_Solve.ReportedMarkerPos = ReportedMarkerPos;
	FKParams_Solve.ReportedCoilPos = ReportedCoilPos;
	FKParams_Solve.ReportedCoilOrient = ReportedCoilOrient;
	for (int i = 0; i < 3; i++) FKParams_Solve.TipConstraintPoint[i] = TipConstraintPoint_Solve[i];
	for (int i = 0; i < 3; i++) FKParams_Solve.TipForce[i] = TipForce_Solve[i];
	for (int i = 0; i < 3; i++) FKParams_Solve.deltau0_initialguess[i] = deltau0_initialguess_Solve[i];
	for (int i = 0; i < 3; i++) FKParams_Solve.ftip_initialguess[i] = ftip_initialguess_Solve[i];
	FKParams_Solve.IntegrationStepSize = IntegrationStepSize;

	// allocate memory for output variables
	double output_values_Solve[18]; // 18 for FIXED_TIP // p[0..2],R[0..8],u0[0..2](,ftip[0..2])  R: in row major order

	// Cosserat Rod Model - Solve the Forward Kinematics for the Constrained Tip Case
	localmin = CRM_ForwardKinematics(control_inputs, output_values_Solve, PotentialEnergy, FKParams_Solve);

	//display the results,
	printMatrix(output_values_Solve, 1, 18, "FK Output for the constrained tip solution-- p,R,u0,ftip");
	std::cout << "PE:" << PotentialEnergy << std::endl;
	std::cout << "localmin:" << localmin << std::endl;
	std::cout << "----" << std::endl;

	// STEP 3 - We will now compare the difference between the two solutions
	double diff[18];
	double perrorv[3], perror;
	double Rerrormat[9], Rerror;
	double uerrorv[3], uerror;
	double ferrorv[3], ferror;
	mSub_AB<18, 1>(output_values_Test_augmented, output_values_Solve, diff);
	mSub_AB<3, 1>(output_values_Test_augmented, output_values_Solve, perrorv);
	perror = sqrt(vNormSq<3>(perrorv));
	mMult_ATB<3, 3, 3>(output_values_Test_augmented + 3, output_values_Solve + 3, Rerrormat);
	Rerror = acos(((Rerrormat[0] + Rerrormat[4] + Rerrormat[8]) - 1.0) / 2.0) * 180.0 / 3.1415;
	mSub_AB<3, 1>(output_values_Test_augmented + 12, output_values_Solve + 12, uerrorv);
	uerror = sqrt(vNormSq<3>(uerrorv));
	mSub_AB<3, 1>(output_values_Test_augmented + 15, output_values_Solve + 15, ferrorv);
	ferror = sqrt(vNormSq<3>(ferrorv));
	// display the results
	printMatrix(diff, 1, 18, "error");
	std::cout << "Error Magnitudes p/R/du/f:" << perror << "; " << Rerror << "; " << uerror << "; " << ferror << std::endl;
	std::cout << "----" << std::endl;
	if ((perror < 1.0) && (ferror < 0.01)) {
		std::cout << "Test passed for Inserted Length =" << InsertedLength << std::endl;
		std::cout << "###" << std::endl;
	}
	else {
		std::cout << "TEST FAIL: " << "Results do not match!" << std::endl;
		std::cout << "###" << std::endl;
		FailFlag++;
	}


	// Jacobian calculation
	// note that Jacobian uses Eigen compatible vectors and matrices for input and outputs
	VectorXd control(NUM_ACT_SET * 3 + 1);				// input vector
	for (int i = 0; i < 3 * NUM_ACT_SET + 1; i++)  control(i) = control_inputs[i];
	VectorXd FKsolution(18);  // output vector from FK solution p[0..2],R[0..8],u0[0..2],ftip[0..2]  R: in row major order
	for (int i = 0; i < 18; i++) FKsolution(i) = output_values_Solve[i];
	MatrixXd Ja;

	// Cosserat Rod Model - Solve the Forward Kinematics and calculate the Jacobian
	Ja = CRM_FKJacobian_Analytical(control, FKsolution, FKParams_Solve); // evaluate the Jacobian matrix J

	std::cout << "Ja = \n" << Ja << std::endl;			// print the evaluated Jacobian matrix

	//
	// let's compare it against numerical Jacobian calculation
	VectorXd outputs;								// the output vector evaluated together with Jacobian matrix below
	MatrixXd Jn;

	// Cosserat Rod Model - Solve the Forward Kinematics and calculate the Jacobian
	Jn = CRM_FKJacobian_Numerical(control, outputs, FKParams_Solve, localmin); // evaluate the output vector F and the Jacobian matrix J

	std::cout << "F = \n" << outputs << std::endl;		// print the evaluated output vector
	std::cout << "Jn = \n" << Jn << std::endl;			// print the evaluated Jacobian matrix


	//
	// Let's compare it to brute force calculation
	MatrixXd Jbf;

	Jbf = CRM_FKJacobian_BruteForce(control, outputs, FKParams_Solve, localmin); // evaluate the output vector F and the Jacobian matrix J

	std::cout << "F = \n" << outputs << std::endl;		// print the evaluated output vector
	std::cout << "Jbf = \n" << Jbf << std::endl;			// print the evaluated Jacobian matrix
	std::cout << "Ja-Jbf_ftip = \n" << (Ja - Jbf.bottomRows(3)) << std::endl;			// print the evaluated Jacobian matrix

	// let's compare the difference between the two solutions
	double jerror = (Jbf.bottomRows(3) - Ja).norm();
	double jperror = (Jbf.topRows(3)).norm();
	// display the results
	std::cout << "||Ja-Jbf_ftip||:" << jerror << "  ||J||: " << Ja.norm() << "  ||Jbf_p||:" << jperror << std::endl;
	std::cout << "----" << std::endl;
	if ((jerror < 0.05 * Ja.norm()) && (jperror < 1e-3)) {
		std::cout << "Test passed for Inserted Length =" << InsertedLength << std::endl;
		std::cout << "###" << std::endl;
	}
	else {
		std::cout << "TEST FAIL: " << "Results do not match!" << std::endl;
		std::cout << "###" << std::endl;
		FailFlag++;
	}

	delete[] ReportedMarkerPos;
	delete[] ReportedCoilPos;
	delete[] ReportedCoilOrient;

	return FailFlag;
}





