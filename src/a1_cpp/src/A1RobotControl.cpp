//
// Created by shuoy on 10/19/21.
//

#include "A1RobotControl.h"

A1RobotControl::A1RobotControl() {
    std::cout << "init A1RobotControl" << std::endl;
    // init QP solver
    // init some parameters
    Q.diagonal() << 1.0, 1.0, 1.0, 400.0, 400.0, 100.0;
    R = 1e-3;
    mu = 0.7;
    F_min = 0;
    F_max = 180;
    hessian.resize(3 * NUM_LEG, 3 * NUM_LEG);
    gradient.resize(3 * NUM_LEG);
    linearMatrix.resize(NUM_LEG + 4 * NUM_LEG, 3 * NUM_LEG);
    lowerBound.resize(NUM_LEG + 4 * NUM_LEG);
    lowerBound.setZero();
    upperBound.resize(NUM_LEG + 4 * NUM_LEG);
    upperBound.setZero();

    // init mpc skip counter
    mpc_init_counter = 0;

    // constraint matrix fixed
    for (int i = 0; i < NUM_LEG; ++i) {
        // extract F_zi
        linearMatrix.insert(i, 2 + i * 3) = 1;
        // friction pyramid
        // 1. F_xi < uF_zi
        linearMatrix.insert(NUM_LEG + i * 4, i * 3) = 1;
        linearMatrix.insert(NUM_LEG + i * 4, 2 + i * 3) = -mu;
        lowerBound(NUM_LEG + i * 4) = -OsqpEigen::INFTY;
        // 2. F_xi > -uF_zi    ===> -F_xi -uF_zi < 0
        linearMatrix.insert(NUM_LEG + i * 4 + 1, i * 3) = -1;
        linearMatrix.insert(NUM_LEG + i * 4 + 1, 2 + i * 3) = -mu;
        lowerBound(NUM_LEG + i * 4 + 1) = -OsqpEigen::INFTY;
        // 3. F_yi < uF_zi
        linearMatrix.insert(NUM_LEG + i * 4 + 2, 1 + i * 3) = 1;
        linearMatrix.insert(NUM_LEG + i * 4 + 2, 2 + i * 3) = -mu;
        lowerBound(NUM_LEG + i * 4 + 2) = -OsqpEigen::INFTY;
        // 4. -F_yi > uF_zi
        linearMatrix.insert(NUM_LEG + i * 4 + 3, 1 + i * 3) = -1;
        linearMatrix.insert(NUM_LEG + i * 4 + 3, 2 + i * 3) = -mu;
        lowerBound(NUM_LEG + i * 4 + 3) = -OsqpEigen::INFTY;
    }
    // debug linearMatrix
//    std::cout << Eigen::MatrixXd(linearMatrix) << std::endl;

    terrain_angle_filter = MovingWindowFilter(100);
    for (int i = 0; i < NUM_LEG; ++i) {
        recent_contact_x_filter[i] = MovingWindowFilter(60);
        recent_contact_y_filter[i] = MovingWindowFilter(60);
        recent_contact_z_filter[i] = MovingWindowFilter(60);
    }
}

A1RobotControl::A1RobotControl(ros::NodeHandle &_nh) : A1RobotControl() {
    std::cout << "init nh" << std::endl;
    nh = _nh;
    _nh.param("use_sim_time", use_sim_time);
    // initial debug publisher
    for (int i = 0; i < NUM_LEG; ++i) {
        std::string id = std::to_string(i);
        std::string start_topic = "/isaac_a1/foot" + id + "/start_pos";
        std::string end_topic = "/isaac_a1/foot" + id + "/end_pos";
        std::string path_topic = "/isaac_a1/foot" + id + "/swing_path";

        pub_foot_start[i] = nh.advertise<visualization_msgs::Marker>(start_topic, 100);
        pub_foot_end[i] = nh.advertise<visualization_msgs::Marker>(end_topic, 100);
        pub_foot_path[i] = nh.advertise<visualization_msgs::Marker>(path_topic, 100);

        // set basic info of markers
        foot_start_marker[i].header.frame_id = "a1_world";
        foot_start_marker[i].ns = "basic_shapes";
        foot_start_marker[i].id = 10 + i;
        foot_start_marker[i].type = visualization_msgs::Marker::CYLINDER;
        foot_start_marker[i].action = visualization_msgs::Marker::ADD;
        // Set the scale of the marker -- 1x1x1 here means 1m on a side
        foot_start_marker[i].scale.x = 0.08;
        foot_start_marker[i].scale.y = 0.08;
        foot_start_marker[i].scale.z = 0.02;
        foot_start_marker[i].pose.orientation.x = 0.0;
        foot_start_marker[i].pose.orientation.y = 0.0;
        foot_start_marker[i].pose.orientation.z = 0.0;
        foot_start_marker[i].pose.orientation.w = 1.0;

        // Set the color -- be sure to set alpha to something non-zero!
        foot_start_marker[i].color.r = 1.0f;
        foot_start_marker[i].color.g = 0.0f;
        foot_start_marker[i].color.b = 0.0f;
        foot_start_marker[i].color.a = 1.0;

        foot_end_marker[i].lifetime = ros::Duration();

        foot_end_marker[i].header.frame_id = "a1_world";
        foot_end_marker[i].ns = "basic_shapes";
        foot_end_marker[i].id = 20 + i;
        foot_end_marker[i].type = visualization_msgs::Marker::CYLINDER;
        foot_end_marker[i].action = visualization_msgs::Marker::ADD;
        // Set the scale of the marker -- 1x1x1 here means 1m on a side
        foot_end_marker[i].scale.x = 0.08;
        foot_end_marker[i].scale.y = 0.08;
        foot_end_marker[i].scale.z = 0.02;
        foot_end_marker[i].pose.orientation.x = 0.0;
        foot_end_marker[i].pose.orientation.y = 0.0;
        foot_end_marker[i].pose.orientation.z = 0.0;
        foot_end_marker[i].pose.orientation.w = 1.0;

        // Set the color -- be sure to set alpha to something non-zero!
        foot_end_marker[i].color.r = 0.0f;
        foot_end_marker[i].color.g = 0.0f;
        foot_end_marker[i].color.b = 1.0f;
        foot_end_marker[i].color.a = 1.0;

        foot_end_marker[i].lifetime = ros::Duration();

        foot_path_marker[i].header.frame_id = "a1_world";
        foot_path_marker[i].ns = "basic_shapes";
        foot_path_marker[i].id = 30 + i;
        foot_path_marker[i].type = visualization_msgs::Marker::LINE_STRIP;
        foot_path_marker[i].action = visualization_msgs::Marker::ADD;
        // Set the scale of the marker -- 1x1x1 here means 1m on a side
        foot_path_marker[i].scale.x = 0.02;
        foot_path_marker[i].pose.position.x = 0.0;
        foot_path_marker[i].pose.position.y = 0.0;
        foot_path_marker[i].pose.position.z = 0.0;
        foot_path_marker[i].pose.orientation.w = 1.0;
        foot_path_marker[i].pose.orientation.x = 0.0;
        foot_path_marker[i].pose.orientation.y = 0.0;
        foot_path_marker[i].pose.orientation.z = 0.0;
        foot_path_marker[i].points.resize(10); // fix to be 10 points
        foot_path_marker[i].colors.resize(10); // fix to be 10 points
        for (int k = 0; k < 10; k++) {
            foot_path_marker[i].colors[k].r = 0.0f;
            foot_path_marker[i].colors[k].g = 1.0f;
            foot_path_marker[i].colors[k].b = 0.0f;
            foot_path_marker[i].colors[k].a = 1.0f;
        }

        foot_path_marker[i].lifetime = ros::Duration();
    }
    pub_terrain_angle = nh.advertise<std_msgs::Float64>("a1_debug/terrain_angle", 100);
}


