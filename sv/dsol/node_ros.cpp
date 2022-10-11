#include <geometry_msgs/PoseArray.h>
#include <geometry_msgs/TransformStamped.h>
#include <ros/ros.h>
#include <rosgraph_msgs/Clock.h>
#include <tf2_ros/transform_listener.h>
#include <tf2_ros/transform_broadcaster.h>
#include "sv/dsol/extra.h"
#include "sv/dsol/node_util.h"
#include "sv/ros1/msg_conv.h"
#include "sv/util/dataset.h"
#include "sv/util/logging.h"
#include "sv/util/ocv.h"
#include "sv/util/eigen.h"
#include "sensor_msgs/Image.h"
#include "sensor_msgs/Imu.h"
#include <cv_bridge/cv_bridge.h>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/sync_policies/exact_time.h>
#include <nav_msgs/Odometry.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>
#include <Eigen/Dense>

namespace sv::dsol {

using SE3d = Sophus::SE3d;
namespace gm = geometry_msgs;
namespace sm = sensor_msgs;
namespace vm = visualization_msgs;
namespace nm = nav_msgs;
namespace mf = message_filters;

struct NodeData {
  explicit NodeData(const ros::NodeHandle& pnh);

  void InitOdom();
  void InitRosIO();

  void PublishOdom(const std_msgs::Header& header, const Sophus::SE3d& Twc);
  void PublishCloud(const std_msgs::Header& header) const;
  void SendTransform(const gm::PoseStamped& pose_msg,
                     const std::string& child_frame);
  void imuCallback(const sm::ImuConstPtr& imu);
  void odomCallback(const nm::OdometryConstPtr& enc);
  void StereoCb(const sensor_msgs::ImageConstPtr& msgLeft, const sensor_msgs::ImageConstPtr& msgRight);
  void StereoDepthCb(const sensor_msgs::ImageConstPtr& msgLeft, const sensor_msgs::ImageConstPtr& msgRight, const sm::ImageConstPtr& msgDepth);
  void Run(cv_bridge::CvImageConstPtr cv_ptrLeft, cv_bridge::CvImageConstPtr cv_ptrRight, const ros::Time timestamp, cv_bridge::CvImageConstPtr cv_ptrDepth = {});
  void getPrediction(double& pred_x, double& pred_y, double& pred_z, double& pred_a);
  void camInfoCallback(const sm::CameraInfo& cinfo);

  double data_max_depth_{0};
  double cloud_max_depth_{100};

//   Dataset dataset_;
  MotionModel motion_;
  TumFormatWriter writer_;
  DirectOdometry odom_;

  KeyControl ctrl_;
  std::string frame_{"fixed"};
  tf2_ros::TransformBroadcaster tfbr_;
  cv::Mat intrin;

  ros::NodeHandle pnh_;
  ros::Publisher clock_pub_;
  ros::Publisher pose_array_pub_;
  ros::Publisher align_marker_pub_;
  PosePathPublisher kf_pub_;
  PosePathPublisher odom_pub_;

  ros::Publisher points_pub_;

  ros::Subscriber imu_sub_;
  ros::Subscriber enc_sub_;
  ros::Subscriber cinfo_sub_;


  sm::Imu curr_imu_msg_;
  sm::Imu prev_imu_msg_;
  nm::Odometry curr_enc_msg_;
  nm::Odometry prev_enc_msg_;

  
  using SyncStereo = mf::TimeSynchronizer<sm::Image, sm::Image>;
  using SyncStereoDepth = mf::TimeSynchronizer<sm::Image, sm::Image, sm::Image>;

  std::optional<SyncStereo> sync_stereo_;
  std::optional<SyncStereoDepth> sync_stereo_depth_;

  mf::Subscriber<sm::Image> sub_image0_;
  mf::Subscriber<sm::Image> sub_image1_;
  mf::Subscriber<sm::Image> sub_depth0_;

  double prev_time {-1};
  int flag {0};
  int cnt {0};
  int buff_count {1};

  bool use_depth;
  bool use_imu;
  bool use_odom;
  
  bool received_imu {false};
  bool received_odom {false};

