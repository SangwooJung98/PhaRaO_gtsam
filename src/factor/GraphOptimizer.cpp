#include <factor/GraphOptimizer.hpp>

GraphOptimizer::GraphOptimizer(ros::NodeHandle nh, DataContainer* dc)
							: nh_(nh), dc_(dc), FactorConstructor(dc)
{
	// ROS
	nh_.getParam("odom_factor_cost_threshold", odom_threshold_);
	nh_.getParam("keyframe_factor_cost_threshold", keyf_threshold_);

	pub_opt_odom_ 	= nh.advertise<nav_msgs::Odometry>("/opt_odom", 1000);
	pub_odom_ 		= nh.advertise<nav_msgs::Odometry>("/odom", 1000);

	// GTSAM
	ISAM2Params parameters;
	parameters.relinearizeThreshold = 0.01;
	parameters.relinearizeSkip = 1;
	isam2 = new ISAM2(parameters);

	Eigen::AngleAxisd rollAngle(0.0, Eigen::Vector3d::UnitX());   //M_PI
	Eigen::AngleAxisd pitchAngle(0.0, Eigen::Vector3d::UnitY());
	Eigen::AngleAxisd yawAngle(0, Eigen::Vector3d::UnitZ());
	Eigen::Quaternion<double> init_q = yawAngle * pitchAngle * rollAngle;

	Pose2 prior_pose = gtsam::Pose2(Rot2(0), gtsam::Point2(.0,.0));

	initial_values.insert(X(pose_count), prior_pose);

	prior_noise_model_ = noiseModel::Diagonal::Sigmas((Vector(3) << 0.01, 0.01, 0.001).finished());
	odom_noise_model_ = noiseModel::Diagonal::Sigmas((Vector(3) << 1, 1, 1e-1).finished());  // m, m, rad
	key_noise_model_ = noiseModel::Diagonal::Sigmas((Vector(3) << 1,1,1e-3).finished());
	auto rot_vector = (Vector(1) << 1e-2).finished();
	rot_noise_model_ = noiseModel::Diagonal::Sigmas(rot_vector);

	// Add prior factor to the graph.
	poseGraph->addPrior(X(pose_count), prior_pose, prior_noise_model_);
}

GraphOptimizer::~GraphOptimizer()
{

}

void
GraphOptimizer::optimize()
{
	
	imshow("Coarse cart.",*(dc_->window_list_cart.end()-1));

	// initialize
	if(dc_->initialized == false){
		dc_->initialized = true;
	}
	else
	{
		cout << "=================" << endl;

		bool valid = generateOdomFactor();


		if(valid){			
			generateKeyfFactor();
		}


		poseGraph->print();
	}

}

bool
GraphOptimizer::generateOdomFactor()
{
	// Phase correlation (coarse to fine)
	int num = dc_->window_list.size() - 1;

	int index = 0;

	cout << "---- Odometry Factor ----" << endl;
	// Odom. Frame factor 
	for(int ii = 1; ii <= num; ii++) {
		int begin = num-ii;
		int end = num;
		dc_->odom_list[num-1] = factorGeneration(begin, end);

		// Cost calculation
		double o_yx = atan2(dc_->odom_list[num-1][1], dc_->odom_list[num-1][0]);
		double o_theta = dc_->odom_list[num-1][2];
		double cost = exp(-abs(o_yx + o_theta));
		double norm = sqrt(pow(dc_->odom_list[num-1][1],2) + pow(dc_->odom_list[num-1][0],2));
		
		cout << "[" << pose_count+1-ii << " & " << pose_count+1 << "] Cost: " << cost 
			<< ", o_theta: " << o_theta/M_PI*180.0 << ", o_yx: " << o_yx << endl;

		if(norm < RESOL){ // There is no change.
			ROS_WARN("Negligible Change.");
			dc_->window_list.erase(dc_->window_list.end()-1);
			dc_->window_list_cart.erase(dc_->window_list_cart.end()-1);
			dc_->window_list_cart_f.erase(dc_->window_list_cart_f.end()-1);
			num--;
			dc_->stamp_list.erase(dc_->stamp_list.end()-1);

			return false;
		} else {
			if(cost > odom_threshold_) {
				pose_count ++;
				pose_node_nums.push_back(pose_count);
				if(pose_count-1 == pose_values.size()){
					pose_values.push_back(current_pose);
				}
				cout << "pose num : " << pose_values.size() << endl;
				base_pose = pose_values.at(pose_count-ii);

				Eigen::Vector2d odom_tr(base_pose(0), base_pose(1));
				Eigen::Rotation2D<double> odom_rot(base_pose(2));
				Eigen::Vector2d tr_delta(dc_->odom_list[num-1][0], dc_->odom_list[num-1][1]);
				Eigen::Rotation2D<double> rot_delta(dc_->odom_list[num-1][2]);

				odom_rot = odom_rot * rot_delta;
				odom_tr = odom_tr + odom_rot * tr_delta;

				current_pose << odom_tr(0), odom_tr(1), odom_rot.angle();
				Point2 prop_point = gtsam::Point2(current_pose(0),current_pose(1));
				Rot2 orien = Rot2(current_pose(2));
				Pose2 prop_pose = gtsam::Pose2(orien, prop_point);

				Pose2 odom_delta = gtsam::Pose2(dc_->odom_list[num-1][0], dc_->odom_list[num-1][1], dc_->odom_list[num-1][2]); // x,y,theta
				poseGraph->add(BetweenFactor<Pose2>(X(pose_count-ii), X(pose_count), odom_delta, odom_noise_model_));
				
				initial_values.insert(X(pose_count), prop_pose);


				Eigen::AngleAxisd rollAngle(0.0, Eigen::Vector3d::UnitX());   //M_PI
				Eigen::AngleAxisd pitchAngle(0.0, Eigen::Vector3d::UnitY());
				Eigen::AngleAxisd yawAngle(odom_rot.angle(), Eigen::Vector3d::UnitZ());
				Eigen::Quaternion<double> gtsam_quat = yawAngle * pitchAngle * rollAngle;

				publishOdom(prop_pose, gtsam_quat);

				cout << "Current pose number: " << pose_count << endl;
				cout << "Best Matching pair: " << pose_count-ii << " & " << pose_count <<endl;
				cout << "x: " << dc_->odom_list[num-1][0] << ", y: " << dc_->odom_list[num-1][1]
					<< ", theta: " << dc_->odom_list[num-1][2] << endl;
				cout << "-------------------------" << endl;

				return true;
			}

		}
	}

	ROS_WARN("Negligible Change.");
	dc_->window_list.erase(dc_->window_list.end()-1);
	dc_->window_list_cart.erase(dc_->window_list_cart.end()-1);
	dc_->window_list_cart_f.erase(dc_->window_list_cart_f.end()-1);
	num--;
	dc_->stamp_list.erase(dc_->stamp_list.end()-1);

	return false;
}