Eigen::Vector2d A1RobotControl::move_COM_pos(A1CtrlStates &state,bool movestate)
{  
    Eigen::Vector3d LH,LF,RH,RF;
    Eigen::Vector2d COM_pos_world;
    double k1,k2,b1,b2;
    double x_inter,y_inter,x_COM,y_COM,region_center_x,region_center_y;

    if (movestate)   //move COM left
    {   
        LH = state.foot_pos_world.block<3, 1>(0, 2);
        LF = state.foot_pos_world.block<3, 1>(0, 0);
        RF = state.foot_pos_world.block<3, 1>(0, 1);
        RH = state.foot_pos_target_world.block<3, 1>(0, 3);
    }
    else      //move COM right
    {
        LH = state.foot_pos_target_world.block<3, 1>(0, 2);
        LF = state.foot_pos_world.block<3, 1>(0, 0);
        RF = state.foot_pos_world.block<3, 1>(0, 1);
        RH = state.foot_pos_world.block<3, 1>(0, 3);
    }
    // for (size_t i = 0; i < 2; i++)
    // {
    //     std::cout<<"LH:"<<LH(i)<<","<<"LF:"<<LF(i)<<","<<"RH:"<<RH(i)<<","<<"RF:"<<RF(i)<<std::endl;
    // }
    // std::cout<<"------------------------------------------"<<std::endl;
    
    //calculate the intersec point
    k1 = (RF(1) - LH(1))/(RF(0) - LH(0));
    k2 = (LF(1) - RH(1))/(LF(0) - RH(0));
    b1 = LH(1) - LH(0)*k1;
    b2 = LF(1) - LF(0)*k2;
    x_inter = (b2-b1)/(k1-k2);
    y_inter = k1*x_inter+b1;

    double delx,dely;
    if (movestate)   //move COM left
    {   
        region_center_x = (LF(0)+LH(0)+x_inter)/3;
        region_center_y = (LF(1)+LH(1)+y_inter)/3;
    }
    else      //move COM right
    {
        region_center_x = (RF(0)+RH(0)+x_inter)/3;
        region_center_y = (RF(1)+RH(1)+y_inter)/3;
    }
    // std::cout<<"center x:"<<region_center_x<<","<<"center y:"<<region_center_y<<std::endl;
    delx = region_center_x - x_inter;
    dely = region_center_y - y_inter;
    // x_COM = x_inter + delx/2;
    // y_COM = y_inter + dely/2;
    x_COM = x_inter + delx;
    y_COM = y_inter + dely;

    COM_pos_world << x_COM,y_COM;

    return COM_pos_world;
}

