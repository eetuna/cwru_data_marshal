/* Load_CatheterConfiguration
 * CathConfig  = Load_CatheterConfiguration(inputfile);
 * Loads the catheter configuration from the file inputfile
 *   and returns in the CathConfig structure
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
    
    CatheterConfiguration CathConfig = Load_CatheterConfiguration(str);
    //try {
    //    CatheterConfiguration CathConfig = Load_CatheterConfiguration(str);
    //}
    //catch (const std::runtime_error& e) {
    //    mexPrintf("Error loading Catheter Configuration file. \n");
    //    mexPrintf(e.what());
    //}
    
    const char* field_names[] = {"B0","g","p0","R0"};
    mwSize dims[2] = {1, NUMBER_OF_STRUCTS};
    mwIndex idx=0;
    /* Create a 1-by-n array of structs. */
    plhs[0] = mxCreateStructArray(2, dims, NUMBER_OF_FIELDS, field_names);
    int B0_field = mxGetFieldNumber(plhs[0], "B0");
    int g_field = mxGetFieldNumber(plhs[0], "g");
    int p0_field = mxGetFieldNumber(plhs[0], "p0");
    int R0_field = mxGetFieldNumber(plhs[0], "R0");
    
    /* Populate the name and phone fields of the structure. */
    mxArray* field_value;
    double* pointer;
    unsigned int ix, jx;

    for (mwSize i = 0; i < NUMBER_OF_STRUCTS; i++) {

        field_value = mxCreateDoubleMatrix(3 /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<3; ix++) pointer[ix]=CathConfig.B0[ix];
        mxSetFieldByNumber(plhs[0], i, B0_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */

        field_value = mxCreateDoubleMatrix(3 /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<3; ix++) pointer[ix]=CathConfig.g[ix];
        mxSetFieldByNumber(plhs[0], i, g_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */
        
        field_value = mxCreateDoubleMatrix(3 /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<3; ix++) pointer[ix]=CathConfig.p0[ix];
        mxSetFieldByNumber(plhs[0], i, p0_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */

        field_value = mxCreateDoubleMatrix(9 /*ROWS*/, 1 /*COLUMNS*/, mxREAL);
        pointer = mxGetPr(field_value);
        for (ix=0; ix<9; ix++) pointer[ix]=CathConfig.R0[ix];
        mxSetFieldByNumber(plhs[0], i, R0_field, field_value);          /* Use mxSetFieldByNumber instead of mxSetField for efficiency */

    }

    mxFree(str);

}