void
GraphOptimizer::generateKeyfFactor()
{
	static int cnt = 0;
	int num = dc_->window_list.size()-1;

	dc_->del_list[num-1] = factorGeneration(0,num);

	cout << "---- Keyframe Factor ----" << endl;

	/////////////////////////////////////////////////////////
	////// Cost (compared with a previous keyframe) calculation
	double d_yx = atan2(dc_->del_list[num-1][1],dc_->del_list[num-1][0]);
	double d_theta = dc_->del_list[num-1][2];
	norm_v[num-1]=sqrt(dc_->del_list[num-1][1]*dc_->del_list[num-1][1]+dc_->del_list[num-1][0]*dc_->del_list[num-1][0]);
	norm_w[num-1]=d_theta*180.0/M_PI;

	atv[num-1] = exp(-abs(d_yx + d_theta));

	// Heuristic constraints
	if(norm_v[num-1] > 1.0){
		// 1. Forward (x-axis in our sensor coordinate) motion is dominant at nonholonimc veheicle.
		if(abs(dc_->del_list[num-1][1]) > 2.0)	
			atv[num-1] = 0;
		// 2. Bounded angular motion.
		if(abs(norm_w[num-1]) > 90.0)
			atv[num-1] = 0;
	}

	/////////////////////////////////////////////////////////
	////// Sorting by costs (ascending)
    vector<int> y(num);
    size_t n(0);

    generate(std::begin(y), std::end(y), [&]{ return n++; });

    std::sort(  std::begin(y), 
                std::end(y),
                [&](int i1, int i2) { return atv[i1] < atv[i2]; } );

    int iter = 0;
    for (auto v : y) {
        //cout << "(" << v << ") " ;

        cost_idx[iter] = v;
        iter++;
    }
    //cout << endl;

	/////////////////////////////////////////////////////////	
	////// Sorting by delta_theta (ascending)
    n = 0;

    generate(std::begin(y), std::end(y), [&]{ return n++; });

    std::sort(  std::begin(y), 
                std::end(y),
                [&](int i1, int i2) { return abs(norm_w[i1]) < abs(norm_w[i2]); } );

    iter = 0;
    for (auto v : y) {
        cost_iter[iter] = v;
        iter++;
    }

	/////////////////////////////////////////////////////////	

    cout << "Cost : ";
    for (int ii = 0; ii < num; ii++)
    	cout << atv[ii] << " ";
    cout << endl;

    cout << "norm_v : ";
	for (int ii = 0; ii < num; ii++)
    	cout << norm_v[ii] << " ";
    cout << endl;

    cout << "norm_w : ";
	for (int ii = 0; ii < num; ii++)
    	cout << norm_w[ii] << " ";
    cout << endl;

    cout << "Indices sorted by cost : ";
	for (int ii = 0; ii < num; ii++)
    	cout << cost_idx[ii] << " ";
    cout << endl;

    cout << "Indices sorted by delta_theta : ";
	for (int ii = 0; ii < num; ii++)
    	cout << cost_iter[ii] << " ";
    cout << endl;

	////////////////////////////////////////////////////

	if(num > 1) {
		// Constraints to decide keyframe
		// Paper II.C.2)
		bool constraint1 = (atv[num-1] < atv[num-2]) && (atv[num-1] < keyf_threshold_*atv[cost_idx[num-1]]);
		bool constraint2 = (num > 3);
		bool constraint3 = (norm_v[0] > 30.0);

		if( constraint1 || constraint2 || constraint3 ) {
			int i = num-2;
			int p_ind = num-2;

			for(int ii = num-1; ii >= 0; ii--){
				if(atv[cost_iter[ii]] > keyf_threshold_*atv[cost_idx[num-1]]){
					i = cost_iter[ii];
					break;
				}
			}
			if(i == -1)
				i = cost_idx[num-1];

			p_ind = i;

			/////////////////////////////////////////////////////////
			for(int ii = 0; ii < num; ii++) {
				if (p_ind == ii) {
					poseGraph->add(PharaoRotFactor(X(key_node), X(pose_count-num+p_ind+1), dc_->del_list[p_ind][2], rot_noise_model_));
				} else if(atv[ii] > keyf_threshold_*atv[cost_idx[num-1]]) {
					poseGraph->add(PharaoRotFactor(X(key_node), X(pose_count-num+ii+1), dc_->del_list[ii][2], rot_noise_model_));
				}
			}

			int p_size = pose_node_nums.size();
			if(p_size > 2) {
				pose_node_nums.clear();
				isam2->update(*poseGraph, initial_values);
				isam2->update();
				Values odom_result = isam2->calculateEstimate();

				poseGraph->resize(0);
    			initial_values.clear();
			
				/////////////////////////////////////////////////////////
				prev_pose = odom_result.at<Pose2>(X(key_node));
				cout << "Last Pose value:\n     x:" << prev_pose.translation().x() 
					<< "     y:"<< prev_pose.translation().y() 
					<< "     theta:"<< prev_pose.rotation().theta()<<endl;
				Eigen::AngleAxisd rollAngle(0.0, Eigen::Vector3d::UnitX());   //M_PI
				Eigen::AngleAxisd pitchAngle(0.0, Eigen::Vector3d::UnitY());
				Eigen::AngleAxisd yawAngle(prev_pose.rotation().theta(), Eigen::Vector3d::UnitZ());
				Eigen::Quaternion<double> gtsam_quat = yawAngle * pitchAngle * rollAngle;

				publishOptOdom(prev_pose, gtsam_quat);

				key_node = pose_count - num + p_ind + 1;
				window_loop += num;
				prev_pose = odom_result.at<Pose2>(X(key_node));
			}

			cv::Mat last_p,last_c,last_cf;
			auto list_iter = dc_->window_list.begin() + 1 + p_ind;
			last_p = *list_iter;
			list_iter = dc_->window_list_cart.begin() + 1 + p_ind;
			last_c = *list_iter;
			list_iter = dc_->window_list_cart_f.begin() + 1 + p_ind;
			last_cf = *list_iter;

			dc_->keyf_list.push_back(last_p);
			dc_->keyf_list_cart.push_back(last_c);
			dc_->keyf_list_cart_f.push_back(last_cf);

			dc_->window_list.clear();
			dc_->window_list_cart.clear();
			dc_->window_list_cart_f.clear();

			dc_->window_list.push_back(last_p);
			dc_->window_list_cart.push_back(last_c);
			dc_->window_list_cart_f.push_back(last_cf);

			cnt++;
		}
	}

}