void A1RobotControl::static_walking_ctrl(A1CtrlStates &state, double t, double dt)
{
    double counter;
    state.counter_static_gait += state.counter_static_gait_speed;
    counter = state.counter_static_gait = std::fmod(state.counter_static_gait, state.counter_per_static_gait);

    if (t < 7.0) // start to walk after 8 seconds
    {
        state.gait_sequence = GAIT_SEQUENCE::STAND;
        counter = state.counter_static_gait = 0.0;
    }
    else if (counter < state.counter_per_static_move_com)
        {state.gait_sequence = GAIT_SEQUENCE::MOVE_COM_TO_LEFT; 
        // std::cout << "move com to left mode\n";
        }
    else if (counter < state.counter_per_static_move_com + state.counter_per_static_swing)
        {state.gait_sequence = GAIT_SEQUENCE::RH; 
        // std::cout << "RH mode\n";
        }
    else if (counter < state.counter_per_static_move_com + state.counter_per_static_swing * 2)
        {state.gait_sequence = GAIT_SEQUENCE::RF; 
        // std::cout << "RF mode\n";
        }
    else if (counter < state.counter_per_static_move_com + state.counter_per_static_swing * 2 + state.counter_per_static_move_com)
        {state.gait_sequence = GAIT_SEQUENCE::MOVE_COM_TO_RIGHT; 
        // std::cout << "move com to right mode\n";
        }
    else if (counter < state.counter_per_static_move_com + state.counter_per_static_swing * 3 + state.counter_per_static_move_com)
        {state.gait_sequence = GAIT_SEQUENCE::LH;
        //  std::cout << "LH mode\n";
         }
    else 
        {state.gait_sequence = GAIT_SEQUENCE::LF; 
        // std::cout << "LF mode\n";
        }

//
    Eigen::Vector2d res;
    Eigen::Vector2d x_0, y_0;
    Eigen::Vector2d x_f, y_f;
    double t_now, T_f;
    Eigen::Vector2d COM_pos;

    // finite state machine
    switch (state.gait_sequence)
    {
    case GAIT_SEQUENCE::STAND:
        // step 1: planed contact states
        // step 2(if move CoM): determine (root_pos_d, root_lin_vel_d) <- (starting_pos_CoM, starting_vel_CoM, target_pos_CoM, target_vel_CoM)
        // step 2(if swing): determine foot_pos_target_rel <- foot_pos_start
        
        for (int i = 0; i < NUM_LEG; ++i)
            state.plan_contacts[i] = true;

        state.starting_pos_CoM.segment<2>(0) = state.root_pos.segment<2>(0);
        state.starting_vel_CoM.segment<2>(0) = state.root_lin_vel_d.segment<2>(0);
        break;

    case GAIT_SEQUENCE::MOVE_COM_TO_LEFT:
        // step 1: 
        for (int i = 0; i < NUM_LEG; ++i)
            state.plan_contacts[i] = true;    //all legs stands to adjust the COM

        // step 2:
        
        COM_pos = move_COM_pos(state, 1);
        // std::cout<< "move left:"<<  COM_pos(0)<<" ,"<<COM_pos(1)<<std::endl;
        //state.target_pos_CoM.segment<2>(0) = (state.foot_pos_world.block<2, 1>(0, 0) + state.foot_pos_world.block<2, 1>(0, 1) + state.foot_pos_world.block<2, 1>(0, 2)) / 3;
        //state.target_pos_CoM(1) += 0.015;
        state.target_pos_CoM(0) = COM_pos(0);
        state.target_pos_CoM(1) = COM_pos(1);
        state.target_vel_CoM.segment<2>(0) = Eigen::VectorXd::Zero(2);

        x_0(0) = state.starting_pos_CoM(0);
        x_0(1) = state.starting_vel_CoM(0);
        x_f(0) = state.target_pos_CoM(0);
        x_f(1) = state.target_vel_CoM(0);

        t_now = counter * state.control_dt;
        T_f = state.counter_per_static_move_com * state.control_dt;
        res = cubicSplinesUtils.cubic_splines_curve(t_now, T_f, x_0, x_f);
        state.root_pos_d(0) = res(0);
        state.root_lin_vel_d(0) = res(1);

        y_0(0) = state.starting_pos_CoM(1);
        y_0(1) = state.starting_vel_CoM(1);
        y_f(0) = state.target_pos_CoM(1);
        y_f(1) = state.target_vel_CoM(1);
        res = cubicSplinesUtils.cubic_splines_curve(t_now, T_f, y_0, y_f);
        state.root_pos_d(1) = res(0);
        state.root_lin_vel_d(1) = res(1);

        state.counter_static_relative = 0.0;

        break;
    case GAIT_SEQUENCE::RH:
        
        // step 1:
        state.plan_contacts[0] = true;
        state.plan_contacts[1] = true;
        state.plan_contacts[2] = true;
        state.plan_contacts[3] = false;
        
        // step 2:
        state.counter_static_relative = counter - state.counter_per_static_move_com;
        

        break;
    case GAIT_SEQUENCE::RF:

        // step 1:
        state.plan_contacts[0] = true;
        state.plan_contacts[1] = false;
        state.plan_contacts[2] = true;
        state.plan_contacts[3] = true;

        // step 2:
        state.counter_static_relative = counter - state.counter_per_static_move_com - state.counter_per_static_swing;

        // initialization for next state
        state.starting_pos_CoM.segment<2>(0) = state.root_pos.segment<2>(0);
        state.starting_vel_CoM.segment<2>(0) = state.root_lin_vel_d.segment<2>(0);
        break;
    case GAIT_SEQUENCE::MOVE_COM_TO_RIGHT:

        // step 1: 
        for (int i = 0; i < NUM_LEG; ++i)
            state.plan_contacts[i] = true;

        // step 2:
        COM_pos = move_COM_pos(state, 0);
        // std::cout<< "move left:"<<  COM_pos(0)<<" ,"<<COM_pos(1)<<std::endl;
        //state.target_pos_CoM.segment<2>(0) = (state.foot_pos_world.block<2, 1>(0, 0) + state.foot_pos_world.block<2, 1>(0, 1) + state.foot_pos_world.block<2, 1>(0, 3)) / 3;
        //state.target_pos_CoM(1) -= 0.015;
        state.target_pos_CoM(0) = COM_pos(0);
        state.target_pos_CoM(1) = COM_pos(1);

        state.target_vel_CoM.segment<2>(0) = Eigen::VectorXd::Zero(2);

        x_0(0) = state.starting_pos_CoM(0);
        x_0(1) = state.starting_vel_CoM(0);
        x_f(0) = state.target_pos_CoM(0);
        x_f(1) = state.target_vel_CoM(0);

        t_now = (counter - state.counter_per_static_move_com - state.counter_per_static_swing * 2) * state.control_dt;
        T_f = state.counter_per_static_move_com * state.control_dt;
        res = cubicSplinesUtils.cubic_splines_curve(t_now, T_f, x_0, x_f);
        state.root_pos_d(0) = res(0);
        state.root_lin_vel_d(0) = res(1);

        y_0(0) = state.starting_pos_CoM(1);
        y_0(1) = state.starting_vel_CoM(1);
        y_f(0) = state.target_pos_CoM(1);
        y_f(1) = state.target_vel_CoM(1);
        res = cubicSplinesUtils.cubic_splines_curve(t_now, T_f, y_0, y_f);
        state.root_pos_d(1) = res(0);
        state.root_lin_vel_d(1) = res(1);

        state.counter_static_relative = 0.0;

        break;
    case GAIT_SEQUENCE::LH:
        // step 1:
        state.plan_contacts[0] = true;
        state.plan_contacts[1] = true;
        state.plan_contacts[2] = false;
        state.plan_contacts[3] = true;
         
        
        // step 2:
        state.counter_static_relative = counter - state.counter_per_static_move_com * 2 - state.counter_per_static_swing * 2;

        break;
    case GAIT_SEQUENCE::LF:
        // step 1:
        state.plan_contacts[0] = false;
        state.plan_contacts[1] = true;
        state.plan_contacts[2] = true;
        state.plan_contacts[3] = true;
        
        // step 2:
        state.counter_static_relative = counter - state.counter_per_static_move_com * 2 - state.counter_per_static_swing * 3;

        state.starting_pos_CoM.segment<2>(0) = state.root_pos.segment<2>(0);
        state.starting_vel_CoM.segment<2>(0) = state.root_lin_vel_d.segment<2>(0);
        break;
    default:
        break;
    }

}

