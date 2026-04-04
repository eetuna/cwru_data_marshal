#pragma once
#include <iostream>
#include <cmath>
#include <chrono> 

#include "CRM_BVPIVP_APIDeclarations.hpp"
#include "CRM_IVPJacobian.hpp"
#include "CRM_KinematicsTestFunctions.hpp"
using namespace std::chrono;

#ifdef ANALYTICAL_SE3_STEP
constexpr int NUM_INTEGRATION_STATES = 3;   // u[0..2]
#else
constexpr int NUM_INTEGRATION_STATES = 15;   // u[0..2],R[0..9],p[0..2]
#endif
