#include <ros/ros.h>

#include <math.h>

#include <std_msgs/Int8.h>
#include <std_msgs/UInt16MultiArray.h>

#include <sensor_msgs/CompressedImage.h>

#include <drv_msgs/recognized_target.h>
#include <drv_msgs/recognize.h>

#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>

#include "search.h"
#include "targetselect.h"
#include "smoothservo.h"
//#include "segment.h"

using namespace std;

string param_target_label = "/vision/target/label";

ros::Publisher servoPubSearch_; // publish servo angle rotated for search
ros::Publisher searchPubStatus_;
ros::Publisher searchPubTarget_; // publish target info to track function
//image_transport::Publisher searchPubImage_; // publish labeled image for user judgement

enum ModeType{m_wander, m_search, m_track};
int modeType_ = m_wander;
string param_running_mode = "/status/running_mode";


// servo position angle
string param_servo_pitch = "/status/servo/pitch";
string param_servo_yaw = "/status/servo/yaw";
int yawAngle_ = 90;
int pitchAngle_ = 70;

string targetLabel_;

// global status control params
bool servoInitialized_ = false;
int searchResult_ = 0; // feedback, -1 indicate no result around, 0 indicate current no result, 1 has result
int selectedNum_ = 0; // selected target number

sensor_msgs::Image img_msg_;

cv_bridge::CvImageConstPtr imagePtr_;
cv_bridge::CvImageConstPtr depthPtr_;


void imageCallback(const sensor_msgs::ImageConstPtr & image_msg)
{
		if (modeType_ != m_search)
				{
						return;
				}

		img_msg_ = *image_msg;
		imagePtr_ = cv_bridge::toCvShare(image_msg, "bgr8");
}

void depthCallback(const sensor_msgs::ImageConstPtr& depth_msg)
{
		if (modeType_ != m_search)
				{
						return;
				}

		if (depth_msg->height != 480)
				{
						ROS_ERROR_THROTTLE(5,"Depth size is wrong.\n");
						return;
				}
		depthPtr_ = cv_bridge::toCvShare(depth_msg);
}

void resetStatus()
{
		servoInitialized_ = false;
		searchResult_ = 0;
		selectedNum_ = 0;
}

int main(int argc, char **argv)
{
		ros::init(argc, argv, "drv_search");

		ros::NodeHandle nh;
		ros::NodeHandle pnh;
		ros::NodeHandle rgb_nh(nh, "rgb");
//		ros::NodeHandle depth_nh(nh, "depth");
		ros::NodeHandle rgb_pnh(pnh, "rgb");
//		ros::NodeHandle depth_pnh(pnh, "depth");
		image_transport::ImageTransport it_rgb_sub(rgb_nh);
//		image_transport::ImageTransport depth_it(depth_nh);
		image_transport::TransportHints hints_rgb("compressed", ros::TransportHints(), rgb_pnh);

		searchPubStatus_ = nh.advertise<std_msgs::Int8>("status/search/feedback", 1);
		searchPubTarget_ = nh.advertise<drv_msgs::recognized_target>("search/recognized_target", 1, true);

//		ros::Subscriber sub_rgb = nh.subscribe("/rgb/image/compressed", 1, imageCallback);
		image_transport::Subscriber sub_rgb = it_rgb_sub.subscribe("/rgb/image", 1, imageCallback, hints_rgb);
//		ros::Subscriber sub_depth = nh.subscribe("/depth/image_raw", 1, depthCallback );

		ros::ServiceClient client = nh.serviceClient<drv_msgs::recognize>("drv_recognize");

		ROS_INFO("Search function initialized!\n");

		Search sh;
		TargetSelect ts;
		SmoothServo ss;
//		Segment sg;
//		Utilities ut;

		while (ros::ok())
				{
						if (ros::param::has(param_servo_pitch))
								ros::param::get(param_servo_pitch, pitchAngle_);

						if (ros::param::has(param_servo_yaw))
								ros::param::get(param_servo_yaw, yawAngle_);


						if (ros::param::has(param_running_mode))
								ros::param::get(param_running_mode, modeType_);

						if (modeType_ != m_search)
								{
										resetStatus();
										continue;
								}

						// get the servo pitch to standard pose for every search
						if (!servoInitialized_)
								{
										ss.getCurrentServoAngle(pitchAngle_, yawAngle_);
										ss.moveServoTo(70, yawAngle_);
										pitchAngle_ = 70;
										servoInitialized_ = true;
								}

						if (ros::param::has(param_target_label))
								ros::param::get(param_target_label, targetLabel_);

						ros::spinOnce();

						// call recognize service
						drv_msgs::recognize srv;

						if (img_msg_.height != 480)
								continue;

						srv.request.img_in = img_msg_;

						std::vector<std_msgs::UInt16MultiArray> bbox_arrays_;
						int choosed_id = -1;

						if (client.call(srv))
								{
										// call user select service if selectnum != 1, i.e. no target selected or more than 1 target candidates
										if (selectedNum_ != 1)
												{
														cv_bridge::CvImagePtr img_labeled;
														selectedNum_ = ts.select(targetLabel_, srv.response, img_msg_, img_labeled, choosed_id);
												}

										int a_s = srv.response.obj_info.bbox_arrays.size();
										bbox_arrays_.resize(a_s);
										bbox_arrays_ = srv.response.obj_info.bbox_arrays;

										if (selectedNum_)
												{
														searchResult_ = 1;
												}
										else
												{
														searchResult_ = 0;
												}
								}
						else
								{
										ROS_ERROR("Failed to call recognize service.");
										searchResult_ = 0;
								}

						ss.getCurrentServoAngle(pitchAngle_, yawAngle_);

						if (!searchResult_)
								{
										int pitch_angle = pitchAngle_;
										int yaw_angle = yawAngle_;
										bool has_next_pos = sh.getNextPosition(yaw_angle, pitch_angle);
										ROS_INFO("Searching at angle: yaw = %d, pitch = %d.\n", yaw_angle, pitch_angle);

										if (!has_next_pos)
												{
														ROS_WARN("Searching around didn't find target.\n");
														searchResult_ = -1;
												}

										// turn camera to the next search direction (according to the criteria above)
										ss.moveServoTo(pitch_angle, yaw_angle);
								}
						else
								{
										// label the detected target with bounding area
//										sg.segment(imagePtr_, depthPtr_);

										// publish goal info for tracking
										if (srv.response.obj_info.bbox_arrays.size() > choosed_id)
												{
														drv_msgs::recognized_target tgt;
														tgt.header = srv.response.obj_info.header;
														tgt.tgt_bbox_array = srv.response.obj_info.bbox_arrays[choosed_id];
														tgt.label = srv.response.obj_info.labels[choosed_id];
														searchPubTarget_.publish(tgt);
												}
										modeType_ = m_wander;
								}

						std_msgs::Int8 flag;
						flag.data = searchResult_;
						searchPubStatus_.publish(flag);
				}

		return 0;
}