void A1RobotControl::generate_footholds_ref(A1CtrlStates &state, double t, double dt) {

    double delta_x, delta_y;
    delta_x = 0.065;
    delta_y = 0.0;

    state.footholds_rel = state.default_foot_pos;

    for (int i = 0; i < NUM_LEG; ++i)
    {
        state.footholds_rel(0, i) += delta_x;
        state.footholds_rel(1, i) += delta_y;

    }

    // Step 1: generate default footholds w.r.t. world frame (state.foot_pos_target_world)
    // calculate default pos w.r.t world frame
    for (int i = 0; i < NUM_LEG; ++i)
    {   

        state.default_footholds_abs.block<3, 1>(0, i) = state.root_rot_mat * state.default_foot_pos.block<3, 1>(0, i);
        state.default_footholds_world.block<3, 1>(0, i) = state.default_footholds_abs.block<3, 1>(0, i) + state.root_pos;
    }
    // Step 2: determine footholds in x-y plane w.r.t world frame
    // state.foot_pos_target_world.block<3, 1>(0, i) = 3 by 1 target in world
    for (int i = 0; i < NUM_LEG; ++i)
    {   
        if(i == int(state.gait_sequence)) 
        {
            // if (int(t * 1000) % 10 == 0) {

            //     std::cout << "------------------" << std::endl;
            //     std::cout << " leg " << i <<": " << "x=" <<  state.foot_pos_world(0, i) << "/" << state.foot_pos_target_world(0, i)
            //     << " y=" << state.foot_pos_world(1, i) << "/" << state.foot_pos_target_world(1, i)
            //     << " z=" << state.foot_pos_world(2, i) << "/" << state.foot_pos_target_world(2, i)
            //     << std::endl;
            // }
            continue;
        }
        
        state.foot_pos_target_world.block<3, 1>(0, i) = state.default_footholds_world.block<3, 1>(0, i);
        state.foot_pos_target_world(0, i) += delta_x;
        state.foot_pos_target_world(1, i) += delta_y;



        Eigen::Vector2d temp_pos_recorded = state.foot_pos_target_world.block<2, 1>(0, i);

        // define the feasible region
        double upperBoundx, lowerBoundx;
        double upperBoundy, lowerBoundy;

        
        upperBoundx = state.default_footholds_world(0, i) + FOOT_DELTA_X_LIMIT;
        lowerBoundx = state.default_footholds_world(0, i) - 0.1 * FOOT_DELTA_X_LIMIT;

        upperBoundy = state.default_footholds_world(1, i) + FOOT_DELTA_Y_LIMIT;
        lowerBoundy = state.default_footholds_world(1, i) - FOOT_DELTA_Y_LIMIT;

        if(i == 0 || i == 2)
        {
            lowerBoundy += 0.02;
        }
        else
        {
            upperBoundy -= 0.02;
        }


        // choose from list
        double farestPile = -1e5;
        for (auto it = state.record.begin(); it != state.record.end(); ++ it) {
        Eigen::Vector3d pos_recorded = (*it).pos;

            


            if(pos_recorded[0] >= lowerBoundx && pos_recorded[0] <= upperBoundx && pos_recorded[1] >= lowerBoundy && pos_recorded[1] <= upperBoundy)
            {
                

                if(pos_recorded[0] > farestPile)
                {
                    
                    farestPile = pos_recorded[0];
                    temp_pos_recorded = pos_recorded.segment<2>(0);

                    if (i == 0 || i == 2)
                    {
                        temp_pos_recorded(1) += 0.02;
                        temp_pos_recorded(0) -= 0.01;
                    }

                    if (i == 2 || i == 3)
                    {
                        temp_pos_recorded(0) -= 0.02;
                    }

                    
                    // state.default_footholds_times ++ ;
                    // if ((state.default_footholds_times > 4000)&&(i == 0 || i == 2) )
                    // {
                    //     state.default_footholds_times = 0;
                    //     printf("--------------------------------------\n");
                    //     printf("Gait sequence: ",state.gait_sequence);
                    //     printf("choose meihuazhuang footholds! \n");
                    // }
                }
                
            
            }
            else
            {
                // state.default_footholds_times ++ ;
                // if ((state.default_footholds_times > 4000)&&(i == 0 || i == 2) )
                // {
                //     state.default_footholds_times = 0;
                //     printf("--------------------------------------\n");
                //     printf("Gait sequence: ",state.gait_sequence);
                //     printf("choose default footholds! \n");
                // }
                
            }
        }
        
        state.foot_pos_target_world.block<2, 1>(0, i) = temp_pos_recorded;

    }
    

    for (int i = 0; i < NUM_LEG; ++i)
    {   
        Eigen::Vector2d deltaDistance = state.foot_pos_target_world.block<2, 1>(0, i) - state.default_footholds_world.block<2, 1>(0, i);

        if(deltaDistance(0) > FOOT_DELTA_X_LIMIT)
        {state.foot_pos_target_world(0, i) = state.default_footholds_world(0, i) + FOOT_DELTA_X_LIMIT;}
        if(deltaDistance(0) < -FOOT_DELTA_X_LIMIT)
        {state.foot_pos_target_world(0, i) = state.default_footholds_world(0, i) - FOOT_DELTA_X_LIMIT;}
        
        if(deltaDistance(1) > FOOT_DELTA_Y_LIMIT)
        {state.foot_pos_target_world(1, i) = state.default_footholds_world(1, i) + FOOT_DELTA_Y_LIMIT;}
        if(deltaDistance(1) < -FOOT_DELTA_Y_LIMIT)
        {state.foot_pos_target_world(1, i) = state.default_footholds_world(1, i) - FOOT_DELTA_Y_LIMIT;}
        
    }
 
 
    // Step 3: transform to the robot frame (state.foot_pos_target_rel)
    for (int i = 0; i < NUM_LEG; ++i)
    {   
        state.foot_pos_target_abs.block<3, 1>(0, i) = state.foot_pos_target_world.block<3, 1>(0, i) - state.root_pos;
        state.foot_pos_target_rel.block<3, 1>(0, i) = state.root_rot_mat.transpose() * state.foot_pos_target_abs.block<3, 1>(0, i);
    }

}


void A1RobotControl::select_footholds(A1CtrlStates &state, double t, double dt) {



    generate_footholds_ref(state, t, dt);

    // Eigen::Matrix<double,3,NUM_LEG> footholds_rel;
    // footholds_rel = state.footholds_rel;
    

    // state.foot_pos_target_rel = state.default_foot_pos;
    for (int i = 0; i < NUM_LEG; ++i)
    {
        // state.foot_pos_target_rel(0, i) += delta_x;
        // state.foot_pos_target_rel(1, i) += delta_y;

        // state.foot_pos_target_rel(0, i) = footholds_rel(0,i);

        // state.foot_pos_target_rel(1, i) = footholds_rel(1,i);

        state.foot_pos_target_abs.block<3, 1>(0, i) = state.root_rot_mat * state.foot_pos_target_rel.block<3, 1>(0, i);
        state.foot_pos_target_world.block<3, 1>(0, i) = state.foot_pos_target_abs.block<3, 1>(0, i) + state.root_pos;
    }
    
}