  bool init_tf{false};
  // The default SE(3) matrix is Identity for Sophus objects
  SE3d dT_pred;
  SE3d T_c0_c_gt;

  bool pred_init {false};
  PosePathPublisher imu_odom_pub_; //imu predict path publisher
  Eigen::Vector3d tw_vel{Eigen::Vector3d::Zero()}; //encoder linear velocities along axes
  ros::Time prev_msg_time; //timestamp for the last IMU/Image message. We integrate over time differences b/w these images
  SE3d imu_pred_pose; //Default is Identity
  SE3d acc_pose; //accumulated pose
};

NodeData::NodeData(const ros::NodeHandle& pnh)
    : pnh_(pnh),
      sub_image0_(pnh_, "image0", pnh_.param<int>("buff_count", 10)),
      sub_image1_(pnh_, "image1", pnh_.param<int>("buff_count", 10)),
      sub_depth0_(pnh_, "depth0", pnh_.param<int>("buff_count", 10)) {
  frame_ = pnh_.param<std::string>("fix_frame", "fixed");
  ROS_INFO_STREAM("fixed frame: " << frame_);

  InitRosIO();
  InitOdom();

  // Not required - delete later (ctrl_, waiter_, motion_ statements)
  // Wait after key_control
  const int wait_ms = pnh_.param<int>("wait_ms", 0);
  ROS_INFO_STREAM("wait_ms: " << wait_ms);
  ctrl_ = KeyControl(wait_ms);

  const auto save = pnh_.param<std::string>("save", "");
  writer_ = TumFormatWriter(save);
  if (!writer_.IsDummy()) {
    ROS_WARN_STREAM("Writing results to: " << writer_.filename());
  }

  const auto alpha = pnh_.param<double>("motion_alpha", 0.5);
  motion_ = MotionModel(alpha);
  ROS_INFO_STREAM("motion_alpha: " << motion_.alpha());
}

void NodeData::camInfoCallback(const sm::CameraInfo& msgCInfo) {
  odom_.camera = MakeCamera(msgCInfo);
  ROS_INFO_STREAM("Camera Intrinsics are: " << odom_.camera.Repr());
  cinfo_sub_.shutdown();
}


void NodeData::imuCallback(const sm::ImuConstPtr& msgImu) {
  curr_imu_msg_ = *msgImu;

  if (!received_imu) {
    prev_imu_msg_ = *msgImu;
    received_imu = true;
  }

  if(!pred_init){
    prev_msg_time = msgImu->header.stamp;
    pred_init = true;
    return;
  }
  // If the current imu message is late, ignore it
  if(msgImu->header.stamp < prev_msg_time)
    return;

  // Get the delta time to integrate over
  ros::Duration dtime = msgImu->header.stamp - prev_msg_time;
  prev_msg_time = msgImu->header.stamp;

  Eigen::Vector3d imu_ang_vel;
  Ros2Eigen(msgImu->angular_velocity, imu_ang_vel);
  SE3d deltapose = {Sophus::SO3d::exp(imu_ang_vel * dtime.toSec()), tw_vel * dtime.toSec()};
  imu_pred_pose *= deltapose;
  acc_pose *= deltapose;

  const gm::PoseStamped pose_msg = imu_odom_pub_.Publish(msgImu->header.stamp, acc_pose);
}

void NodeData::odomCallback(const nm::OdometryConstPtr& msgEnc) {
  curr_enc_msg_ = *msgEnc;

  if (!received_odom) {
    prev_enc_msg_ = *msgEnc;
    received_odom = true;
  }
  //Set the current linear velocity
  Ros2Eigen(msgEnc->twist.twist.linear, tw_vel);
}

void NodeData::getPrediction(double& pred_x, double& pred_y, double& pred_z, double& pred_a) {
  pred_x = 0.0;
  pred_y = 0.0;
  pred_z = 0.0;
  pred_a = 0.0;

  if (use_odom && received_odom) {
    pred_x = curr_enc_msg_.pose.pose.position.x - prev_enc_msg_.pose.pose.position.x;
    pred_y = curr_enc_msg_.pose.pose.position.y - prev_enc_msg_.pose.pose.position.y;
    pred_z = curr_enc_msg_.pose.pose.position.z - prev_enc_msg_.pose.pose.position.z;
    prev_enc_msg_ = curr_enc_msg_;
  }

  if (use_imu && received_imu) {
    pred_a = tf::getYaw(curr_imu_msg_.orientation) - tf::getYaw(prev_imu_msg_.orientation);

    if      (pred_a >= M_PI) pred_a -= 2.0 * M_PI;
    else if (pred_a < -M_PI) pred_a += 2.0 * M_PI;    
    
    prev_imu_msg_ = curr_imu_msg_;
  }
}

void NodeData::StereoCb(const sensor_msgs::ImageConstPtr& msgLeft, const sensor_msgs::ImageConstPtr& msgRight) {
  StereoDepthCb(msgLeft, msgRight, nullptr);
}

void NodeData::StereoDepthCb(const sensor_msgs::ImageConstPtr& msgLeft, const sensor_msgs::ImageConstPtr& msgRight, const sensor_msgs::ImageConstPtr& msgDepth) {
  cv_bridge::CvImageConstPtr cv_ptrLeft;
  try {
      cv_ptrLeft = cv_bridge::toCvShare(msgLeft);
  } catch (cv_bridge::Exception& e) {
      ROS_ERROR("cv_bridge exception: %s", e.what());
      return;
  }

  cv_bridge::CvImageConstPtr cv_ptrRight;
  try {
      cv_ptrRight = cv_bridge::toCvShare(msgRight);
  } catch (cv_bridge::Exception& e) {
      ROS_ERROR("cv_bridge exception: %s", e.what());
      return;
  }

  if (msgDepth) {
    cv_bridge::CvImageConstPtr cv_ptrDepth;
    try {
        cv_ptrDepth = cv_bridge::toCvShare(msgDepth);
    } catch (cv_bridge::Exception& e) {
        ROS_ERROR("cv_bridge exception: %s", e.what());
        return;
    }  
    Run(cv_ptrLeft, cv_ptrRight, cv_ptrLeft->header.stamp, cv_ptrDepth);
  }
  else
    Run(cv_ptrLeft, cv_ptrRight, cv_ptrLeft->header.stamp, nullptr);
}

void NodeData::InitRosIO() {

  pnh_.getParam("use_imu", use_imu);
  pnh_.getParam("use_odom", use_odom);
  pnh_.getParam("use_depth", use_depth);

  cinfo_sub_ = pnh_.subscribe("cinfo1", 1, &NodeData::camInfoCallback, this);
  //cinfo_sub_ = pnh_.subscribe("/camera/infra2/camera_info", pnh_.param<int>("buff_count", 1), &NodeData::camInfoCallback, this);
  if (use_imu)
    imu_sub_ = pnh_.subscribe("imu", pnh_.param<int>("buff_count", 10), &NodeData::imuCallback, this);
  if (use_odom)
    enc_sub_ = pnh_.subscribe("enc", pnh_.param<int>("buff_count", 10), &NodeData::odomCallback, this);  

  clock_pub_ = pnh_.advertise<rosgraph_msgs::Clock>("/clock", 1);
  kf_pub_ = PosePathPublisher(pnh_, "kf", frame_);
  odom_pub_ = PosePathPublisher(pnh_, "odom", frame_);
  points_pub_ = pnh_.advertise<sm::PointCloud2>("points", 1);
  pose_array_pub_ = pnh_.advertise<gm::PoseArray>("poses", 1);
  align_marker_pub_ = pnh_.advertise<vm::Marker>("align_graph", 1);

  if (use_depth) {
    ROS_INFO_STREAM("Depth is being used from the camera");
    sync_stereo_depth_.emplace(sub_image0_, sub_image1_, sub_depth0_, 5);
    sync_stereo_depth_->registerCallback(
        boost::bind(&NodeData::StereoDepthCb, this, _1, _2, _3));
  } else {
    ROS_INFO_STREAM("Depth is not used from the camera");
    sync_stereo_.emplace(sub_image0_, sub_image1_, 5);
    sync_stereo_->registerCallback(
        boost::bind(&NodeData::StereoCb, this, _1, _2));
  }

  pnh_.getParam("data_max_depth", data_max_depth_);
  pnh_.getParam("cloud_max_depth", cloud_max_depth_);

  imu_odom_pub_ = PosePathPublisher(pnh_, "imu_odom", "imudom");

  //intrin = (cv::Mat_<double>(1,5) << pnh_.param<double>("fx",0), pnh_.param<double>("fy",
        //0),pnh_.param<double>("cx",0),pnh_.param<double>("cy",0),pnh_.param<double>("bs",0));
  //ROS_INFO_STREAM("intrinsics obtained from the launch file: "<<intrin);
}

void NodeData::InitOdom() {
  {
    auto cfg = ReadOdomCfg({pnh_, "odom"});
    pnh_.getParam("tbb", cfg.tbb);
    pnh_.getParam("log", cfg.log);
    pnh_.getParam("vis", cfg.vis);
    odom_.Init(cfg);
  }
  odom_.selector = PixelSelector(ReadSelectCfg({pnh_, "select"}));
  odom_.matcher = StereoMatcher(ReadStereoCfg({pnh_, "stereo"}));
  odom_.aligner = FrameAligner(ReadDirectCfg({pnh_, "align"}));
  odom_.adjuster = BundleAdjuster(ReadDirectCfg({pnh_, "adjust"}));
  odom_.cmap = GetColorMap(pnh_.param<std::string>("cm", "jet"));

  ROS_INFO_STREAM(odom_.Repr());
}

void NodeData::PublishCloud(const std_msgs::Header& header) const {
  if (points_pub_.getNumSubscribers() == 0) return;
  static sensor_msgs::PointCloud2 cloud;
  cloud.header = header;
  cloud.point_step = 16;
  cloud.fields = MakePointFieldsXYZI();

  ROS_DEBUG_STREAM(odom_.window.MargKf().status().Repr());

  //Publish in camera frame for straightforward mapping later
  cloud.header.frame_id = "camera";
  Keyframe2CloudCameraFrame(odom_.window.MargKf(), cloud, cloud_max_depth_);
  //Keyframe2Cloud(odom_.window.MargKf(), cloud, cloud_max_depth_);

  points_pub_.publish(cloud);
}

void NodeData::SendTransform(const geometry_msgs::PoseStamped& pose_msg,
                             const std::string& child_frame) {
  gm::TransformStamped tf_msg;
  tf_msg.header = pose_msg.header;
  tf_msg.child_frame_id = child_frame;
  Ros2Ros(pose_msg.pose, tf_msg.transform);
  tfbr_.sendTransform(tf_msg);
}

void NodeData::Run(cv_bridge::CvImageConstPtr cv_ptrLeft, cv_bridge::CvImageConstPtr cv_ptrRight, const ros::Time timestamp, cv_bridge::CvImageConstPtr cv_ptrDepth) {
  double dt;
  double timestamp_sec = timestamp.toSec();

  if(!pred_init){
    prev_msg_time = timestamp;
    pred_init = true;
  }
  if(timestamp > prev_msg_time){
    //integrate further to close the time gap
    ros::Duration dtime = timestamp - prev_msg_time;
    prev_msg_time = timestamp;
    SE3d deltapose = {Sophus::SO3d(), tw_vel * dtime.toSec()};
    imu_pred_pose *= deltapose;
    acc_pose *= deltapose;
  }
  //ELSE use the pose diff as it is, don't update the last timestamp

  double pred_x, pred_y, pred_z, pred_a;
  getPrediction(pred_x, pred_y, pred_z, pred_a);

  if(prev_time < 0){
    motion_.Init(T_c0_c_gt);
    prev_time = timestamp_sec;
    dt = 0;
    flag = 1; //flag is set to 1 only for the first iteration
  }
  else{
    dt = timestamp_sec - prev_time;
    prev_time = timestamp_sec;
    dT_pred = motion_.PredictDelta(dt);
    flag = 0;
  }

  // Reset the pose to Identity
  //dT_pred = imu_pred_pose;
  imu_pred_pose = SE3d();

  auto image_l = cv_ptrLeft->image;
  auto image_r = cv_ptrRight->image;
  cv::Mat image_depth;

  // Intrinsic
  // if (!odom_.camera.Ok()){
  // These are for the ERL Realsense camera. On second thought, the order is wrong I believe
  //const cv::Mat intrin({1, 5}, {380.4, 312.9, 379.9, 247.2, 0.095});

  // These are for the Realsense camera from the repo
  //const cv::Mat intrin({1, 5}, {393.4910888671875, 393.4910888671875, 318.6263122558594, 240.12942504882812, 0.095150406});

  // After on-board calibration on ERL realsense
  //K: [430.1014404296875, 0.0, 420.8174133300781, 0.0, 430.1014404296875, 241.85072326660156, 0.0, 0.0, 1.0]
  //const cv::Mat intrin({1, 5}, {430.1014404296875, 430.1014404296875, 420.8174133300781, 241.85072326660156, 0.09493});

  //For the gazebo realsense plugin D435 480x640
  //[347.99755859375, 0.0, 320.0, 0.0, 347.99755859375, 240.0, 0.0, 0.0, 1.0]
  //const cv::Mat intrin({1, 5}, {347.99755859375, 347.99755859375, 320.0, 240.0, 0.05});

  // ROS_INFO_STREAM("intrinsics: "<<intrin);
  // const auto camera = Camera::FromMat({image_l.cols, image_l.rows}, intrin);
  // odom_.SetCamera(camera);
  // ROS_INFO_STREAM(camera);
  // }

  // Odom
  OdomStatus status;

  if (use_depth) {
    image_depth = cv_ptrDepth->image;
    image_depth.convertTo(image_depth, CV_32FC1, 0.001);
    if (data_max_depth_ > 0)
      cv::threshold(image_depth, image_depth, data_max_depth_, 0, cv::THRESH_TOZERO_INV);
  }
    
  status = odom_.Estimate(image_l, image_r, dT_pred, image_depth);

  ROS_INFO_STREAM(status.Repr());

  // Motion model correct if tracking is ok and not first frame
  if (status.track.ok && flag == 0) {
    motion_.Correct(status.Twc(), dt);
  } else {
    ROS_WARN_STREAM("Tracking failed (or 1st frame), slow motion model");
    motion_.Scale(0.5);
  }

  // Write to output
  writer_.Write(cnt++, status.Twc());

  // ROS_DEBUG_STREAM("trans gt:   " << T_c0_c_gt.translation().transpose());
  ROS_DEBUG_STREAM("trans odom: " << status.Twc().translation().transpose());
  ROS_DEBUG_STREAM("trans ba:   " << odom_.window.CurrKf().Twc().translation().transpose());
  ROS_DEBUG_STREAM("aff_l: " << odom_.frame.state().affine_l.ab.transpose());
  ROS_DEBUG_STREAM("aff_r: " << odom_.frame.state().affine_r.ab.transpose());

  // publish stuff
  std_msgs::Header header;
  header.frame_id = frame_;
  header.stamp = timestamp;

  PublishOdom(header, status.Twc());

  if (status.map.remove_kf) {
    PublishCloud(header);
  }
  return;
}

void NodeData::PublishOdom(const std_msgs::Header& header,
                           const Sophus::SE3d& Twc) {
  const auto odom_pose_msg = odom_pub_.Publish(header.stamp, Twc);
  SendTransform(odom_pose_msg, "camera");

  const auto poses = odom_.window.GetAllPoses();
  gm::PoseArray pose_array_msg;
  pose_array_msg.header = header;
  pose_array_msg.poses.resize(poses.size());
  for (size_t i = 0; i < poses.size(); ++i) {
    Sophus2Ros(poses.at(i), pose_array_msg.poses.at(i));
  }
  pose_array_pub_.publish(pose_array_msg);
}

} //namespace closure

int main(int argc, char** argv) {
  ros::init(argc, argv, "dsol_data");
  cv::setNumThreads(4);
  sv::dsol::NodeData node{ros::NodeHandle{"~"}};
  ros::spin();
  return 0;
}
