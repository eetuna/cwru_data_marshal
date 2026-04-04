#include <iostream>
#include <fstream>
#include "CRM.hpp"
#include "CRM_MatrixOperations.hpp"

namespace CRMCatheterModel {


    CRMCatheterModelParams Load_CRMCatheterModelParams(const char* path_to_input) {

        std::string line, configline, var;

        int32_t no_seg = 0, no_flex = 0, no_rigid = 0, no_act = 0, no_loc = 0;
        char Comment_Line_var[] = "#";
        char CatheterConfig_var[] = "CatheterConfig";
        char NUM_LOCALIZATION_MARKERS_var[] = "NumLocalizationMarkers";
        char oRlist_var[] = "oRlist";
        char iRlist_var[] = "iRlist";
        char YoungModlist_var[] = "YoungModlist";
        char ShearModlist_var[] = "ShearModlist";
        char CoilAlignmentAngles_var[] = "CoilAlignmentAngles";
        char CoilTurnAreaMat_var[] = "CoilTurnAreaMat";
        char SegmentLengths_var[] = "SegmentLengths";
        char ActMass_var[] = "ActMass";
        char MarkerLoc_var[] = "MarkerLoc";
        char rho_var[] = "rho";
        char ustarlist_var[] = "ustarlist";


        std::vector<double> input;
        std::ifstream File;
        File.open(path_to_input);
        if (!File.is_open()) {
            throw std::runtime_error{ "Unable to open Catheter Model Parameters file!" };
        }
        // first line to process should be CatheterConfig
        while (getline(File, line)) {  // we will ignore everything until we see the CatheterConfig
            std::istringstream line_(line);
            line_ >> var;
            if (var == Comment_Line_var) {
                // comment lines will be ignored
                continue;
            } 
            else if (var == CatheterConfig_var) {
                configline = line; // make a copy, since we will process this line again to construct the catheter class
                // but, first, let's find the segment counts
                while (line_ >> var) {
                    if (var == "F") no_flex++;
                    else if (var == "R") no_rigid++;
                    else if (var == "A") { no_act++; no_rigid++; }
                    else throw std::runtime_error{ "Invalid Segment Type!" };
                }
                break;  // Move onto the next keyword to process
            } 
            else {
                throw std::runtime_error{ "First non-comment keyword should have been 'CatheterConfig' !..." };
            }
        }
        no_seg = no_flex + no_rigid;
        // error check
        if ( no_act!=NUM_ACT_SET ) {
            throw std::runtime_error{ "Number of actuator sets in the catheter configuration doesn't match the compiled library configuration!.." };
        }
        /*
        if (no_rigid > no_act) {
            throw std::runtime_error{ "Unsupported Catheter Configuration!..  Rigid segments without actuators present." };
        }
        */
        // Next, we should find the NumLocalizationMarkers
        while (getline(File, line)) { // we will ignore everything until we see the NumLocalizationMarkers
            std::istringstream line_(line);
            line_ >> var;
            if (var == Comment_Line_var) {
                // comment lines will be ignored
                continue;
            } 
            else if (var == NUM_LOCALIZATION_MARKERS_var) {
                line_ >> no_loc;
                break;  // Move onto the next keyword to process
            }
            else {
                throw std::runtime_error{ "Second non-comment keyword should have been 'NumLocalizationMarkers' !..." };
            }
        }

        CRMCatheterModelParams CathParams(no_flex, no_rigid, no_act, no_loc);

        // Let's fill in the SegmentTypes array
        std::istringstream line_(configline);
        line_ >> var;  // we know that the first keyword is CatheterConfig_var, so ignore
        size_t ix = 0, jx = 0;
        while (line_ >> var) {
            if (var == "F") CathParams.SegmentTypes[ix] = CatheterSegmentType::FLEXIBLE;
            else if (var == "R") CathParams.SegmentTypes[ix] = CatheterSegmentType::RIGID;
            else if (var == "A") CathParams.SegmentTypes[ix] = CatheterSegmentType::RIGID_WITH_ACTUATOR;
            ix++;
        }

        // And, let's process the remaining parameters
        while (getline(File, line)) {  // we will ignore blank lines
            std::istringstream line_(line);
            line_ >> var;
            if (var == Comment_Line_var) {
                // comment lines will be ignored
                continue;
            } 
            else if (var == oRlist_var) {
                for (ix = 0; ix < no_flex; ix++) line_ >> CathParams.OuterRadius[ix];
            }
            else if (var == iRlist_var) {
                for (ix = 0; ix < no_flex; ix++) line_ >> CathParams.InnerRadius[ix];
            }
            else if (var == YoungModlist_var) {
                for (ix = 0; ix < no_flex; ix++) line_ >> CathParams.YoungsModulus[ix];
            }
            else if (var == ShearModlist_var) {
                for (ix = 0; ix < no_flex; ix++) line_ >> CathParams.ShearModulus[ix];
            }
            else if (var == ustarlist_var) {
                for (ix = 0; ix < no_flex; ix++) {
                    for (jx = 0; jx < 3; jx++) {
                        line_ >> CathParams.ustar[ix][jx];
                    }
                }
            }
            else if (var == CoilAlignmentAngles_var) {
                for (ix = 0; ix < no_act; ix++) {
                    for (jx = 0; jx < 2; jx++) {
                        line_ >> CathParams.CoilAlignmentAngles[ix][jx];
                    }
                }
            }
            else if (var == CoilTurnAreaMat_var) {
                for (ix = 0; ix < no_act; ix++) {
                    for (jx = 0; jx < 9; jx++) {
                        line_ >> CathParams.CoilTurnAreaMat[ix][jx];
                    }
                }
            }
            else if (var == ActMass_var) {
                for (ix = 0; ix < no_act; ix++) line_ >> CathParams.ActMass[ix];
            }
            else if (var == SegmentLengths_var) {
                for (ix = 0; ix < no_seg; ix++) line_ >> CathParams.SegLengths[ix];
            }
            else if (var == rho_var) {
                for (ix = 0; ix < no_seg; ix++) line_ >> CathParams.rho[ix];
            }
            else if (var == MarkerLoc_var) {
                for (ix = 0; ix < no_loc; ix++) line_ >> CathParams.LocMarkers[ix];
            }
        }

        File.close();
        return CathParams;
    }