void A1RobotControl::generate_swing_to_dest(A1CtrlStates &state, double t, double dt) {
    state.joint_torques.setZero();

    // get current foot pos and target foot pose
    Eigen::Matrix<double, 3, NUM_LEG> foot_pos_cur;
    Eigen::Matrix<double, 3, NUM_LEG> foot_vel_cur;
    Eigen::Matrix<float, 1, NUM_LEG> spline_time;
    spline_time.setZero();
    Eigen::Matrix<double, 3, NUM_LEG> foot_pos_target;
    foot_pos_target.setZero();
    Eigen::Matrix<double, 3, NUM_LEG> foot_vel_target;
    foot_vel_target.setZero();
    Eigen::Matrix<double, 3, NUM_LEG> foot_pos_error;
    Eigen::Matrix<double, 3, NUM_LEG> foot_vel_error;

    // the foot force of swing foot and stance foot, both are in robot frame
    Eigen::Matrix<double, 3, NUM_LEG> foot_forces_kin;
    Eigen::Matrix<double, 3, NUM_LEG> foot_forces_grf;

    for (int i = 0; i < NUM_LEG; ++i) {
        foot_pos_cur.block<3, 1>(0, i) = state.root_rot_mat_z.transpose() * state.foot_pos_abs.block<3, 1>(0, i);

        // from foot_pos_cur to foot_pos_final computes an intermediate point using BezierUtils
        if (state.plan_contacts[i] == true) {
            // stance foot
            spline_time(i) = 0.0;
            // in this case the foot should be stance
            // keep refreshing foot_pos_start in stance mode
            state.foot_pos_start.block<3, 1>(0, i) = foot_pos_cur.block<3, 1>(0, i);

        } else {

            // in this case the foot should be swing
            spline_time(i) = float(state.counter_static_relative) / float(state.counter_per_static_swing);
            
        }

        foot_pos_target.block<3, 1>(0, i) = bezierUtils[i].get_foot_pos_curve(spline_time(i),
                                                                              state.foot_pos_start.block<3, 1>(0, i),
                                                                              state.foot_pos_target_rel.block<3, 1>(0, i),
                                                                              0.0);

        

        foot_vel_cur.block<3, 1>(0, i) = (foot_pos_cur.block<3, 1>(0, i) - state.foot_pos_rel_last_time.block<3, 1>(0, i)) / dt;
        state.foot_pos_rel_last_time.block<3, 1>(0, i) = foot_pos_cur.block<3, 1>(0, i);

        foot_vel_target.block<3, 1>(0, i) = (foot_pos_target.block<3, 1>(0, i) - state.foot_pos_target_last_time.block<3, 1>(0, i)) / dt;
        state.foot_pos_target_last_time.block<3, 1>(0, i) = foot_pos_target.block<3, 1>(0, i);

        foot_pos_error.block<3, 1>(0, i) = foot_pos_target.block<3, 1>(0, i) - foot_pos_cur.block<3, 1>(0, i);
        foot_vel_error.block<3, 1>(0, i) = foot_vel_target.block<3, 1>(0, i) - foot_vel_cur.block<3, 1>(0, i);
        foot_forces_kin.block<3, 1>(0, i) = foot_pos_error.block<3, 1>(0, i).cwiseProduct(state.kp_foot.block<3, 1>(0, i)) +
                                            foot_vel_error.block<3, 1>(0, i).cwiseProduct(state.kd_foot.block<3, 1>(0, i));
    }
    state.foot_pos_cur = foot_pos_cur;

    // detect early contact
    bool last_contacts[NUM_LEG];

    for (int i = 0; i < NUM_LEG; ++i) {
        if (state.counter_static_relative <= state.counter_per_static_swing * 0.5) {
            state.early_contacts[i] = false;
        }
        else if(!state.plan_contacts[i] && (state.foot_force(i) > FOOT_FORCE_LOW)) {
            state.early_contacts[i] = true;
        }

        // actual contact
        last_contacts[i] = state.contacts[i];
        state.contacts[i] = state.plan_contacts[i] || state.early_contacts[i];

        // record recent contact position if the foot is in touch with the ground
        if (state.contacts[i]) {
//            state.foot_pos_recent_contact.block<3, 1>(0, i) = state.root_rot_mat.transpose() * (state.foot_pos_world.block<3, 1>(0, i));
//            state.foot_pos_recent_contact.block<3, 1>(0, i) = state.foot_pos_abs.block<3, 1>(0, i);
            state.foot_pos_recent_contact.block<3, 1>(0, i)
                    << recent_contact_x_filter[i].CalculateAverage(state.foot_pos_abs(0, i)),
                    recent_contact_y_filter[i].CalculateAverage(state.foot_pos_abs(1, i)),
                    recent_contact_z_filter[i].CalculateAverage(state.foot_pos_abs(2, i));
        }
    }

    // std::cout << "foot_pos_recent_contact z: " << state.foot_pos_recent_contact.block<1, 4>(2, 0) << std::endl;

    state.foot_forces_kin = foot_forces_kin;
}

void A1RobotControl::update_plan(A1CtrlStates &state, double dt) {
    state.counter += 1;
    if (!state.movement_mode) {
        // movement_mode == 0, standstill with all feet in contact with ground
        for (bool &plan_contact: state.plan_contacts) plan_contact = true;
        state.gait_counter_reset();
    } else {
        // movement_mode == 1, walk
        for (int i = 0; i < NUM_LEG; ++i) {
            state.gait_counter(i) = state.gait_counter(i) + state.gait_counter_speed(i);
            state.gait_counter(i) = std::fmod(state.gait_counter(i), state.counter_per_gait);
            if (state.gait_counter(i) <= state.counter_per_swing) {
                state.plan_contacts[i] = true;
            } else {
                state.plan_contacts[i] = false;
            }
        }
    }

    // update foot plan: state.foot_pos_target_world
    Eigen::Vector3d lin_vel_world = state.root_lin_vel; // world frame linear velocity
    Eigen::Vector3d lin_vel_rel = state.root_rot_mat_z.transpose() * lin_vel_world; // robot body frame linear velocity

    // Raibert Heuristic, calculate foothold position
    state.foot_pos_target_rel = state.default_foot_pos;
    for (int i = 0; i < NUM_LEG; ++i) {
        double delta_x =
                std::sqrt(std::abs(state.default_foot_pos(2)) / 9.8) * (lin_vel_rel(0) - state.root_lin_vel_d(0)) +
                ((state.counter_per_swing / state.gait_counter_speed(i)) * state.control_dt) / 2.0 *
                state.root_lin_vel_d(0);
        double delta_y =
                std::sqrt(std::abs(state.default_foot_pos(2)) / 9.8) * (lin_vel_rel(1) - state.root_lin_vel_d(1)) +
                ((state.counter_per_swing / state.gait_counter_speed(i)) * state.control_dt) / 2.0 *
                state.root_lin_vel_d(1);

        if (delta_x < -FOOT_DELTA_X_LIMIT) {
            delta_x = -FOOT_DELTA_X_LIMIT;
        }
        if (delta_x > FOOT_DELTA_X_LIMIT) {
            delta_x = FOOT_DELTA_X_LIMIT;
        }
        if (delta_y < -FOOT_DELTA_Y_LIMIT) {
            delta_y = -FOOT_DELTA_Y_LIMIT;
        }
        if (delta_y > FOOT_DELTA_Y_LIMIT) {
            delta_y = FOOT_DELTA_Y_LIMIT;
        }

        state.foot_pos_target_rel(0, i) += delta_x;
        state.foot_pos_target_rel(1, i) += delta_y;

        state.foot_pos_target_abs.block<3, 1>(0, i) = state.root_rot_mat * state.foot_pos_target_rel.block<3, 1>(0, i);
        state.foot_pos_target_world.block<3, 1>(0, i) = state.foot_pos_target_abs.block<3, 1>(0, i) + state.root_pos;
    }
}

