function DrawCRMCatheterModel(FKParams, control_inputs, flag, varargin); % ReportedMarkerPos, ReportedCoilOrient, segment_steps)
% DrawCRMCatheterModel Function to draw the catheter    
% FKParams - forward kinematics params structure used in FK calculation
% control_inputs - the control inputs used in FK calculation
% flag - rendering controls (true: draw mag vector, false: don't draw mag vector) 
%
%  if the following optional parameters are not all specified, then 
%     CRM_ForwardKinematics_matlab is executed to determine the catheter shape
%       
% ReportedMarkerPos - the array of marker positions returned from FK calculation
% ReportedCoilPos - the array of coil orientation position returned from FK calculation
% ReportedCoilOrient - the array of coil orientation matrices returned from FK calculation
% segment_steps - the number of steps of markers (each gap between a pair of markers is a single step) 
%                  for each of the catheter segments
%

    if (nargin ~=7)
        % Create a copy of FK Params
        FKParamsRevised=FKParams;
        % *** Change the localization marker locations for smooth visualization
        vis_step=2;  % stepsize for visualization
        segment_steps=ceil((FKParamsRevised.CathParams.SegLengths)/vis_step);
        segment_deltas=FKParamsRevised.CathParams.SegLengths./segment_steps;
        FKParamsRevised.CathParams.LocMarkers=[ 0 ];
        cumlength = 0;
        for (cnt = 1:FKParamsRevised.CathParams.no_segments) 
            FKParamsRevised.CathParams.LocMarkers = [FKParamsRevised.CathParams.LocMarkers ; [cumlength + (segment_deltas(cnt):segment_deltas(cnt):FKParamsRevised.CathParams.SegLengths(cnt))]' ];
            cumlength = cumlength + FKParamsRevised.CathParams.SegLengths(cnt);
        end
        FKParamsRevised.CathParams.no_locmarkers = length(FKParamsRevised.CathParams.LocMarkers);
        
        %
        % Cosserat Rod Model - Solve the Forward Kinematics 
        %
        [~, ~, ReportedMarkerPos, ReportedCoilPos, ReportedCoilOrient, ~] = CRM_ForwardKinematics_matlab(control_inputs, FKParamsRevised);
        % FKsolution: p[0..2],R[0..8],deltau0[0..2](,ftip[0..2])  R: in row major order
        % ReportedMarkerPos .. by 3 array of coordinates of localization marker locations 
        % localmin: numerical nonlinear equation solver diagnostic output
        
    else
        % Use the prespecified visualization date
        ReportedMarkerPos=varargin{1};
        ReportedCoilPos=varargin{2};
        ReportedCoilOrient=varargin{3};
        segment_steps=varargin{4};
    end

    % Calculate the magnetization vectors for coils
    for (cnt=1:FKParams.CathParams.no_act_set)
        c0=cos(FKParams.CathParams.CoilAlignmentAngles(1));
        s0=sin(FKParams.CathParams.CoilAlignmentAngles(1));
        c1=cos(FKParams.CathParams.CoilAlignmentAngles(2));
        s1=sin(FKParams.CathParams.CoilAlignmentAngles(2));
        CoilAlignmentTurnAreaMatrix(:,:,cnt) = [ c0 -s1 0; s0 c1 0; 0 0 1] * reshape(FKParams.CathParams.CoilTurnAreaMat(cnt,:),3,3)';
        ActuationCurrents(:,cnt)=control_inputs(((cnt-1)*3+1):((cnt-1)*3+3))';
        MagMoment_c(:,cnt) = CoilAlignmentTurnAreaMatrix(:,:,cnt)*ActuationCurrents(:,cnt);
        MagMoment_s(:,cnt) = ReportedCoilOrient(:,:,cnt)*MagMoment_c(:,cnt);
    end

    %draw the catheter body, coils, and the magnetization vectors
    plotidx=1;
    actno=1;
    hold on
    for (cnt = 1:FKParams.CathParams.no_segments) 
        x=ReportedMarkerPos(1,plotidx:(plotidx+segment_steps(cnt)));
        y=ReportedMarkerPos(2,plotidx:(plotidx+segment_steps(cnt)));
        z=ReportedMarkerPos(3,plotidx:(plotidx+segment_steps(cnt)));
        p=plot3(x,y,z);
        if (FKParams.CathParams.SegmentTypes(cnt) == 0) % flexible
            p.LineWidth=3;
            p.Color=[0 0.4470 0.7410];
        elseif (FKParams.CathParams.SegmentTypes(cnt) == 1) % rigid with actuator
            p.LineWidth=6;
            p.Color=[0.6350 0.0780 0.1840];
            %plot magnetization vector
            cx=mean(x);
            cy=mean(y);
            cz=mean(z);
            if (flag==true) %draw magnetization vector
                MagVectDir=MagMoment_s(:,actno);
                %mag=norm(MagMoment_s(:,actno));
                %if (mag>0) 
                %    MagVectDir=MagMoment_s(:,actno)/mag*10;
                %else 
                %    MagVectDir=[0;0;0];
                %end
                mv=quiver3(cx,cy,cz,MagVectDir(1),MagVectDir(2),MagVectDir(3),50);
                mv.LineWidth=2;
                mv.Color=[0.4660 0.6740 0.1880];
                %mark coil center
                %cs=plot3(ReportedCoilPos(1,1,actno),ReportedCoilPos(2,1,actno),ReportedCoilPos(3,1,actno),'o');
                %cs.LineWidth=3;
                %cs.Color=[0.4660 0.6740 0.1880];
                actno=actno+1;
            end
        elseif (FKParams.CathParams.SegmentTypes(cnt) == 2) % rigid
            p.LineWidth=3;
            p.Color=[0.9290 0.6940 0.1250];
        end
        plotidx = plotidx+segment_steps(cnt);
    end
    %plot a circle at the entry point (p0)
    plot3(FKParams.CathConfig.p0(1),FKParams.CathConfig.p0(2),FKParams.CathConfig.p0(3),'ko')
    %draw x-y-z coordinate axes of R0 at p0
    plot3([FKParams.CathConfig.p0(1),FKParams.CathConfig.p0(1)+10*FKParams.CathConfig.R0(1)], ...
        [FKParams.CathConfig.p0(2),FKParams.CathConfig.p0(2)+10*FKParams.CathConfig.R0(4)], ...
        [FKParams.CathConfig.p0(3),FKParams.CathConfig.p0(3)+10*FKParams.CathConfig.R0(7)],'r-');
    plot3([FKParams.CathConfig.p0(1),FKParams.CathConfig.p0(1)+10*FKParams.CathConfig.R0(2)], ...
        [FKParams.CathConfig.p0(2),FKParams.CathConfig.p0(2)+10*FKParams.CathConfig.R0(5)], ...
        [FKParams.CathConfig.p0(3),FKParams.CathConfig.p0(3)+10*FKParams.CathConfig.R0(8)],'g-');
    plot3([FKParams.CathConfig.p0(1),FKParams.CathConfig.p0(1)+10*FKParams.CathConfig.R0(3)], ...
        [FKParams.CathConfig.p0(2),FKParams.CathConfig.p0(2)+10*FKParams.CathConfig.R0(6)], ...
        [FKParams.CathConfig.p0(3),FKParams.CathConfig.p0(3)+10*FKParams.CathConfig.R0(9)],'b-');
    %set the view volume
    xlim([-100,100]);
    ylim([-100,100]);
    zlim([-10,150]);
    %mark the B0 vector of the MRI
    b0v=quiver3(0,0,50,FKParams.CathConfig.B0(1),FKParams.CathConfig.B0(2),FKParams.CathConfig.B0(3),15);
    text(1+FKParams.CathConfig.B0(1)*15,1+FKParams.CathConfig.B0(2)*15,51+FKParams.CathConfig.B0(3)*15,'B_0');
    b0v.Color=[0.4940 0.1840 0.5560];
    %turn on grid
    grid on;
    %set view
    view([-37.5 30]);
    hold off

end
