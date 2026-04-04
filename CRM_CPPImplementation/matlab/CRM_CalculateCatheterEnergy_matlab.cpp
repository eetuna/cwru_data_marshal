/* CRM_ForwardKinematics
 * [PotentialEnergy] = CRM_CalculateCatheterEnergy(deltau0, ftip, control_inputs, FKParams);
 * CRM Catheter Potential Energy Calculation
 *   
*/

#include "mex.h"
#include <cstring>
#include <string>
#include "CRM.hpp"
#include "CRM_BVPIVP_APIDeclarations.hpp"
using namespace CRMCatheterModel;

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
        
    int32_t ix, jx, kx;
    mxArray* field_value;
    double* pointer;
    int _PCNT;  // parameter counter

    //
    // lets parse the rhs structures
    //

    // deltau0
    _PCNT=0;
    double *deltau0;
    deltau0 = mxGetDoubles(prhs[_PCNT]);

    // ftip
    _PCNT++;
    double *ftip;
    ftip = mxGetDoubles(prhs[_PCNT]);

    // control_inputs
    _PCNT++;
    double *control_inputs;
    control_inputs = mxGetDoubles(prhs[_PCNT]);

    //FKParams
    _PCNT++;
    const mxArray* CParamStruct = mxGetField(prhs[_PCNT], 0, "CathParams");
    const mxArray* CConfigStruct = mxGetField(prhs[_PCNT], 0, "CathConfig");

    // first get sizes from the structure
    field_value=mxGetField(CParamStruct, 0, "no_flex_seg");
    pointer = mxGetDoubles(field_value);
    int32_t no_flex_seg = pointer[0];

    field_value=mxGetField(CParamStruct, 0, "no_rigid_seg");
    pointer = mxGetDoubles(field_value);
    int32_t no_rigid_seg = pointer[0];

    field_value=mxGetField(CParamStruct, 0, "no_act_set");
    pointer = mxGetDoubles(field_value);
    int32_t no_act_set = pointer[0];

    field_value=mxGetField(CParamStruct, 0, "no_segments");
    pointer = mxGetDoubles(field_value);
    int32_t no_segments = pointer[0];

    field_value=mxGetField(CParamStruct, 0, "no_locmarkers");
    pointer = mxGetDoubles(field_value);
    int32_t no_locmarkers = pointer[0];

    // create CRM_ForwardKinematics variables
    CRMCatheterModelParams CathParams(no_flex_seg, no_rigid_seg, no_act_set, no_locmarkers);		// physical parameters of the catheter
    CatheterConfiguration CathConfig;		// catheter configuration in space
    int localmin;
    CRMForwardKinematicsData Params;
    Params.CathParams = &CathParams;
    Params.CathConfig = &CathConfig;

    // need to fill in Params

    // fill CathParams
    field_value=mxGetField(CParamStruct, 0, "SegmentTypes");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<no_segments; ix++) {
            if (pointer[ix] == 0) CathParams.SegmentTypes[ix] = CatheterSegmentType::FLEXIBLE;
            else if (pointer[ix] == 1) CathParams.SegmentTypes[ix] = CatheterSegmentType::RIGID_WITH_ACTUATOR;
            else if (pointer[ix] == 2) CathParams.SegmentTypes[ix] = CatheterSegmentType::RIGID;
    }
    
    field_value=mxGetField(CParamStruct, 0, "SegLengths");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<no_segments; ix++) CathParams.SegLengths[ix] = pointer[ix];

    field_value=mxGetField(CParamStruct, 0, "LocMarkers");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<no_locmarkers; ix++) CathParams.LocMarkers[ix] = pointer[ix];

    field_value=mxGetField(CParamStruct, 0, "InnerRadius");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<no_flex_seg; ix++) CathParams.InnerRadius[ix] = pointer[ix];

    field_value=mxGetField(CParamStruct, 0, "OuterRadius");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<no_flex_seg; ix++) CathParams.OuterRadius[ix] = pointer[ix];

    field_value=mxGetField(CParamStruct, 0, "YoungsModulus");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<no_flex_seg; ix++) CathParams.YoungsModulus[ix] = pointer[ix];

    field_value=mxGetField(CParamStruct, 0, "ShearModulus");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<no_flex_seg; ix++) CathParams.ShearModulus[ix] = pointer[ix];

    field_value=mxGetField(CParamStruct, 0, "ustar");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<no_flex_seg; ix++) for (jx=0; jx<3; jx++) CathParams.ustar[ix][jx] = pointer[ix+no_flex_seg*jx];  // matlab arrays are column-major

    field_value=mxGetField(CParamStruct, 0, "CoilAlignmentAngles");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<no_act_set; ix++) for (jx=0; jx<2; jx++) CathParams.CoilAlignmentAngles[ix][jx] = pointer[ix+no_act_set*jx];  // matlab arrays are column-major

    field_value=mxGetField(CParamStruct, 0, "CoilTurnAreaMat");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<no_act_set; ix++) for (jx=0; jx<9; jx++) CathParams.CoilTurnAreaMat[ix][jx] = pointer[ix+no_act_set*jx];  // matlab arrays are column-major

    field_value=mxGetField(CParamStruct, 0, "rho");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<no_segments; ix++) CathParams.rho[ix] = pointer[ix];

    field_value=mxGetField(CParamStruct, 0, "ActMass");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<no_act_set; ix++) CathParams.ActMass[ix] = pointer[ix];

    // fill CathConfig        
    field_value=mxGetField(CConfigStruct, 0, "B0");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<3; ix++) CathConfig.B0[ix] = pointer[ix];

    field_value=mxGetField(CConfigStruct, 0, "g");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<3; ix++) CathConfig.g[ix] = pointer[ix];

    field_value=mxGetField(CConfigStruct, 0, "p0");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<3; ix++) CathConfig.p0[ix] = pointer[ix];

    field_value=mxGetField(CConfigStruct, 0, "R0");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<9; ix++) CathConfig.R0[ix] = pointer[ix];

    // fill the rest
    field_value=mxGetField(prhs[_PCNT], 0, "ContactMode");
    mxInt8 *cm = mxGetInt8s(field_value);
    Params.ContactMode = (cm[0]==0)?(ContactModeType::FREE_TIP):(ContactModeType::FIXED_TIP);

    field_value=mxGetField(prhs[_PCNT], 0, "TipConstraintPoint");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<3; ix++) Params.TipConstraintPoint[ix] = pointer[ix];

    field_value=mxGetField(prhs[_PCNT], 0, "TipForce");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<3; ix++) Params.TipForce[ix] = pointer[ix];

    field_value=mxGetField(prhs[_PCNT], 0, "deltau0_initialguess");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<3; ix++) Params.deltau0_initialguess[ix] = pointer[ix];

    field_value=mxGetField(prhs[_PCNT], 0, "ftip_initialguess");
    pointer = mxGetDoubles(field_value);
    for (ix=0; ix<3; ix++) Params.ftip_initialguess[ix] = pointer[ix];

    field_value=mxGetField(prhs[_PCNT], 0, "IntegrationStepSize");
    pointer = mxGetDoubles(field_value);
    Params.IntegrationStepSize = pointer[0];

    field_value=mxGetField(prhs[_PCNT], 0, "FinalValueOnly");
    mxLogical *FVO = mxGetLogicals(field_value);
    Params.FinalValueOnly = FVO[0];

    // for ReportedMarkerPos:
    // Create a local array
    double  *dynamicDataforRMP;
    dynamicDataforRMP = (double  *) mxMalloc(no_locmarkers * 3 * sizeof(double));
    Params.ReportedMarkerPos = (double (*)[3]) dynamicDataforRMP;
    /*
    double ReportedMarkerPos[NUM_LOCALIZATION_MARKERS][3];
    Params.ReportedMarkerPos = &ReportedMarkerPos;
    */

    // for ReportedCoilPos:
    // Create a local array
    double  *dynamicDataforRCP;
    dynamicDataforRCP = (double  *) mxMalloc(no_act_set * 3 * sizeof(double));
    Params.ReportedCoilPos = (double (*)[3]) dynamicDataforRCP;
    /*
    double ReportedCoilPos[NUM_ACT_SETS][3];
    Params.ReportedCoilPos = &ReportedCoilPos;
    */

    // for ReportedCoilOrient:
    // Create a local array
    double  *dynamicDataforRCO;
    dynamicDataforRCO = (double  *) mxMalloc(no_act_set * 9 * sizeof(double));
    Params.ReportedCoilOrient = (double (*)[9]) dynamicDataforRCO;
    /*
    double ReportedCoilOrient[NUM_ACT_SETS][9];
    Params.ReportedCoilOrient = &ReportedCoilOrient;
    */

    // Prepare the variables needed for CRMSolverIVP call

    // We will need to construct the ShootingMethodParamSet
    //     Parameters needed for constructing the ShootingMethodParamSet
	double ActuationCurrents[NUM_ACT_SET][3];
	double InsertedLength;
    for (int i = 0; i < NUM_ACT_SET; i++)	for (int j = 0; j < 3; j++)	ActuationCurrents[i][j] = control_inputs[i * 3 + j];
    InsertedLength = control_inputs[NUM_ACT_SET * 3];

    //     construct the ShootingMethodParamSet
    CRMShootingMethodParams BVPParams = 
		CRMConstructShootingMethodParamSet(CathParams, CathConfig, InsertedLength, ActuationCurrents, Params.ContactMode, Params.TipConstraintPoint, Params.TipForce, Params.IntegrationStepSize);

    // we want to calculate the potential energy of the catheter to return
    bool CalculateEnergy = true;
    // we only want the final value - no need to calculate the marker locations and coil positions/orientations
    bool FinalValueOnly = true;  // Params.FinalValueOnly;

    // output variables for CRMSolverIVP
    double PotentialEnergy;
	double xf[NUM_STATES];
	double residual[3];
    
	CRMSolverIVP(BVPParams, deltau0, ftip, CalculateEnergy, FinalValueOnly, xf, residual, PotentialEnergy, Params.ReportedMarkerPos, Params.ReportedCoilPos, Params.ReportedCoilOrient);


    //
    // lets fill the lhs structures
    //

    // copy PotentialEnergy
    _PCNT=0;
    plhs[_PCNT] = mxCreateNumericMatrix(1, 1, mxDOUBLE_CLASS, mxREAL);
    pointer = mxGetDoubles(plhs[_PCNT]);
    pointer[0] = PotentialEnergy;
   
    /*
    // copy ReportedMarkerPos
    _PCNT++;
    // Create a 0-by-0 mxArray to return; we will use the dynamically allocated memory
    plhs[_PCNT] = mxCreateNumericMatrix(0, 0, mxDOUBLE_CLASS, mxREAL);
    mxSetPr(plhs[_PCNT], dynamicDataforRMP);
    // note that we will flip the rows and columns since the ReportedMarkerPos array
    //   comes as row-major order from the C/C++ function
    mxSetM(plhs[_PCNT], 3);                         
    mxSetN(plhs[_PCNT], no_locmarkers);  


    // copy ReportedCoilPos
    _PCNT++;
    // we will not use dynamically allocated memory, instead directly copy entries
    mwSize dims[3];
    dims[0]=3;  dims[1]=1; dims[2]=no_act_set;
    plhs[_PCNT] = mxCreateNumericArray(3, dims, mxDOUBLE_CLASS, mxREAL);
    pointer = mxGetDoubles(plhs[_PCNT]);
    for (ix = 0; ix<3; ix++) 
        for (jx=0; jx<3; jx++) 
            for (kx=0; kx<no_act_set; kx++)
                pointer[ix+3*jx+9*kx] = Params.ReportedCoilOrient[kx][ix*3+jx];


    // copy ReportedCoilOrient
    _PCNT++
    // we will not use dynamically allocated memory, instead directly copy entries
    mwSize dims[3];
    dims[0]=3;  dims[1]=3; dims[2]=no_act_set;
    plhs[_PCNT] = mxCreateNumericArray(3, dims, mxDOUBLE_CLASS, mxREAL);
    pointer = mxGetDoubles(plhs[_PCNT]);
    for (ix = 0; ix<3; ix++) 
        for (jx=0; jx<3; jx++) 
            for (kx=0; kx<no_act_set; kx++)
                pointer[ix+3*jx+9*kx] = Params.ReportedCoilOrient[kx][ix*3+jx];
    */



}