void A1RobotControl::generate_swing_legs_ctrl(A1CtrlStates &state, double dt) {
    state.joint_torques.setZero();

    // get current foot pos and target foot pose
    Eigen::Matrix<double, 3, NUM_LEG> foot_pos_cur;
    Eigen::Matrix<double, 3, NUM_LEG> foot_vel_cur;
    Eigen::Matrix<float, 1, NUM_LEG> spline_time;
    spline_time.setZero();
    Eigen::Matrix<double, 3, NUM_LEG> foot_pos_target;
    foot_pos_target.setZero();
    Eigen::Matrix<double, 3, NUM_LEG> foot_vel_target;
    foot_vel_target.setZero();
    Eigen::Matrix<double, 3, NUM_LEG> foot_pos_error;
    Eigen::Matrix<double, 3, NUM_LEG> foot_vel_error;

    // the foot force of swing foot and stance foot, both are in robot frame
    Eigen::Matrix<double, 3, NUM_LEG> foot_forces_kin;
    Eigen::Matrix<double, 3, NUM_LEG> foot_forces_grf;

    for (int i = 0; i < NUM_LEG; ++i) {
        foot_pos_cur.block<3, 1>(0, i) = state.root_rot_mat_z.transpose() * state.foot_pos_abs.block<3, 1>(0, i);

        // from foot_pos_cur to foot_pos_final computes an intermediate point using BezierUtils
        if (state.gait_counter(i) <= state.counter_per_swing) {
            // stance foot
            spline_time(i) = 0.0;
            // in this case the foot should be stance
            // keep refreshing foot_pos_start in stance mode
            state.foot_pos_start.block<3, 1>(0, i) = foot_pos_cur.block<3, 1>(0, i);
        } else {
            // in this case the foot should be swing
            spline_time(i) = float(state.gait_counter(i) - state.counter_per_swing) / float(state.counter_per_swing);
        }

        foot_pos_target.block<3, 1>(0, i) = bezierUtils[i].get_foot_pos_curve(spline_time(i),
                                                                              state.foot_pos_start.block<3, 1>(0, i),
                                                                              state.foot_pos_target_rel.block<3, 1>(0, i),
                                                                              0.0);

        foot_vel_cur.block<3, 1>(0, i) = (foot_pos_cur.block<3, 1>(0, i) - state.foot_pos_rel_last_time.block<3, 1>(0, i)) / dt;
        state.foot_pos_rel_last_time.block<3, 1>(0, i) = foot_pos_cur.block<3, 1>(0, i);

        foot_vel_target.block<3, 1>(0, i) = (foot_pos_target.block<3, 1>(0, i) - state.foot_pos_target_last_time.block<3, 1>(0, i)) / dt;
        state.foot_pos_target_last_time.block<3, 1>(0, i) = foot_pos_target.block<3, 1>(0, i);

        foot_pos_error.block<3, 1>(0, i) = foot_pos_target.block<3, 1>(0, i) - foot_pos_cur.block<3, 1>(0, i);
        foot_vel_error.block<3, 1>(0, i) = foot_vel_target.block<3, 1>(0, i) - foot_vel_cur.block<3, 1>(0, i);
        foot_forces_kin.block<3, 1>(0, i) = foot_pos_error.block<3, 1>(0, i).cwiseProduct(state.kp_foot.block<3, 1>(0, i)) +
                                            foot_vel_error.block<3, 1>(0, i).cwiseProduct(state.kd_foot.block<3, 1>(0, i));
    }
    state.foot_pos_cur = foot_pos_cur;

    // detect early contact
    bool last_contacts[NUM_LEG];

    for (int i = 0; i < NUM_LEG; ++i) {
        if (state.gait_counter(i) <= state.counter_per_swing * 1.5) {
            state.early_contacts[i] = false;
        }
        if (!state.plan_contacts[i] &&
            (state.gait_counter(i) > state.counter_per_swing * 1.5) &&
            (state.foot_force(i) > FOOT_FORCE_LOW)) {
            state.early_contacts[i] = true;
        }

        // actual contact
        last_contacts[i] = state.contacts[i];
        state.contacts[i] = state.plan_contacts[i] || state.early_contacts[i];

        // record recent contact position if the foot is in touch with the ground
        if (state.contacts[i]) {
//            state.foot_pos_recent_contact.block<3, 1>(0, i) = state.root_rot_mat.transpose() * (state.foot_pos_world.block<3, 1>(0, i));
//            state.foot_pos_recent_contact.block<3, 1>(0, i) = state.foot_pos_abs.block<3, 1>(0, i);
            state.foot_pos_recent_contact.block<3, 1>(0, i)
                    << recent_contact_x_filter[i].CalculateAverage(state.foot_pos_abs(0, i)),
                    recent_contact_y_filter[i].CalculateAverage(state.foot_pos_abs(1, i)),
                    recent_contact_z_filter[i].CalculateAverage(state.foot_pos_abs(2, i));
        }
    }

    // std::cout << "foot_pos_recent_contact z: " << state.foot_pos_recent_contact.block<1, 4>(2, 0) << std::endl;

    state.foot_forces_kin = foot_forces_kin;
}

void A1RobotControl::compute_joint_torques(A1CtrlStates &state) {
    Eigen::Matrix<double, NUM_DOF, 1> joint_torques;
    joint_torques.setZero();
    mpc_init_counter++;
    // for the first 10 ticks, just return zero torques.
    if (mpc_init_counter < 10) {
        state.joint_torques = joint_torques;
    } else {
        // for each leg, if it is a swing leg (contact[i] is false), use foot_force_kin to get joint_torque
        // for each leg, if it is a stance leg (contact[i] is true), use foot_forces_grf to get joint_torque
        for (int i = 0; i < NUM_LEG; ++i) {
            Eigen::Matrix3d jac = state.j_foot.block<3, 3>(3 * i, 3 * i);
            if (state.contacts[i]) {
                // stance leg
                joint_torques.segment<3>(i * 3) = jac.transpose() * -state.foot_forces_grf.block<3, 1>(0, i);
            } else {
                // swing leg
                Eigen::Vector3d force_tgt = state.km_foot.cwiseProduct(state.foot_forces_kin.block<3, 1>(0, i));
                joint_torques.segment<3>(i * 3) = jac.lu().solve(force_tgt);   // jac * tau = F
            }
        }
        // gravity compensation
        joint_torques += state.torques_gravity;

        // prevent nan
        for (int i = 0; i < 12; ++i) {
            if (!isnan(joint_torques[i]))
                state.joint_torques[i] = joint_torques[i];
        }
    }
}

void A1RobotControl::compute_joint_torques_kin(A1CtrlStates &state) {
    Eigen::Matrix<double, NUM_DOF, 1> joint_torques;
    joint_torques.setZero();
    mpc_init_counter++;
    // for the first 10 ticks, just return zero torques.
    if (mpc_init_counter < 10) {
        state.joint_torques = joint_torques;
    } else {
        // for each leg, if it is a swing leg (contact[i] is false), use foot_force_kin to get joint_torque
        // for each leg, if it is a stance leg (contact[i] is true), use foot_forces_grf to get joint_torque
        for (int i = 0; i < NUM_LEG; ++i) {
            Eigen::Matrix3d jac = state.j_foot.block<3, 3>(3 * i, 3 * i);
            if (state.contacts[i]) {
                // stance leg
                joint_torques.segment<3>(i * 3) = jac.transpose() * -state.foot_forces_grf.block<3, 1>(0, i);
            } else {
                // swing leg
                Eigen::Vector3d force_tgt = state.km_foot.cwiseProduct(state.foot_forces_kin.block<3, 1>(0, i));
                joint_torques.segment<3>(i * 3) = jac.lu().solve(force_tgt);   // jac * tau = F
            }
        }
        // gravity compensation
        joint_torques += state.torques_gravity;

        // prevent nan
        for (int i = 0; i < 12; ++i) {
            if (!isnan(joint_torques[i]))
                state.joint_torques[i] = joint_torques[i];
        }
    }
}

