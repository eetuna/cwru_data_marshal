mex COMPFLAGS='-std:c++17' -DNUM_ACT_SET=2 Load_CRMCatheterModelParams_matlab.cpp "../src/*.cpp" "../numerical/*.cpp" -I../src/ -I../numerical/ -I../..

mex COMPFLAGS='-std:c++17' -DNUM_ACT_SET=2 Load_CatheterConfiguration_matlab.cpp "../src/*.cpp" "../numerical/*.cpp" -I../src/ -I../numerical/ -I../..

mex -R2018a COMPFLAGS='-std:c++17' -DNUM_ACT_SET=2 CRM_ForwardKinematics_matlab.cpp "../src/*.cpp" "../numerical/*.cpp" -I../src/ -I../numerical/ -I../..

mex -R2018a COMPFLAGS='-std:c++17' -DNUM_ACT_SET=2 CRM_FKJacobian_Analytical_matlab.cpp "../src/*.cpp" "../numerical/*.cpp" -I../src/ -I../numerical/ -I../..

mex -R2018a COMPFLAGS='-std:c++17' -DNUM_ACT_SET=2 CRM_CalculateCatheterEnergy_matlab.cpp "../src/*.cpp" "../numerical/*.cpp" -I../src/ -I../numerical/ -I../..