void
GraphOptimizer::publishOdom(Pose2 pose, Eigen::Quaterniond quat)
{
	nav_msgs::Odometry odom;
	odom.header.frame_id = "odom";
	odom.pose.pose.position.x = pose.translation().x();
	odom.pose.pose.position.y = pose.translation().y();
	odom.pose.pose.position.z = 0;
	odom.pose.pose.orientation.w = quat.w();
	odom.pose.pose.orientation.x = quat.x();
	odom.pose.pose.orientation.y = quat.y();
	odom.pose.pose.orientation.z = quat.z();
	pub_odom_.publish(odom);
}

void
GraphOptimizer::publishOptOdom(Pose2 pose, Eigen::Quaterniond quat)
{
	nav_msgs::Odometry opt_odom;
	opt_odom.header.frame_id = "odom";
	opt_odom.pose.pose.position.x = pose.translation().x();
	opt_odom.pose.pose.position.y = pose.translation().y();
	opt_odom.pose.pose.position.z = 0;
	opt_odom.pose.pose.orientation.w = quat.w();
	opt_odom.pose.pose.orientation.x = quat.x();
	opt_odom.pose.pose.orientation.y = quat.y();
	opt_odom.pose.pose.orientation.z = quat.z();
	pub_opt_odom_.publish(opt_odom);
}