Eigen::Matrix<double, 3, NUM_LEG> A1RobotControl::compute_grf(A1CtrlStates &state, double dt) {
    Eigen::Matrix<double, 3, NUM_LEG> foot_forces_grf;
    // first get parameters needed to construct the solver hessian and gradient
    // use euler angle to get desired angle
    Eigen::Vector3d euler_error = state.root_euler_d - state.root_euler;

    // limit euler error to pi/2
    if (euler_error(2) > 3.1415926 * 1.5) {
        euler_error(2) = state.root_euler_d(2) - 3.1415926 * 2 - state.root_euler(2);
    } else if (euler_error(2) < -3.1415926 * 1.5) {
        euler_error(2) = state.root_euler_d(2) + 3.1415926 * 2 - state.root_euler(2);
    }

    // only do terrain adaptation in MPC
    if (state.stance_leg_control_type == 1) {
        Eigen::Vector3d surf_coef = compute_walking_surface(state);
        Eigen::Vector3d flat_ground_coef;
        flat_ground_coef << 0, 0, 1;
        double terrain_angle = 0;
        // only record terrain angle when the body is high
        if (state.root_pos[2] > 0.1) {
            terrain_angle = terrain_angle_filter.CalculateAverage(Utils::cal_dihedral_angle(flat_ground_coef, surf_coef));
        } else {
            terrain_angle = 0;
        }

        if (terrain_angle > 0.5) {
            terrain_angle = 0.5;
        }
        if (terrain_angle < -0.5) {
            terrain_angle = -0.5;
        }

        // FL, FR, RL, RR
        double F_R_diff = state.foot_pos_recent_contact(2, 0) + state.foot_pos_recent_contact(2, 1) - state.foot_pos_recent_contact(2, 2) -
                        state.foot_pos_recent_contact(2, 3);

        if (F_R_diff > 0.05) {
        state.root_euler_d[1] = -terrain_angle;
        } else {
        state.root_euler_d[1] = terrain_angle;
        }


        std_msgs::Float64 terrain_angle_msg;
        terrain_angle_msg.data = terrain_angle * (180 / 3.1415926);
        pub_terrain_angle.publish(terrain_angle_msg); // publish in deg
        // std::cout << "desire pitch in deg: " << state.root_euler_d[1] * (180 / 3.1415926) << std::endl;
        // std::cout << "terrain angle: " << terrain_angle << std::endl;

        // save calculated terrain pitch angle
        // TODO: limit terrain pitch angle to -30 to 30? 
        state.terrain_pitch_angle = terrain_angle;
    }
    if (state.stance_leg_control_type == 0) { // 0: QP
        // desired acc in world frame
        root_acc.setZero();

        root_acc.block<3, 1>(0, 0) = state.kp_linear.cwiseProduct(state.root_pos_d - state.root_pos);

        root_acc.block<3, 1>(0, 0) += state.root_rot_mat * state.kd_linear.cwiseProduct(
                state.root_lin_vel_d - state.root_rot_mat.transpose() * state.root_lin_vel);

        root_acc.block<3, 1>(3, 0) = state.kp_angular.cwiseProduct(euler_error);

        root_acc.block<3, 1>(3, 0) += state.kd_angular.cwiseProduct(
                state.root_ang_vel_d - state.root_rot_mat.transpose() * state.root_ang_vel);

        // add gravity
        root_acc(2) += state.robot_mass * 9.81;

        // Create inverse inertia matrix: A
        Eigen::Matrix<double, 6, DIM_GRF> inertia_inv;
        for (int i = 0; i < NUM_LEG; ++i) {
            inertia_inv.block<3, 3>(0, i * 3).setIdentity();
            // TODO: confirm this should be root_rot_mat instead of root_rot_mat
            inertia_inv.block<3, 3>(3, i * 3) = state.root_rot_mat_z.transpose() * Utils::skew(state.foot_pos_abs.block<3, 1>(0, i));
        }
        Eigen::Matrix<double, DIM_GRF, DIM_GRF> dense_hessian;
        dense_hessian.setIdentity();
        dense_hessian *= R;
        dense_hessian += inertia_inv.transpose() * Q * inertia_inv;
        hessian = dense_hessian.sparseView();
        // accidentally wrote this as -2* before. Huge problem
        gradient.block<3 * NUM_LEG, 1>(0, 0) = -inertia_inv.transpose() * Q * root_acc;

        // adjust bounds according to contact flag
        for (int i = 0; i < NUM_LEG; ++i) {
            double c_flag = state.contacts[i] ? 1.0 : 0.0;
            lowerBound(i) = c_flag * F_min;
            upperBound(i) = c_flag * F_max;
        }

        // instantiate the solver
        OsqpEigen::Solver solver;
        // settings
        solver.settings()->setVerbosity(false);
        solver.settings()->setWarmStart(false);
        solver.data()->setNumberOfVariables(3 * NUM_LEG);
        solver.data()->setNumberOfConstraints(NUM_LEG + 4 * NUM_LEG);
        solver.data()->setLinearConstraintsMatrix(linearMatrix);
        solver.data()->setHessianMatrix(hessian);
        solver.data()->setGradient(gradient);
        solver.data()->setLowerBound(lowerBound);
        solver.data()->setUpperBound(upperBound);

        auto t1 = std::chrono::high_resolution_clock::now();
        solver.initSolver();
        auto t2 = std::chrono::high_resolution_clock::now();
        solver.solve();
        auto t3 = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double, std::milli> ms_double_1 = t2 - t1;
        std::chrono::duration<double, std::milli> ms_double_2 = t3 - t2;

        // std::cout << "qp solver init time: " << ms_double_1.count() << "ms; solve time: " << ms_double_2.count() << "ms" << std::endl;

        Eigen::VectorXd QPSolution = solver.getSolution(); //12x1
        for (int i = 0; i < NUM_LEG; ++i) {
            // the QP solves for world frame force
            // here we convert the force into robot frame
            foot_forces_grf.block<3, 1>(0, i) = state.root_rot_mat.transpose() * QPSolution.segment<3>(i * 3);
        }

    } else if (state.stance_leg_control_type == 1) { // 1: MPC
        ConvexMpc mpc_solver = ConvexMpc(state.q_weights, state.r_weights);
        mpc_solver.reset();

        // initialize the mpc state at the first time step
        // state.mpc_states.resize(13);
        state.mpc_states << state.root_euler[0], state.root_euler[1], state.root_euler[2],
                state.root_pos[0], state.root_pos[1], state.root_pos[2],
                state.root_ang_vel[0], state.root_ang_vel[1], state.root_ang_vel[2],
                state.root_lin_vel[0], state.root_lin_vel[1], state.root_lin_vel[2],
                -9.8;

        // previously we use dt passed by outer thread. It turns out that this dt is not stable on hardware.
        // if the thread is slowed down, dt becomes large, then MPC will output very large force and torque value
        // which will cause over current. Here we use a new mpc_dt, this should be roughly close to the average dt
        // of thread 1 
        double mpc_dt = 0.0025;

        // in simulation, use dt has no problem
        if (use_sim_time == true) {
            mpc_dt = dt;
        }

        // initialize the desired mpc states trajectory
        state.root_lin_vel_d_world = state.root_rot_mat * state.root_lin_vel_d;
        // state.mpc_states_d.resize(13 * PLAN_HORIZON);
        for (int i = 0; i < PLAN_HORIZON; ++i) {
            state.mpc_states_d.segment(i * 13, 13)
                    <<
                    state.root_euler_d[0],
                    state.root_euler_d[1],
                    state.root_euler[2] + state.root_ang_vel_d[2] * mpc_dt * (i + 1),
                    state.root_pos[0] + state.root_lin_vel_d_world[0] * mpc_dt * (i + 1),
                    state.root_pos[1] + state.root_lin_vel_d_world[1] * mpc_dt * (i + 1),
                    state.root_pos_d[2],
                    state.root_ang_vel_d[0],
                    state.root_ang_vel_d[1],
                    state.root_ang_vel_d[2],
                    state.root_lin_vel_d_world[0],
                    state.root_lin_vel_d_world[1],
                    0,
                    -9.8;
        }

        // a single A_c is computed for the entire reference trajectory
        auto t1 = std::chrono::high_resolution_clock::now();
        mpc_solver.calculate_A_mat_c(state.root_euler);

        // for each point in the reference trajectory, an approximate B_c matrix is computed using desired values of euler angles and feet positions
        // from the reference trajectory and foot placement controller
        // state.foot_pos_abs_mpc = state.foot_pos_abs;
        auto t2 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < PLAN_HORIZON; i++) {
            // calculate current B_c matrix
            mpc_solver.calculate_B_mat_c(state.robot_mass,
                                         state.a1_trunk_inertia,
                                         state.root_rot_mat,
                                         state.foot_pos_abs);
            // state.foot_pos_abs_mpc.block<3, 1>(0, 0) = state.foot_pos_abs_mpc.block<3, 1>(0, 0) - state.root_lin_vel_d * mpc_dt;
            // state.foot_pos_abs_mpc.block<3, 1>(0, 1) = state.foot_pos_abs_mpc.block<3, 1>(0, 1) - state.root_lin_vel_d * mpc_dt;
            // state.foot_pos_abs_mpc.block<3, 1>(0, 2) = state.foot_pos_abs_mpc.block<3, 1>(0, 2) - state.root_lin_vel_d * mpc_dt;
            // state.foot_pos_abs_mpc.block<3, 1>(0, 3) = state.foot_pos_abs_mpc.block<3, 1>(0, 3) - state.root_lin_vel_d * mpc_dt;

            // state space discretization, calculate A_d and current B_d
            mpc_solver.state_space_discretization(mpc_dt);

            // store current B_d matrix
            mpc_solver.B_mat_d_list.block<13, 12>(i * 13, 0) = mpc_solver.B_mat_d;
        }

        // calculate QP matrices
        auto t3 = std::chrono::high_resolution_clock::now();
        mpc_solver.calculate_qp_mats(state);

        // solve
        auto t4 = std::chrono::high_resolution_clock::now();
        if (!solver.isInitialized()) {
            solver.settings()->setVerbosity(false);
            solver.settings()->setWarmStart(true);
            solver.data()->setNumberOfVariables(NUM_DOF * PLAN_HORIZON);
            solver.data()->setNumberOfConstraints(MPC_CONSTRAINT_DIM * PLAN_HORIZON);
            solver.data()->setLinearConstraintsMatrix(mpc_solver.linear_constraints);
            solver.data()->setHessianMatrix(mpc_solver.hessian);
            solver.data()->setGradient(mpc_solver.gradient);
            solver.data()->setLowerBound(mpc_solver.lb);
            solver.data()->setUpperBound(mpc_solver.ub);
            solver.initSolver();
        } else {
            solver.updateHessianMatrix(mpc_solver.hessian);
            solver.updateGradient(mpc_solver.gradient);
            solver.updateLowerBound(mpc_solver.lb);
            solver.updateUpperBound(mpc_solver.ub);
        }
        auto t5 = std::chrono::high_resolution_clock::now();
        solver.solve();
        auto t6 = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double, std::milli> ms_double_1 = t2 - t1;
        std::chrono::duration<double, std::milli> ms_double_2 = t3 - t2;
        std::chrono::duration<double, std::milli> ms_double_3 = t4 - t3;
        std::chrono::duration<double, std::milli> ms_double_4 = t5 - t4;
        std::chrono::duration<double, std::milli> ms_double_5 = t6 - t5;

//        std::cout << "mpc cal A_mat_c: " << ms_double_1.count() << "ms" << std::endl;
//        std::cout << "mpc cal B_mat_d_list: " << ms_double_2.count() << "ms" << std::endl;
//        std::cout << "mpc cal qp mats: " << ms_double_3.count() << "ms" << std::endl;
//        std::cout << "mpc init time: " << ms_double_4.count() << "ms" << std::endl;
//        std::cout << "mpc solve time: " << ms_double_5.count() << "ms" << std::endl << std::endl;

        Eigen::VectorXd solution = solver.getSolution();
        // std::cout << solution.transpose() << std::endl;

        for (int i = 0; i < NUM_LEG; ++i) {
            if (!isnan(solution.segment<3>(i * 3).norm()))
                foot_forces_grf.block<3, 1>(0, i) = state.root_rot_mat.transpose() * solution.segment<3>(i * 3);
        }
    }
    return foot_forces_grf;
}

Eigen::Vector3d A1RobotControl::compute_walking_surface(A1CtrlStates &state) {
    Eigen::Matrix<double, NUM_LEG, 3> W;
    Eigen::VectorXd foot_pos_z;
    Eigen::Vector3d a;
    Eigen::Vector3d surf_coef;

    W.block<NUM_LEG, 1>(0, 0).setOnes();
    W.block<NUM_LEG, 2>(0, 1) = state.foot_pos_recent_contact.block<2, NUM_LEG>(0, 0).transpose();

    foot_pos_z.resize(NUM_LEG);
    foot_pos_z = state.foot_pos_recent_contact.block<1, NUM_LEG>(2, 0).transpose();

    a = Utils::pseudo_inverse(W.transpose() * W) * W.transpose() * foot_pos_z;
    // surface: a1 * x + a2 * y - z + a0 = 0, coefficient vector: [a1, a2, -1]
    surf_coef << a[1], a[2], -1;
    return surf_coef;
}
