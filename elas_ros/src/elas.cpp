/*
 Copywrite 2012. All rights reserved.
 Cyphy Lab, https://wiki.qut.edu.au/display/cyphy/Robotics,+Vision+and+Sensor+Networking+at+QUT
 Queensland University of Technology
 Brisbane, Australia

 Author: Patrick Ross
 Contact: patrick.ross@connect.qut.edu.au

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <ros/ros.h>

#include <std_msgs/MultiArrayLayout.h>
#include <std_msgs/MultiArrayDimension.h>
#include <std_msgs/Int32MultiArray.h>

#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/image_encodings.h>
#include <stereo_msgs/DisparityImage.h>
#include <nav_msgs/Odometry.h>
#include <tf/transform_broadcaster.h>

#include <visualization_msgs/Marker.h>

#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/exact_time.h>
#include <message_filters/sync_policies/approximate_time.h>

#include <image_transport/subscriber_filter.h>
#include <image_geometry/stereo_camera_model.h>

#include <dynamic_reconfigure/server.h>


#include <cv_bridge/cv_bridge.h>

#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>

#include <elas_ros/ElasFrameData.h>
#include <elas_ros/ElasDynConfig.h>
#include <elas_ros/SparseDepth.h>

#include <elas.h>
#include <math.h>
#include <string>
#include <fstream>
#include <sstream>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

//#define DOWN_SAMPLE

using nav_msgs::OdometryConstPtr;

static tf::Transform odom_to_tf(const OdometryConstPtr &odom) {
  const geometry_msgs::Quaternion &q = odom->pose.pose.orientation;
  const geometry_msgs::Point &T = odom->pose.pose.position;
  tf::Quaternion qt(q.x, q.y, q.z, q.w);
  tf::Vector3    Tt(T.x, T.y, T.z);
  return (tf::Transform(qt, Tt));
}

#ifdef DEBUG_IMAGE
static int image_counter(0);
std::ofstream transform_file("transforms.txt");
#endif

class Elas_Proc
{
public:
  typedef elas_ros::ElasDynConfig Config;
  typedef std::vector<Elas::sparse_triangle> TriangleVec;
  Elas_Proc(const std::string& transport)
  {
    ros::NodeHandle local_nh("~");
    local_nh.param("queue_size", queue_size_, 5);

    // Topics
    std::string stereo_ns = nh.resolveName("stereo");
    std::string left_topic = ros::names::clean(stereo_ns + "/left/" + nh.resolveName("image"));
    std::string right_topic = ros::names::clean(stereo_ns + "/right/" + nh.resolveName("image"));
    std::string left_info_topic = stereo_ns + "/left/camera_info";
    std::string right_info_topic = stereo_ns + "/right/camera_info";
    std::string odom_topic = "odom";
    // setCallback() is supposedly guaranteed to invoke the callback
    // immediately, with the config default values.
    configServer_.setCallback(boost::bind(&Elas_Proc::configure, this, _1, _2));

    image_transport::ImageTransport it(nh);
    left_sub_.subscribe(it, left_topic, 1, transport);
    right_sub_.subscribe(it, right_topic, 1, transport);
    left_info_sub_.subscribe(nh, left_info_topic, 1);
    right_info_sub_.subscribe(nh, right_info_topic, 1);
    odom_sub_.subscribe(nh, odom_topic, 1);

    ROS_INFO("Subscribing to:\n%s\n%s\n%s\n%s\n%s",left_topic.c_str(),right_topic.c_str(),left_info_topic.c_str(),right_info_topic.c_str(),
             odom_topic.c_str());

    image_transport::ImageTransport local_it(local_nh);
    disp_pub_.reset(new Publisher(local_it.advertise("image_disparity", 1)));
    depth_pub_.reset(new Publisher(local_it.advertise("depth", 1)));
    pc_pub_.reset(new ros::Publisher(local_nh.advertise<PointCloud>("point_cloud", 1)));
    elas_fd_pub_.reset(new ros::Publisher(local_nh.advertise<elas_ros::ElasFrameData>("frame_data", 1)));
    support_pt_pub_.reset(new ros::Publisher(local_nh.advertise<sensor_msgs::PointCloud>("support_points", 1)));
    triangle_pub_.reset(new ros::Publisher(local_nh.advertise<sensor_msgs::PointCloud>("triangles", 1)));
    triangle_list_pub_.reset(new ros::Publisher(local_nh.advertise<visualization_msgs::Marker>("triangle_list", 1)));
    sparse_depth_pub_.reset(new ros::Publisher(local_nh.advertise<elas_ros::SparseDepth>("sparse_depth", 1)));

    pub_disparity_ = local_nh.advertise<stereo_msgs::DisparityImage>("disparity", 1);
    elas_.reset(new Elas(param_));
    startSync();
  }

  typedef image_transport::SubscriberFilter Subscriber;
  typedef message_filters::Subscriber<sensor_msgs::CameraInfo> InfoSubscriber;
  typedef message_filters::Subscriber<nav_msgs::Odometry> OdomSubscriber;  
  typedef image_transport::Publisher Publisher;
  typedef message_filters::sync_policies::ExactTime<sensor_msgs::Image, sensor_msgs::Image,
                                                    sensor_msgs::CameraInfo, sensor_msgs::CameraInfo,
                                                    nav_msgs::Odometry> ExactPolicy;
  typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::Image,
                                                          sensor_msgs::CameraInfo, sensor_msgs::CameraInfo,
                                                          nav_msgs::Odometry> ApproximatePolicy;
  typedef message_filters::Synchronizer<ExactPolicy> ExactSync;
  typedef message_filters::Synchronizer<ApproximatePolicy> ApproximateSync;
  typedef pcl::PointCloud<pcl::PointXYZRGB> PointCloud;

  void configToParam(const Config &config) {
#define UPDATE_PARAM(X) if (param_.X != config.X) { param_.X = config.X;  ROS_INFO_STREAM("updated " #X " from " << config.X << " to "  << param_.X); }
#ifdef DOWN_SAMPLE
    config.subsampling = true;
    ROS_WARN("forcing subsampling to be true!");
#endif
    UPDATE_PARAM(disp_min);
    UPDATE_PARAM(disp_max);
    UPDATE_PARAM(support_threshold);
    UPDATE_PARAM(support_texture);
    UPDATE_PARAM(candidate_stepsize);
    UPDATE_PARAM(incon_window_size);
    UPDATE_PARAM(incon_threshold);
    UPDATE_PARAM(incon_min_support);
    UPDATE_PARAM(add_corners);
    UPDATE_PARAM(grid_size);
    UPDATE_PARAM(beta);
    UPDATE_PARAM(gamma);
    UPDATE_PARAM(sigma);
    UPDATE_PARAM(sradius);
    UPDATE_PARAM(match_texture);
    UPDATE_PARAM(lr_threshold);
    UPDATE_PARAM(speckle_sim_threshold);
    UPDATE_PARAM(speckle_size);
    UPDATE_PARAM(ipol_gap_width);
    UPDATE_PARAM(filter_median);
    UPDATE_PARAM(filter_adaptive_mean);
    UPDATE_PARAM(postprocess_only_left);
    UPDATE_PARAM(subsampling);
  }

  bool doApproxSync() const {
    ros::NodeHandle local_nh("~");
    bool approx;
    local_nh.param("approximate_sync", approx, false);
    return (approx);
  }
  
  void stopSync() {
    if (doApproxSync()) {
      approximate_sync_.reset();
    }  else {
      exact_sync_.reset();
    }
  }

  void startSync() {
    // Synchronize input topics. Optionally do approximate synchronization.
    if (doApproxSync()) {
      approximate_sync_.reset(new ApproximateSync(ApproximatePolicy(queue_size_),
                                                  left_sub_, right_sub_,
                                                  left_info_sub_, right_info_sub_, odom_sub_));
      approximate_sync_->registerCallback(boost::bind(&Elas_Proc::process, this, _1, _2, _3, _4, _5));
    } else {
      exact_sync_.reset(new ExactSync(ExactPolicy(queue_size_),
                                      left_sub_, right_sub_, left_info_sub_, right_info_sub_, odom_sub_));
      exact_sync_->registerCallback(boost::bind(&Elas_Proc::process, this, _1, _2, _3, _4, _5));
    }
  }

  void configure(Config& config, int level) {
    stopSync();
    configToParam(config);
    elas_.reset(new Elas(param_));
    startSync();
  }

  void publish_triangles(const sensor_msgs::ImageConstPtr& l_image_msg,
                         const std::vector<Elas::triangle> &triangles) {
    if (triangle_pub_->getNumSubscribers() > 0) {
      sensor_msgs::PointCloud::Ptr msg(new sensor_msgs::PointCloud());
      msg->header = l_image_msg->header;
      msg->points.resize(triangles.size());
      for (int i = 0; i < triangles.size(); i++) {
        // a bit ugly to store ints as floats...
        msg->points[i].x = triangles[i].c1;
        msg->points[i].y = triangles[i].c2;
        msg->points[i].z = triangles[i].c3;
      }
      triangle_pub_->publish(msg);
    }
  }

  static bool normal_points_to_camera(const cv::Point3d *vert, double anglim) {
    cv::Point3d a = vert[1]-vert[0];
    cv::Point3d b = vert[2]-vert[0];
    // compute normal vector
    double axb[3] = {a.y*b.z - a.z*b.y,
                     a.z*b.x - a.x*b.z,
                     a.x*b.y - a.y*b.x};
    double axb_sq = axb[0]*axb[0] + axb[1]*axb[1] + axb[2]*axb[2];
    // compute vector to center
    const cv::Point3d c = vert[0] + vert[1] + vert[2];
    double c_sq = c.x*c.x + c.y*c.y + c.z*c.z;
    double cdotaxb = c.x*axb[0] + c.y*axb[1] +c.z*axb[2];
    return (cdotaxb * cdotaxb > axb_sq * c_sq * anglim);
  }
  
  void publish_sparse_depth(const tf::Transform &T_cam_world,
                            const std::vector<elas_ros::SupportPoint3d> &pt,
                            const std::vector<Elas::sparse_triangle> &triangles,
                            const sensor_msgs::CameraInfoConstPtr& l_info_msg) {
    if (sparse_depth_pub_->getNumSubscribers() > 0) {
      elas_ros::SparseDepth::Ptr msg(new elas_ros::SparseDepth());
      msg->header = l_info_msg->header;
      msg->point.resize(pt.size());
      msg->triangle.resize(triangles.size());
      for (int i = 0; i < pt.size(); i++) {
        msg->point[i].x  = pt[i].x;
        msg->point[i].y  = pt[i].y;
        msg->point[i].z  = pt[i].z;
        msg->point[i].id = pt[i].id;
      }
      
      for (int i = 0; i < triangles.size(); i++) {
        msg->triangle[i].c[0] = triangles[i].c[0];
        msg->triangle[i].c[1] = triangles[i].c[1];
        msg->triangle[i].c[2] = triangles[i].c[2];
      }
      sparse_depth_pub_->publish(msg);
    }
  }

  void publish_triangle_list(const sensor_msgs::ImageConstPtr& l_image_msg,
                             const std::vector<Elas::support_pt> &pt,
                             const std::vector<Elas::triangle> &triangles,
                             const sensor_msgs::CameraInfoConstPtr& l_info_msg, 
                             const sensor_msgs::CameraInfoConstPtr& r_info_msg) {
    if (triangle_list_pub_->getNumSubscribers() > 0) {
      image_geometry::StereoCameraModel model;
      model.fromCameraInfo(*l_info_msg, *r_info_msg);

      visualization_msgs::Marker::Ptr msg(new visualization_msgs::Marker());
      msg->header = l_image_msg->header;
      msg->header.frame_id = l_info_msg->header.frame_id;
      msg->points.clear();
      msg->ns = "elas_triangle_list";
      msg->id = 0;
      msg->type = visualization_msgs::Marker::TRIANGLE_LIST;
      msg->action = visualization_msgs::Marker::ADD;
      msg->pose.position.x = 0.0;
      msg->pose.position.y = 0.0;
      msg->pose.position.z = 0.0;
      msg->pose.orientation.x = 0.0;
      msg->pose.orientation.y = 0.0;
      msg->pose.orientation.z = 0.0;
      msg->pose.orientation.w = 1.0;
      msg->scale.x = 1.0;
      msg->scale.y = 1.0;
      msg->scale.z = 1.0;
      msg->color.g = 1.0;
      msg->color.a = 1.0;
      std_msgs::ColorRGBA c;
      c.r = 1.0;
      c.g = 0.0;
      c.b = 1.0;
      c.a = 1.0;
      for (int i = 0; i < triangles.size(); i++) {
        std::vector<Elas::support_pt> vert;
        vert.push_back(pt[triangles[i].c1]);
        vert.push_back(pt[triangles[i].c2]);
        vert.push_back(pt[triangles[i].c3]);
        const int MIN_DISP(2);  // anything below is questionable!
        if (vert[0].d > MIN_DISP &&
            vert[1].d > MIN_DISP &&
            vert[2].d > MIN_DISP) {
          for (int j = 0; j < 3; j++) {
            cv::Point2d left_uv(vert[j].u, vert[j].v);
            cv::Point3d pcv;
            model.projectDisparityTo3d(left_uv, vert[j].d, pcv);
            geometry_msgs::Point p;
            p.x = pcv.x;
            p.y = pcv.y;
            p.z = pcv.z;
            msg->points.push_back(p);
            msg->colors.push_back(c);
          }
        }
      }
      if ((msg->points.size() % 3) != 0) {
        ROS_ERROR_STREAM("ERROR: number of points must be mult of 3!");
      }
      triangle_list_pub_->publish(msg);
    }
  }
 
  void publish_support_points(const sensor_msgs::ImageConstPtr& l_image_msg,
                              const std::vector<Elas::support_pt> &points) {
    if (support_pt_pub_->getNumSubscribers() > 0) {
      sensor_msgs::PointCloud:: Ptr msg(new sensor_msgs::PointCloud());
      msg->header = l_image_msg->header;
      msg->points.resize(points.size());
      for (int i = 0; i < points.size(); i++) {
        msg->points[i].x = points[i].u;
        msg->points[i].y = points[i].v;
        msg->points[i].z = points[i].d;
      }
      support_pt_pub_->publish(msg);
    }
  }

  void publish_point_cloud(const sensor_msgs::ImageConstPtr& l_image_msg, 
                           float* l_disp_data, const std::vector<int32_t>& inliers,
                           int32_t l_width, int32_t l_height,
                           const sensor_msgs::CameraInfoConstPtr& l_info_msg, 
                           const sensor_msgs::CameraInfoConstPtr& r_info_msg)
  {
    try
    {
      cv_bridge::CvImageConstPtr cv_ptr;
      cv_ptr = cv_bridge::toCvShare(l_image_msg, sensor_msgs::image_encodings::RGB8);
      image_geometry::StereoCameraModel model;
      model.fromCameraInfo(*l_info_msg, *r_info_msg);
      pcl::PCLHeader l_info_header = pcl_conversions::toPCL(l_info_msg->header);

      PointCloud::Ptr point_cloud(new PointCloud());
      point_cloud->header.frame_id = l_info_header.frame_id;

      point_cloud->header.stamp = l_info_header.stamp;
      point_cloud->width = 1;
      point_cloud->height = inliers.size();
      point_cloud->points.resize(inliers.size());

      elas_ros::ElasFrameData data;
      data.header.frame_id = l_info_msg->header.frame_id;
      data.header.stamp = l_info_msg->header.stamp;
      data.width = l_width;
      data.height = l_height;
      data.disparity.resize(l_width * l_height);
      data.r.resize(l_width * l_height);
      data.g.resize(l_width * l_height);
      data.b.resize(l_width * l_height);
      data.x.resize(l_width * l_height);
      data.y.resize(l_width * l_height);
      data.z.resize(l_width * l_height);
      data.left = *l_info_msg;
      data.right = *r_info_msg;

      // Copy into the data
      for (int32_t u=0; u<l_width; u++)
      {
        for (int32_t v=0; v<l_height; v++)
        {
          int index = v*l_width + u;
          data.disparity[index] = l_disp_data[index];
#ifdef DOWN_SAMPLE
          cv::Vec3b col = cv_ptr->image.at<cv::Vec3b>(v*2,u*2);
#else
          cv::Vec3b col = cv_ptr->image.at<cv::Vec3b>(v,u);
#endif
          data.r[index] = col[0];
          data.g[index] = col[1];
          data.b[index] = col[2];
        }
      }

      for (size_t i=0; i<inliers.size(); i++)
      {
        cv::Point2d left_uv;
        int32_t index = inliers[i];
#ifdef DOWN_SAMPLE
        left_uv.x = (index % l_width) * 2;
        left_uv.y = (index / l_width) * 2;
#else
        left_uv.x = index % l_width;
        left_uv.y = index / l_width;
#endif
        cv::Point3d point;
        model.projectDisparityTo3d(left_uv, l_disp_data[index], point);
        point_cloud->points[i].x = point.x;
        point_cloud->points[i].y = point.y;
        point_cloud->points[i].z = point.z;
        point_cloud->points[i].r = data.r[index];
        point_cloud->points[i].g = data.g[index];
        point_cloud->points[i].b = data.b[index];

        data.x[index] = point.x;
        data.y[index] = point.y;
        data.z[index] = point.z;
      }

      pc_pub_->publish(point_cloud);
      elas_fd_pub_->publish(data);
    }
    catch (cv_bridge::Exception& e)
    {
      ROS_ERROR("cv_bridge exception: %s", e.what());
    }
  }

  
  void add_to_support_point_cloud(const std::vector<elas_ros::SupportPoint3d> &pts) {
    int numPointsAdded(0);
    for (int i = 0; i < pts.size(); i++) {
      const elas_ros::SupportPoint3d &sp = pts[i];
      if (support_pt_cloud_.count(sp.id) == 0) {
        support_pt_cloud_[sp.id] = cv::Point3d(sp.x, sp.y, sp.z);
        numPointsAdded++;
      }
    }
  }
  
  void support_points_to_3d(std::vector<elas_ros::SupportPoint3d> *pt3d,
                            const std::vector<Elas::support_pt> &pts,
                            const tf::Transform &T_world_cam) {
    const image_geometry::StereoCameraModel &cam = model_;
    for (int i = 0; i < pts.size(); i++) {
      const Elas::support_pt &sp = pts[i];
      cv::Point3d p3d;
      cv::Point2d left_uv(sp.u, sp.v);
      // project to 3d in camera frame
      cam.projectDisparityTo3d(left_uv, sp.d > 0 ? sp.d : 1e-3, p3d);
      // transform to world frame
      const tf::Vector3 v(p3d.x, p3d.y, p3d.z);
      const tf::Vector3 vt(T_world_cam(v));
      elas_ros::SupportPoint3d sp3d;
      sp3d.x = vt.x();
      sp3d.y = vt.y();
      sp3d.z = vt.z();
      sp3d.id = sp.id;
      pt3d->push_back(sp3d);
    }
  }

  static void disparity_to_3d(const image_geometry::StereoCameraModel &model,
                              const Elas::support_pt *sp, cv::Point3d *vert) {
    cv::Point3d c;
    for (int j = 0; j < 3; j++) {
      cv::Point2d left_uv(sp[j].u, sp[j].v);
      cv::Point3d pcv;
      model.projectDisparityTo3d(left_uv, sp[j].d, vert[j]);
    }
  }

  void filter_triangles(std::vector<Elas::sparse_triangle> *filtered,
                        const std::vector<Elas::support_pt> &pt,
                        const std::vector<Elas::sparse_triangle> raw,
                        const image_geometry::StereoCameraModel &model) {
    double anglim = cos(88.0 * M_PI / 180.0);

    for (int i = 0; i < raw.size(); i++) {
      const Elas::sparse_triangle &traw = raw[i];
      Elas::support_pt sp[3];
      for (int j = 0; j< 3; j++) {
        int idx = traw.cidx[j];
        if (idx >= 0 && idx < pt.size()) {
          sp[j] = pt[idx];
        } else {
          std::cout << "out of range!!" << idx << " vs " << pt.size() << std::endl;
        }
      }

      const int MIN_DISP(2);  // anything below is questionable!
      if (sp[0].d > MIN_DISP && sp[1].d > MIN_DISP && sp[2].d > MIN_DISP) {
        cv::Point3d vert[3];
        disparity_to_3d(model, sp, vert);
        if (normal_points_to_camera(vert, anglim)) {
          filtered->push_back(traw);
        }
      }
    }
    std::cout << "filtered triangles: " << raw.size() << " -> " << filtered->size() << std::endl;
  }
  void project_points(std::vector<Elas::support_pt> *ptp,
                      Elas_Proc::TriangleVec *triUsed,
                      const tf::Transform &T_cam_world) const {
#ifdef DEBUG_IMAGE    
    std::ostringstream convert;
    convert << image_counter;
    std::ofstream point_file(("points_" + convert.str() + ".txt").c_str());
    tf::Vector3          T = T_cam_world.inverse().getOrigin();
    const tf::Matrix3x3 &R = T_cam_world.inverse().getBasis();
    transform_file << image_counter << " " <<
      R[0][0] << " " << R[0][1] << " " << R[0][2] << " " << T[0] << " " <<
      R[1][0] << " " << R[1][1] << " " << R[1][2] << " " << T[1] << " " <<
      R[2][0] << " " << R[2][1] << " " << R[2][2] << " " << T[2] << std::endl;
    transform_file.flush();
    image_counter++;
#endif    
    image_geometry::StereoCameraModel cam = model_;
    const image_geometry::PinholeCameraModel &lcam = cam.left();
    cv::Size res = lcam.fullResolution();

    std::map<int64_t, int> points_used;
    for (SupportPointCloud::const_iterator pci = support_pt_cloud_.begin();
         pci != support_pt_cloud_.end(); ++pci) {
      // transform 3d point to current frame
      const cv::Point3d &pw = pci->second;   // 3d in world frame
      const tf::Vector3 v(pw.x, pw.y, pw.z);
      const tf::Vector3 vt(T_cam_world(v));
      cv::Point3d pc(vt.x(), vt.y(), vt.z()); // 3d in camera frame
      if (vt.z() > 0) {
        double d = cam.getDisparity(vt.z());
        cv::Point2d p2dl = lcam.project3dToPixel(pc);
        if ((p2dl.x > 0) && (p2dl.x < res.width) &&
            (p2dl.y > 0) && (p2dl.y < res.height)) {
          points_used[pci->first] = ptp->size();
          ptp->push_back(Elas::support_pt(p2dl.x, p2dl.y, (int)round(d), pci->first));
        }
#ifdef DEBUG_IMAGE
        point_file << pw.x  << " " << pw.y  << " " << pw.z << " "
                   << pc.x  << " " << pc.y  << " " << pc.z << " "
                   << p2dl.x << " " << p2dl.y << " " << d << std::endl;
#endif        
      }
    }
    for (TriangleVec::const_iterator it = triangle_cloud_.begin();
         it != triangle_cloud_.end(); ++it) {
      const Elas::sparse_triangle &tri = *it;
      if (points_used.count(tri.c[0]) != 0 &&
          points_used.count(tri.c[1]) != 0 &&
          points_used.count(tri.c[2]) != 0) {
        Elas::sparse_triangle t(tri);
        t.cidx[0] = points_used[tri.c[0]];
        t.cidx[1] = points_used[tri.c[1]];
        t.cidx[2] = points_used[tri.c[2]];
        triUsed->push_back(t);
      }
    }
  }

  void process(const sensor_msgs::ImageConstPtr& l_image_msg, 
               const sensor_msgs::ImageConstPtr& r_image_msg,
               const sensor_msgs::CameraInfoConstPtr& l_info_msg, 
               const sensor_msgs::CameraInfoConstPtr& r_info_msg,
               const nav_msgs::OdometryConstPtr &odom_msg
    )
  {
    ROS_DEBUG("Received images and camera info.");
    // Update the camera model
    model_.fromCameraInfo(l_info_msg, r_info_msg);

    // Allocate new disparity image message
    stereo_msgs::DisparityImagePtr disp_msg =
      boost::make_shared<stereo_msgs::DisparityImage>();
    disp_msg->header         = l_info_msg->header;
    disp_msg->image.header   = l_info_msg->header;
    disp_msg->image.height   = l_image_msg->height;
    disp_msg->image.width    = l_image_msg->width;
    disp_msg->image.encoding = sensor_msgs::image_encodings::TYPE_32FC1;
    disp_msg->image.step     = disp_msg->image.width * sizeof(float);
    disp_msg->image.data.resize(disp_msg->image.height * disp_msg->image.step);
    disp_msg->min_disparity = param_.disp_min;
    disp_msg->max_disparity = param_.disp_max;

    // Stereo parameters
    float f = model_.right().fx();
    float T = model_.baseline();
    float depth_fact = T*f*1000.0f;
    uint16_t bad_point = std::numeric_limits<uint16_t>::max();

    // Have a synchronised pair of images, now to process using elas
    // convert images if necessary
    uint8_t *l_image_data, *r_image_data;
    int32_t l_step, r_step;
    cv_bridge::CvImageConstPtr l_cv_ptr, r_cv_ptr;
    if (l_image_msg->encoding == sensor_msgs::image_encodings::MONO8)
    {
#ifdef DEBUG_IMAGE      
      l_cv_ptr = cv_bridge::toCvShare(l_image_msg, sensor_msgs::image_encodings::MONO8);
      std::ostringstream convert;
      convert << image_counter;
      cv::imwrite("image_l_" + convert.str() + ".jpg", l_cv_ptr->image);
#endif      
      l_image_data = const_cast<uint8_t*>(&(l_image_msg->data[0]));
      l_step = l_image_msg->step;
    }
    else
    {
      l_cv_ptr = cv_bridge::toCvShare(l_image_msg, sensor_msgs::image_encodings::MONO8);
      l_image_data = l_cv_ptr->image.data;
      l_step = l_cv_ptr->image.step[0];
#ifdef DEBUG_IMAGE      
      std::ostringstream convert;
      convert << image_counter;
      cv::imwrite("image_l_" + convert.str() + ".jpg", l_cv_ptr->image);
#endif      
    }
    if (r_image_msg->encoding == sensor_msgs::image_encodings::MONO8)
    {
#ifdef DEBUG_IMAGE
      r_cv_ptr = cv_bridge::toCvShare(r_image_msg, sensor_msgs::image_encodings::MONO8);
      std::ostringstream convert;
      convert << image_counter;
      cv::imwrite("image_r_" + convert.str() + ".jpg", r_cv_ptr->image);
#endif
      r_image_data = const_cast<uint8_t*>(&(r_image_msg->data[0]));
      r_step = r_image_msg->step;
    }
    else
    {
      r_cv_ptr = cv_bridge::toCvShare(r_image_msg, sensor_msgs::image_encodings::MONO8);
      r_image_data = r_cv_ptr->image.data;
      r_step = r_cv_ptr->image.step[0];
#ifdef DEBUG_IMAGE
      std::ostringstream convert;
      convert << image_counter;
      cv::imwrite("image_r_" + convert.str() + ".jpg", r_cv_ptr->image);
#endif      
    }

    ROS_ASSERT(l_step == r_step);
    ROS_ASSERT(l_image_msg->width == r_image_msg->width);
    ROS_ASSERT(l_image_msg->height == r_image_msg->height);

#ifdef DOWN_SAMPLE
    int32_t width = l_image_msg->width/2;
    int32_t height = l_image_msg->height/2;
#else
    int32_t width = l_image_msg->width;
    int32_t height = l_image_msg->height;
#endif

    // Allocate
    const int32_t dims[3] = {l_image_msg->width,l_image_msg->height,l_step};
    //float* l_disp_data = new float[width*height*sizeof(float)];
    float* l_disp_data = reinterpret_cast<float*>(&disp_msg->image.data[0]);
    float* r_disp_data = new float[width*height*sizeof(float)];

    const std::vector<Elas::support_pt> points = elas_->getSupportPoints();
    bool firstTime = points.empty();
    tf::Matrix3x3 rotmat(-0.008324225326135968, -0.9999613549225855,     0.0028277082785044313,
                         -0.9997628867409561,    0.008265604744898349,  -0.020145720974759567,
                          0.02012156972284892,  -0.002994735312130247,  -0.9997930555831545);
    tf::Vector3 trans(0.054400047716549035,0.002726787577576585,-0.01724244334027849);
    tf::Transform T_cam_imu(rotmat, trans);

    // T_world_cam = T_world_imu * T_imu_cam
    tf::Transform T_world_cam = odom_to_tf(odom_msg) * T_cam_imu.inverse();
    
    std::vector<Elas::support_pt> tpoints;
    TriangleVec triUsed;
    project_points(&tpoints, &triUsed, T_world_cam.inverse());
    ROS_INFO_STREAM("projected " << tpoints.size() << " out of " << support_pt_cloud_.size());
    elas_->setSupportPoints(tpoints);
    elas_->setExistLeftTriangles(triUsed);
    // process
    elas_->process(l_image_data, r_image_data, l_disp_data, r_disp_data, dims);

    
    //ROS_INFO_STREAM("support points after: " << elas_->getSupportPoints().size());
    //ROS_INFO_STREAM("new support points: " << elas_->getNewSupportPoints().size());
    std::vector<elas_ros::SupportPoint3d> new_support_points_3d;
    support_points_to_3d(&new_support_points_3d, elas_->getNewSupportPoints(), T_world_cam);
    add_to_support_point_cloud(new_support_points_3d);
    const TriangleVec &new_tri = elas_->getNewLeftTriangles();
    triangle_cloud_.insert(triangle_cloud_.end(), new_tri.begin(), new_tri.end());
 
    // Find the max for scaling the image colour
    float disp_max = 0;
    for (int32_t i=0; i<width*height; i++)
    {
      if (l_disp_data[i]>disp_max) disp_max = l_disp_data[i];
      if (r_disp_data[i]>disp_max) disp_max = r_disp_data[i];
    }

    cv_bridge::CvImage out_depth_msg;
    out_depth_msg.header = l_image_msg->header;
    out_depth_msg.encoding = sensor_msgs::image_encodings::MONO16;
    out_depth_msg.image = cv::Mat(height, width, CV_16UC1);
    uint16_t * out_depth_msg_image_data = reinterpret_cast<uint16_t*>(&out_depth_msg.image.data[0]);

    cv_bridge::CvImage out_msg;
    out_msg.header = l_image_msg->header;
    out_msg.encoding = sensor_msgs::image_encodings::MONO8;
    out_msg.image = cv::Mat(height, width, CV_8UC1);
    std::vector<int32_t> inliers;
    for (int32_t i=0; i<width*height; i++)
    {
      out_msg.image.data[i] = (uint8_t)std::max(255.0*l_disp_data[i]/disp_max,0.0);
      //disp_msg->image.data[i] = l_disp_data[i];
      //disp_msg->image.data[i] = out_msg.image.data[i]

      float disp =  l_disp_data[i];
      // In milimeters
      //out_depth_msg_image_data[i] = disp;
      out_depth_msg_image_data[i] = disp <= 0.0f ? bad_point : (uint16_t)(depth_fact/disp);

      if (l_disp_data[i] > 0) inliers.push_back(i);
    }

    // Publish
    disp_pub_->publish(out_msg.toImageMsg());
    depth_pub_->publish(out_depth_msg.toImageMsg());
    publish_support_points(l_image_msg, elas_->getSupportPoints());
    publish_triangles(l_image_msg, elas_->getLeftTriangles());
#if 0    
    publish_point_cloud(l_image_msg, l_disp_data, inliers, width, height, l_info_msg, r_info_msg);
    publish_triangle_list(l_image_msg, elas_->getSupportPoints(),
                          elas_->getLeftTriangles(), l_info_msg, r_info_msg);
#else
    std::vector<Elas::sparse_triangle> new_triangles;
    image_geometry::StereoCameraModel model;
    model.fromCameraInfo(*l_info_msg, *r_info_msg);
    filter_triangles(&new_triangles, elas_->getSupportPoints(), elas_->getNewLeftTriangles(), model);
    publish_sparse_depth(T_world_cam.inverse(), new_support_points_3d, new_triangles, l_info_msg);
#endif    
    pub_disparity_.publish(disp_msg);

    // Cleanup data
    //delete l_disp_data;
    delete r_disp_data;
  }

private:

  ros::NodeHandle nh;
  Subscriber left_sub_, right_sub_;
  InfoSubscriber left_info_sub_, right_info_sub_;
  OdomSubscriber odom_sub_;
  dynamic_reconfigure::Server<elas_ros::ElasDynConfig>  configServer_;

  boost::shared_ptr<Publisher> disp_pub_;
  boost::shared_ptr<Publisher> depth_pub_;
  boost::shared_ptr<ros::Publisher> pc_pub_;
  boost::shared_ptr<ros::Publisher> elas_fd_pub_;
  boost::shared_ptr<ros::Publisher> support_pt_pub_;
  boost::shared_ptr<ros::Publisher> triangle_pub_;
  boost::shared_ptr<ros::Publisher> triangle_list_pub_;
  boost::shared_ptr<ros::Publisher> sparse_depth_pub_;
  boost::shared_ptr<ExactSync> exact_sync_;
  boost::shared_ptr<ApproximateSync> approximate_sync_;
  boost::shared_ptr<Elas> elas_;
  int queue_size_;

  image_geometry::StereoCameraModel model_;
  ros::Publisher pub_disparity_;

  Elas::parameters param_;
  typedef std::map<int64_t, cv::Point3d> SupportPointCloud;
  SupportPointCloud support_pt_cloud_;
  TriangleVec triangle_cloud_;
};

int main(int argc, char** argv)
{
  ros::init(argc, argv, "elas_ros");
  if (ros::names::remap("stereo") == "stereo") {
    ROS_WARN("'stereo' has not been remapped! Example command-line usage:\n"
             "\t$ rosrun viso2_ros stereo_odometer stereo:=narrow_stereo image:=image_rect");
  }
  if (ros::names::remap("image").find("rect") == std::string::npos) {
    ROS_WARN("stereo_odometer needs rectified input images. The used image "
             "topic is '%s'. Are you sure the images are rectified?",
             ros::names::remap("image").c_str());
  }

  std::string transport = argc > 1 ? argv[1] : "raw";
  Elas_Proc processor(transport);

  ros::spin();
  return 0;
}
