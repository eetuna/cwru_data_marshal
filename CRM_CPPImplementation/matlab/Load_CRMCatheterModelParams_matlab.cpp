/* Load_CRMCatheterModelParams
 * CathParams = Load_CRMCatheterModelParams(inputfile);
 * Loads the catheter model parameters from the file inputfile
 *   and returns on the CathParams structure
*/

#include "mex.h"
#include <cstring>
#include <string>
#include "CRM.hpp"
using namespace CRMCatheterModel;

#define NUMBER_OF_FIELDS (sizeof(field_names) / sizeof(*field_names))

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
        
    mwSize NUMBER_OF_STRUCTS =1;   /* if we will have an array of structures, this will be larger than 1 */
    
    char *str;
    str=mxArrayToString(prhs[0]);
    mexPrintf("Reading file: %s\n", str);

    CRMCatheterModelParams CathParams =Load_CRMCatheterModelParams(str);
    //try {
    //    CRMCatheterModelParams CathParams =Load_CRMCatheterModelParams(str);
    //}
    //catch (const std::runtime_error& e) {
    //    mexPrintf("Error loading Catheter Model Parameters file. \n");
    //    mexPrintf(e.what());
    //}
    

    const char* field_names[] = { "no_flex_seg", "no_rigid_seg", "no_act_set", "no_segments", "no_locmarkers", "SegmentTypes", "SegLengths","LocMarkers","InnerRadius","OuterRadius","YoungsModulus","ShearModulus","ustar","CoilAlignmentAngles","CoilTurnAreaMat","rho","ActMass" };
    mwSize dims[2] = {1, NUMBER_OF_STRUCTS};
    mwIndex idx=0;
    /* Create a 1-by-n array of structs. */
    plhs[0] = mxCreateStructArray(2, dims, NUMBER_OF_FIELDS, field_names);
    int no_flex_seg_field = mxGetFieldNumber(plhs[0], "no_flex_seg");    
    int no_rigid_seg_field = mxGetFieldNumber(plhs[0], "no_rigid_seg");    
    int no_act_set_field = mxGetFieldNumber(plhs[0], "no_act_set");    
    int no_segments_field = mxGetFieldNumber(plhs[0], "no_segments");    
    int no_locmarkers_field = mxGetFieldNumber(plhs[0], "no_locmarkers");    
    int SegmentTypes_field = mxGetFieldNumber(plhs[0], "SegmentTypes");    
    int SegLengths_field = mxGetFieldNumber(plhs[0], "SegLengths");    
    int LocMarkers_field = mxGetFieldNumber(plhs[0], "LocMarkers");    
    int InnerRadius_field = mxGetFieldNumber(plhs[0], "InnerRadius");    
    int OuterRadius_field = mxGetFieldNumber(plhs[0], "OuterRadius");    
    int YoungsModulus_field = mxGetFieldNumber(plhs[0], "YoungsModulus");    
    int ShearModulus_field = mxGetFieldNumber(plhs[0], "ShearModulus");    
    int ustar_field = mxGetFieldNumber(plhs[0], "ustar");    
    int CoilAlignmentAngles_field = mxGetFieldNumber(plhs[0], "CoilAlignmentAngles");    
    int CoilTurnAreaMat_field = mxGetFieldNumber(plhs[0], "CoilTurnAreaMat");    
    int rho_field = mxGetFieldNumber(plhs[0], "rho");    
    int ActMass_field = mxGetFieldNumber(plhs[0], "ActMass");    

     /* Populate the fields of the structure. */
    mxArray* field_value;
    double* pointer;
    unsigned int ix, jx;

    for (mwSize i = 0; i < NUMBER_OF_STRUCTS; i++) {

        field_value = mxCreateDoubleMatrix(1 /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        pointer[0]=CathParams.no_flex_seg;
        mxSetFieldByNumber(plhs[0], i, no_flex_seg_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(1 /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        pointer[0]=CathParams.no_rigid_seg;
        mxSetFieldByNumber(plhs[0], i, no_rigid_seg_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(1 /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        pointer[0]=CathParams.no_act_set;
        mxSetFieldByNumber(plhs[0], i, no_act_set_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(1 /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        pointer[0]=CathParams.no_segments;
        mxSetFieldByNumber(plhs[0], i, no_segments_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(1 /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        pointer[0]=CathParams.no_locmarkers;
        mxSetFieldByNumber(plhs[0], i, no_locmarkers_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(CathParams.no_segments /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<CathParams.no_segments; ix++) {
            if (CathParams.SegmentTypes[ix] == CatheterSegmentType::FLEXIBLE ) pointer[ix]=0;
            else if (CathParams.SegmentTypes[ix] == CatheterSegmentType::RIGID_WITH_ACTUATOR ) pointer[ix]=1;
            else if (CathParams.SegmentTypes[ix] == CatheterSegmentType::RIGID ) pointer[ix]=2;
        }
        mxSetFieldByNumber(plhs[0], i, SegmentTypes_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(CathParams.no_segments /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<CathParams.no_segments; ix++) pointer[ix]=CathParams.SegLengths[ix];
        mxSetFieldByNumber(plhs[0], i, SegLengths_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(CathParams.no_locmarkers /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<CathParams.no_locmarkers; ix++) pointer[ix]=CathParams.LocMarkers[ix];
        mxSetFieldByNumber(plhs[0], i, LocMarkers_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(CathParams.no_flex_seg /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<CathParams.no_flex_seg; ix++) pointer[ix]=CathParams.InnerRadius[ix];
        mxSetFieldByNumber(plhs[0], i, InnerRadius_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(CathParams.no_flex_seg /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<CathParams.no_flex_seg; ix++) pointer[ix]=CathParams.OuterRadius[ix];
        mxSetFieldByNumber(plhs[0], i, OuterRadius_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(CathParams.no_flex_seg /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<CathParams.no_flex_seg; ix++) pointer[ix]=CathParams.YoungsModulus[ix];
        mxSetFieldByNumber(plhs[0], i, YoungsModulus_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(CathParams.no_flex_seg /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<CathParams.no_flex_seg; ix++) pointer[ix]=CathParams.ShearModulus[ix];
        mxSetFieldByNumber(plhs[0], i, ShearModulus_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(CathParams.no_flex_seg /*ROWS*/, 3 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<CathParams.no_flex_seg; ix++) for (jx=0; jx<3; jx++) pointer[ix+CathParams.no_flex_seg*jx]=CathParams.ustar[ix][jx];  // matlab arrays are column-major
        mxSetFieldByNumber(plhs[0], i, ustar_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(CathParams.no_act_set /*ROWS*/, 2 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<CathParams.no_act_set; ix++) for (jx=0; jx<2; jx++) pointer[ix+CathParams.no_act_set*jx]=CathParams.CoilAlignmentAngles[ix][jx];  // matlab arrays are column-major
        mxSetFieldByNumber(plhs[0], i, CoilAlignmentAngles_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(CathParams.no_act_set /*ROWS*/, 9 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<CathParams.no_act_set; ix++) for (jx=0; jx<9; jx++) pointer[ix+CathParams.no_act_set*jx]=CathParams.CoilTurnAreaMat[ix][jx];  // matlab arrays are column-major
        mxSetFieldByNumber(plhs[0], i, CoilTurnAreaMat_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(CathParams.no_segments /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<CathParams.no_segments; ix++) pointer[ix]=CathParams.rho[ix];
        mxSetFieldByNumber(plhs[0], i, rho_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(CathParams.no_act_set /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<CathParams.no_act_set; ix++) pointer[ix]=CathParams.ActMass[ix];
        mxSetFieldByNumber(plhs[0], i, ActMass_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */

    }
        
    mxFree(str);

}