    CatheterConfiguration Load_CatheterConfiguration(const char* path_to_input) {

        std::string line, var;
        CatheterConfiguration CathConfig;

        char Comment_Line_var[] = "#";
        char B0_var[] = "B0";
        char gravity_var[] = "gravity";
        char p0_var[] = "p0";
        char R0_var[] = "R0";

        // *** Catheter Configuration in spatial coordinates
        // B0 field vector of the MRI scanner (in spatial coordinates) - unit: Tesla
        double B0[3] = { 0 }, gravity[3] = { 0 }, p0[3] = { 0 }, R0[9] = { 0 };

        std::vector<double> input;
        std::ifstream File;
        File.open(path_to_input);
        if (!File.is_open()) {
            throw std::runtime_error{ "Unable to open Catheter Configuration file!" };
        }
        while (getline(File, line)) {
            std::istringstream line_(line);
            line_ >> var;
            if (var == Comment_Line_var) {
                // comment lines will be ignored
                continue;
            }
            else if (var == B0_var) {
                for (double& i : B0) { line_ >> i; }
            }
            else if (var == gravity_var) {
                for (double& i : gravity) { line_ >> i; }
            }
            else if (var == p0_var) {
                for (double& i : p0) { line_ >> i; }
            }
            else if (var == R0_var) {
                for (double& i : R0) { line_ >> i; }
            }
        }

        mCopy_AB<3>(B0, CathConfig.B0);
        mCopy_AB<3>(gravity, CathConfig.g);
        mCopy_AB<3>(p0, CathConfig.p0);
        mCopy_AB<9>(R0, CathConfig.R0);
        File.close();
        return CathConfig;

    }


}

