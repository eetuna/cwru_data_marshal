disp('### CRM Forward Kinematics Examples... ');
disp('Free Space Deflection Example: ');

% *** Load parameters from file
% This step would typically needs to be executed only once
%  Physical Description of the Catheter
FKParams.CathParams=Load_CRMCatheterModelParams_matlab('../catheterdata/CatheterParameterSet_2.txt')
%  Catheter Configuration in spatial coordinates
FKParams.CathConfig=Load_CatheterConfiguration_matlab('../catheterdata/CatheterSpatialConfiguration_1.txt')

% *** Other External variables
% specify if catheter is in free space or if the catheter tip is constrained to a contact point
FKParams.ContactMode = int8(0);  % 0: FREE_TIP;   1: FIXED_TIP
% External point force (in spatial coordinates) applied at the tip of the catheter (\lambda = 0)  - unit: ??
%   (this will be used when ContactMode == 0, FREE_TIP)
FKParams.TipForce = [ 0.0, 0.0, 0.0 ];
% The spatial coordinates of the point where the catheter tip is constrained to be 
%   (this will be used when ContactMode == ContactModeType::FIXED_TIP)
FKParams.TipConstraintPoint = [ 0.0, 0.0, 0.0 ];
% We want the localization coil locations, too
FKParams.FinalValueOnly = false; 

% *** Numerical Computation Parameters
% Stepsize used in numerical integration along the length of the catheter during IVP - unit: mm
FKParams.IntegrationStepSize = 0.2;
% Define initial guesses to be used when solving boundary value problem
% initial guess for the curvature at the catheter base
FKParams.deltau0_initialguess = [ 0.0, 0.0, 0.0 ];
% initial guess for the contstraint force at the catheter tip (this will be used when ContactMode == ContactModeType::FIXED_TIP)
FKParams.ftip_initialguess = [ 0.0, 0.0, 0.0 ];

% *** Control Inputs
% Inserted Length of the catheter (length of the catheter that is inside the heart chamber) - unit: mm
InsertedLength = 100.0;
% Actuation currents for each of the coils for each of the coil sets - unit: A
ActuationCurrents = [ 0.100, 0.100, 0.100, 0.100, 0.100, 0.100 ];
% pack them together into a single array
control_inputs = [ ActuationCurrents InsertedLength ];

%
% Cosserat Rod Model - Solve the Forward Kinematics 
%
[FKsolution, PotentialEnergy, ReportedMarkerPos, ReportedCoilPos, ReportedCoilOrient, localmin] = CRM_ForwardKinematics_matlab(control_inputs, FKParams);
% FKsolution: p[0..2],R[0..8],deltau0[0..2](,ftip[0..2])  R: in row major order
% ReportedMarkerPos .. by 3 array of coordinates of localization marker locations 
% localmin: numerical nonlinear equation solver diagnostic output

% Cosserat Rod Model - calculate the Jacobian using the solution of the Forward Kinematics
Ja = CRM_FKJacobian_Analytical_matlab(control_inputs, FKsolution, FKParams); 

disp('p[0..2],R[0..8],deltau0[0..2]');
disp(FKsolution);
disp('PE');
disp(PotentialEnergy);
format shortG
disp('ReportedMarkerPos');
disp(ReportedMarkerPos);
disp('ReportedCoilPos');
disp(ReportedCoilPos);
disp('ReportedCoilOrient');
disp(ReportedCoilOrient);
disp('Ja:  [ dpdz, dpdft; ws_dz, ws_dft ]');
disp(Ja);
%format default
DrawCRMCatheterModel(FKParams,control_inputs,true)

%%

disp('### CRM Forward Kinematics Examples... ');
disp('Potential Energy Calculation Example (Free Space):');

PE0 = CRM_CalculateCatheterEnergy_matlab(FKParams.deltau0_initialguess, FKParams.TipForce, control_inputs, FKParams);
PEf = CRM_CalculateCatheterEnergy_matlab(FKsolution(13:15), FKParams.TipForce, control_inputs, FKParams);
disp('PE(deltau0_initialguess)');
disp(PE0);
disp('PE(deltau0_calc)');
disp(PEf);

%%

disp('### CRM Forward Kinematics Examples... ');
disp('Constrained Tip Deflection Example: ');

% The spatial coordinates of the point where the catheter tip is constrained to be 
%   (this will be used when ContactMode == ContactModeType::FIXED_TIP)
FKParams.TipConstraintPoint = [ 6.12, 40.57, 93.96 ];
% specify if catheter is in free space or if the catheter tip is constrained to a contact point
FKParams.ContactMode = int8(1);  % 0: FREE_TIP;   1: FIXED_TIP

% *** Control Inputs
% Inserted Length of the catheter (length of the catheter that is inside the heart chamber) - unit: mm
InsertedLength = 104.0;
% Actuation currents for each of the coils for each of the coil sets - unit: A
ActuationCurrents = [ 0.100, 0.100, 0.100, 0.100, 0.100, 0.100 ];
% pack them together into a single array
control_inputs = [ ActuationCurrents InsertedLength ];

%
% Cosserat Rod Model - Solve the Forward Kinematics 
%
[FKsolution, PotentialEnergy, ReportedMarkerPos, ReportedCoilPos, ReportedCoilOrient, localmin] = CRM_ForwardKinematics_matlab(control_inputs, FKParams);
% FKsolution: p[0..2],R[0..8],deltau0[0..2](,ftip[0..2])  R: in row major order
% ReportedMarkerPos .. by 3 array of coordinates of localization marker locations 
% localmin: numerical nonlinear equation solver diagnostic output

% Cosserat Rod Model - calculate the Jacobian using the solution of the Forward Kinematics
Ja = CRM_FKJacobian_Analytical_matlab(control_inputs, FKsolution, FKParams); 

disp('p[0..2],R[0..8],deltau0[0..2],ftip[0..2]');
disp(FKsolution);
disp('PE');
disp(PotentialEnergy);
format shortG
disp('ReportedMarkerPos');
disp(ReportedMarkerPos);
disp('ReportedCoilPos');
disp(ReportedCoilPos);
disp('ReportedCoilOrient');
disp(ReportedCoilOrient);
disp('Ja:  [ dftip_dz ]');
disp(Ja);
%format default
DrawCRMCatheterModel(FKParams,control_inputs,true)

%%

disp('### CRM Forward Kinematics Examples... ');
disp('Potential Energy Calculation Example (Constrained Tip):');

PE0 = CRM_CalculateCatheterEnergy_matlab(FKParams.deltau0_initialguess, FKsolution(16:18), control_inputs, FKParams);
PEf = CRM_CalculateCatheterEnergy_matlab(FKsolution(13:15), FKsolution(16:18), control_inputs, FKParams);
disp('PE(deltau0_initialguess)');
disp(PE0);
disp('PE(deltau0_calc)');
disp(PEf);

