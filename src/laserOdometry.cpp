#include "ros/ros.h"
#include <eigen3/Eigen/Dense>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <sensor_msgs/PointCloud2.h>
#include <nav_msgs/Odometry.h>
#include <nav_msgs/Path.h>
#include <queue>
#include <mutex>
#include "loam_velodyne/common.h"
#include "loam_velodyne/tic_toc.h"

#include "lidarFactor.hpp"



#define DISTORTION 0

constexpr double SCAN_PERIOD = 0.1;
constexpr double DISTANCE_SQ_THRESHOLD = 25;
int skipFrameNum = 5;
bool systemInited = false;
int corner_correspondence = 0;
int plane_correspondence = 0;
int NEARBY_SCAN = 2.5;

double timeCornerPointsSharp = 0;
double timeCornerPointsLessSharp = 0;
double timeSurfPointsFlat = 0;
double timeSurfPointsLessFlat = 0;
double timeLaserCloudFullRes = 0;

pcl::KdTreeFLANN<pcl::PointXYZI>::Ptr kdtreeCornerLast(new pcl::KdTreeFLANN<pcl::PointXYZI>());
pcl::KdTreeFLANN<pcl::PointXYZI>::Ptr kdtreeSurfLast(new pcl::KdTreeFLANN<pcl::PointXYZI>());

pcl::PointCloud<PointType>::Ptr cornerPointsSharp(new pcl::PointCloud<PointType>());
pcl::PointCloud<PointType>::Ptr cornerPointsLessSharp(new pcl::PointCloud<PointType>());
pcl::PointCloud<PointType>::Ptr surfPointsFlat(new pcl::PointCloud<PointType>());
pcl::PointCloud<PointType>::Ptr surfPointsLessFlat(new pcl::PointCloud<PointType>());
pcl::PointCloud<PointType>::Ptr laserCloudFullRes(new pcl::PointCloud<PointType>());

pcl::PointCloud<PointType>::Ptr laserCloudCornerLast(new pcl::PointCloud<PointType>());
pcl::PointCloud<PointType>::Ptr laserCloudSurfLast(new pcl::PointCloud<PointType>());

// transform form current frame to world frame
Eigen::Quaterniond q_w_curr(1,0,0,0);
Eigen::Vector3d t_w_curr(0,0,0);

// q_curr_last(x,y,z,w) t_curr_last
double para_q[4] = {0,0,0,1};
double para_t[3] = {0,0,0};

Eigen::Map<Eigen::Quaterniond> q_last_curr(para_q);
Eigen::Map<Eigen::Vector3d> t_last_curr(para_t);

std::queue<sensor_msgs::PointCloud2ConstPtr> cornerSharpBuf;
std::queue<sensor_msgs::PointCloud2ConstPtr> cornerLessSharpBuf;
std::queue<sensor_msgs::PointCloud2ConstPtr> surfFlatBuf;
std::queue<sensor_msgs::PointCloud2ConstPtr> surfLessFlatBuf;
std::queue<sensor_msgs::PointCloud2ConstPtr> fullPointsBuf;
std::mutex mBuf;

// ????????????????????????????????????????????????
void TransformToStart(PointType const * const pi, PointType* const po)
{
     // ????????????
     double s;
     if (DISTORTION)
     {
          s = (pi->intensity - int(pi->intensity)) / SCAN_PERIOD;
     }
     else
     {
          s = 1;
     }
     // ????????????????????????????????????????????????????????????????????????
     // ????????????????????????
     Eigen::Quaterniond q_point_last = Eigen::Quaterniond::Identity().slerp(s, q_last_curr);
     Eigen::Vector3d t_point_last = s * t_last_curr;
     Eigen::Vector3d point(pi->x, pi->y, pi->z);
     Eigen::Vector3d un_point = q_point_last * point + t_point_last;
     po->x = un_point.x();
     po->y = un_point.y();
     po->z = un_point.z();
     po->intensity = pi->intensity;
}

// ???????????????????????????????????????????????????
void TransformToEnd(PointType const* const pi, PointType* const po)
{
     pcl::PointXYZI un_point_tmp;
     TransformToStart(pi, &un_point_tmp);
     Eigen::Vector3d un_point(un_point_tmp.x, un_point_tmp.y, un_point_tmp.z);
     Eigen::Vector3d point_end = q_last_curr.inverse() * (un_point - t_last_curr);
     po->x = point_end.x();
     po->y = point_end.y();
     po->z = point_end.z();
     // remove distortion time infomation
     po->intensity = int(pi->intensity);
}

void laserCloudSharpHandler(const sensor_msgs::PointCloud2ConstPtr &cornerPointsSharp2)
{
     mBuf.lock();
     cornerSharpBuf.push(cornerPointsSharp2);
     mBuf.unlock();
}

void laserCloudLessSharpHandler(const sensor_msgs::PointCloud2ConstPtr& cornerPointsLessSharp2)
{
     mBuf.lock();
     cornerLessSharpBuf.push(cornerPointsLessSharp2);
     mBuf.unlock();
}

void laserCloudFlatHandler(const sensor_msgs::PointCloud2ConstPtr &surfPointsFlat2)
{
    mBuf.lock();
    surfFlatBuf.push(surfPointsFlat2);
    mBuf.unlock();
}

void laserCloudLessFlatHandler(const sensor_msgs::PointCloud2ConstPtr &surfPointsLessFlat2)
{
    mBuf.lock();
    surfLessFlatBuf.push(surfPointsLessFlat2);
    mBuf.unlock();
}

//receive all point cloud
void laserCloudFullResHandler(const sensor_msgs::PointCloud2ConstPtr &laserCloudFullRes2)
{
    mBuf.lock();
    fullPointsBuf.push(laserCloudFullRes2);
    mBuf.unlock();
}

int main(int argc, char* argv[])
{
     ros::init(argc, argv, "laserOdometry");
     ros::NodeHandle nh;

     // ?????????????????????????????????????????????????????????
     nh.param<int>("mapping_skip_frame", skipFrameNum, 2);
     std::cout << "mapping " << (10 / skipFrameNum) << "hz" << std::endl;
     // ???????????????????????????
     // ????????????????????????
     ros::Subscriber subCornerPointsSharp = nh.subscribe("/laser_cloud_sharp", 100, laserCloudSharpHandler);
     // ????????????????????????
     ros::Subscriber subCornerPointLessSharp = nh.subscribe("/laser_cloud_less_sharp", 100, laserCloudLessSharpHandler);
     // ????????????????????????
     ros::Subscriber subSurfPointFlat = nh.subscribe("laser_cloud_flat", 100, laserCloudFlatHandler);
     // ????????????????????????
     ros::Subscriber subSurfPointLessFlat = nh.subscribe("laser_cloud_less_flat", 100, laserCloudLessFlatHandler);
     // ???????????????????????????
     ros::Subscriber subLaserCloudFullRes = nh.subscribe("/velodyne_cloud_2", 100, laserCloudFullResHandler);
     // ??????????????????????????????????????????????????????????????????
     ros::Publisher pubLaserCloudCornerLast = nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_corner_last", 100); 
     // ????????????????????????????????????????????????????????????????????????)
     ros::Publisher pubLaserCloudSurfLast = nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_surf_last", 100);
     // // ?????????????????????????????????
     ros::Publisher pubLaserCloudFullRes = nh.advertise<sensor_msgs::PointCloud2>("/velodyne_cloud_3", 100);

     ros::Publisher pubLaserOdometry = nh.advertise<nav_msgs::Odometry>("/laser_odom_to_init", 100);

     ros::Publisher pubLaserPath = nh.advertise<nav_msgs::Path>("laser_odom_path", 100);

     int frameCount = 0;
     ros::Rate rate(100);
     while (ros::ok())
     {
         ros::spinOnce();
         // ????????????????????????5???????????????????????????????????????????????????
         if (!cornerSharpBuf.empty() && !cornerLessSharpBuf.empty() &&
               !surfFlatBuf.empty() && !surfLessFlatBuf.empty() && !fullPointsBuf.empty())
          {
               // ?????????????????????????????????
               timeCornerPointsSharp = cornerSharpBuf.front()->header.stamp.toNSec();
               timeCornerPointsLessSharp = cornerLessSharpBuf.front()->header.stamp.toSec();
               timeSurfPointsFlat = surfFlatBuf.front()->header.stamp.toSec();
               timeSurfPointsLessFlat = surfLessFlatBuf.front()->header.stamp.toSec();
               timeLaserCloudFullRes = fullPointsBuf.front()->header.stamp.toSec();
               // ?????????????????????????????????????????????????????????????????????????????????
               if (timeCornerPointsSharp !=  timeLaserCloudFullRes || timeCornerPointsLessSharp != timeLaserCloudFullRes ||
                    timeSurfPointsFlat != timeLaserCloudFullRes || timeSurfPointsLessFlat != timeLaserCloudFullRes)
               {
                    ROS_WARN("unsync messahe!");
                    ROS_BREAK();
               }
               // ???????????????????????????????????????????????????pcl????????????
               mBuf.lock();
               cornerPointsSharp->clear();
               pcl::fromROSMsg(*cornerSharpBuf.front(), *cornerPointsSharp);
               cornerSharpBuf.pop();

               cornerPointsLessSharp->clear();
               pcl::fromROSMsg(*cornerLessSharpBuf.front(), *cornerPointsLessSharp);
               cornerLessSharpBuf.pop();


               surfPointsFlat->clear();
               pcl::fromROSMsg(*surfFlatBuf.front(), *surfPointsFlat);
               surfFlatBuf.pop();

               surfPointsLessFlat->clear();
               pcl::fromROSMsg(*surfLessFlatBuf.front(), *surfPointsLessFlat);
               surfLessFlatBuf.pop();

               laserCloudFullRes->clear();
               pcl::fromROSMsg(*fullPointsBuf.front(), *laserCloudFullRes);
               fullPointsBuf.pop();
               mBuf.unlock();

               TicToc t_whole;
               // initializing
               // ???????????????????????????
               if (!systemInited)
               {
                    systemInited = true;
                    std::cout << "Initialization finished \n";
               }
               else
               {
                    // ?????????????????????(???????????????????????????????????????)
                    int cornerPointsSharpNum = cornerPointsSharp->size();
                    int surfPointsFlatNum = surfPointsFlat->points.size();

                    TicToc t_opt;
                    // ????????????????????????
                    for (size_t opti_counter = 0; opti_counter < 2; ++opti_counter)
                    {
                         corner_correspondence = 0;
                         plane_correspondence = 0;
                         // ??????ceres?????????????????????????????????????????????????????????????????????????????????
                         // hubbr?????????????????????0.1??????????????????0.1,??????????????????????????????0.1????????????
                         ceres::LossFunction* loss_function = new ceres::HuberLoss(0.1);
                         // ??????????????????????????????????????????????????????????????????ceres?????????local param
                         ceres::LocalParameterization* q_parameterization = 
                              new ceres::EigenQuaternionParameterization();
                         ceres::Problem::Options problem_options;
                         ceres::Problem problem(problem_options);
                         // ??????????????????????????????????????????????????????????????????????????????????????????
                         problem.AddParameterBlock(para_q, 4, q_parameterization);
                         problem.AddParameterBlock(para_t, 3);

                         pcl::PointXYZI pointSel; // ???????????????????????????????????????
                         std::vector<int> pointSearchInd; // ???kd-tree????????????????????????????????????
                         std::vector<float> pointSearchSqDis; // ??????????????????????????????

                         TicToc t_data;
                         // ?????????????????????
                         for (int i = 0; i < cornerPointsSharpNum; ++i)
                         {
                              // ????????????
                              TransformToStart(&(cornerPointsSharp->points[i]), &pointSel);
                              // ?????????????????????????????????kdtree??????????????????????????????????????????
                              kdtreeCornerLast->nearestKSearch(pointSel, 1, pointSearchInd, pointSearchSqDis);
                              
                              int closestPointInd = -1, minPointInd2 = -1;
                              // ?????????????????????????????????????????????????????????????????????????????????????????????????????????
                              if (pointSearchSqDis[0] < DISTANCE_SQ_THRESHOLD)
                              {
                                   closestPointInd = pointSearchInd[0]; // ???????????????????????????
                                   // ????????????????????????id??????????????????intensity???????????????
                                   int closestPointScanID = int(laserCloudCornerLast->points[closestPointInd].intensity);
                                   double minPointSqDis2 = DISTANCE_SQ_THRESHOLD;
                                   // ????????????????????????????????????????????????(1)???????????????????????????scna???(2)??????????????????
                                   for (int j = closestPointInd + 1; j < (int)laserCloudCornerLast->points.size(); ++j)
                                   {
                                        // ????????????(1)
                                        if (int(laserCloudCornerLast->points[j].intensity) <= closestPointScanID) continue;
                                        if (int(laserCloudCornerLast->points[j].intensity) > (closestPointScanID + NEARBY_SCAN)) break;
                                        // ????????????????????????????????????
                                        double pointSqDis = (laserCloudCornerLast->points[j].x - pointSel.x) * 
                                                                 (laserCloudCornerLast->points[j].x - pointSel.x)  + 
                                                            (laserCloudCornerLast->points[j].y - pointSel.y) * 
                                                                 (laserCloudCornerLast->points[j].y - pointSel.y) + 
                                                            (laserCloudCornerLast->points[j].z - pointSel.z) * 
                                                                 (laserCloudCornerLast->points[j].z - pointSel.z);
                                        if (pointSqDis < minPointSqDis2)
                                        {
                                             // ????????????
                                             minPointSqDis2 = pointSqDis;
                                             minPointInd2 = j;
                                        }
                                   }
                                   // ???????????????  ?????????????????????????????????
                                   for (int j = closestPointInd - 1; j >= 0; --j)
                                   {
                                        // ????????????(1)
                                        if (int(laserCloudCornerLast->points[j].intensity) >= closestPointScanID) continue;
                                        if (int(laserCloudCornerLast->points[j].intensity) < (closestPointScanID - NEARBY_SCAN)) break;
                                        // ????????????????????????????????????
                                        double pointSqDis = (laserCloudCornerLast->points[j].x - pointSel.x) * 
                                                                 (laserCloudCornerLast->points[j].x - pointSel.x)  + 
                                                            (laserCloudCornerLast->points[j].y - pointSel.y) * 
                                                                 (laserCloudCornerLast->points[j].y - pointSel.y) + 
                                                            (laserCloudCornerLast->points[j].z - pointSel.z) * 
                                                                 (laserCloudCornerLast->points[j].z - pointSel.z);
                                        if (pointSqDis < minPointSqDis2)
                                        {
                                             // ????????????
                                             minPointSqDis2 = pointSqDis;
                                             minPointInd2 = j;
                                        }
                                   }
                              }
                              // ??????????????????????????????
                              if (minPointInd2 >= 0)
                              {
                                   // ???????????????????????????????????????
                                   Eigen::Vector3d curr_point(cornerPointsSharp->points[i].x,
                                                              cornerPointsSharp->points[i].y,
                                                              cornerPointsSharp->points[i].z);
                                   Eigen::Vector3d last_point_a(laserCloudCornerLast->points[closestPointInd].x,
                                                                 laserCloudCornerLast->points[closestPointInd].y,
                                                                 laserCloudCornerLast->points[closestPointInd].z);
                                   Eigen::Vector3d last_point_b(laserCloudCornerLast->points[minPointInd2].x,
                                                                 laserCloudCornerLast->points[minPointInd2].y,
                                                                 laserCloudCornerLast->points[minPointInd2].z);
                                   double s;
                                   if (DISTORTION)
                                   {
                                        s = (cornerPointsSharp->points[i].intensity - int(cornerPointsSharp->points[i].intensity)) / SCAN_PERIOD;
                                   }
                                   else s = 1;
                                   ceres::CostFunction* cost_function = LidarEdgeFactor::Create(curr_point, last_point_a, last_point_b, s);
                                   problem.AddResidualBlock(cost_function, loss_function, para_q, para_t);
                                   corner_correspondence++;
                              }
                         }

                         // ??????????????????
                         for (int i = 0; i < surfPointsFlatNum; ++i)
                         {
                              TransformToStart(&(surfPointsFlat->points[i]), &pointSel);
                              kdtreeSurfLast->nearestKSearch(pointSel, 1, pointSearchInd, pointSearchSqDis);
                              int closestPointInd = -1, minPointInd2 = -1, minPointInd3 = -1;
                              if (pointSearchSqDis[0] < DISTANCE_SQ_THRESHOLD)
                              {
                                   closestPointInd = pointSearchInd[0];
                                   int closestPointScanID = int(laserCloudSurfLast->points[closestPointInd].intensity);
                                   double minPointSqDis2 = DISTANCE_SQ_THRESHOLD, minPointSqDis3 = DISTANCE_SQ_THRESHOLD;
                                   for (int j = 0; j < (int)laserCloudSurfLast->points.size(); ++j)
                                   {
                                        if (int(laserCloudSurfLast->points[j].intensity) > (closestPointScanID + NEARBY_SCAN)) break;
                                        double pointSqDis = (laserCloudSurfLast->points[j].x - pointSel.x) * 
                                                                 (laserCloudSurfLast->points[j].x - pointSel.x) +
                                                            (laserCloudSurfLast->points[j].y - pointSel.y) * 
                                                                 (laserCloudSurfLast->points[j].y - pointSel.y) + 
                                                            (laserCloudSurfLast->points[j].z - pointSel.z) *
                                                                 (laserCloudSurfLast->points[j].z - pointSel.z);
                                        if (int(laserCloudSurfLast->points[j].intensity) <= closestPointScanID && pointSqDis < minPointSqDis2)
                                        {
                                             minPointSqDis2 = pointSqDis;
                                             minPointInd2 = j;
                                        }
                                        else if (int(laserCloudSurfLast->points[j].intensity) > closestPointScanID && pointSqDis < minPointSqDis3)
                                        {
                                             minPointSqDis3 = pointSqDis;
                                             minPointInd3 = j;
                                        }
                                   }
                                   for (int j = closestPointInd - 1; j >= 0; --j)
                                   {
                                        if (int(laserCloudSurfLast->points[j].intensity) < (closestPointScanID - NEARBY_SCAN)) break;
                                        double pointSqDis = (laserCloudSurfLast->points[j].x - pointSel.x) * 
                                                                 (laserCloudSurfLast->points[j].x - pointSel.x) +
                                                            (laserCloudSurfLast->points[j].y - pointSel.y) * 
                                                                 (laserCloudSurfLast->points[j].y - pointSel.y) + 
                                                            (laserCloudSurfLast->points[j].z - pointSel.z) *
                                                                 (laserCloudSurfLast->points[j].z - pointSel.z);
                                        if (int(laserCloudSurfLast->points[j].intensity) >= closestPointScanID && pointSqDis < minPointSqDis2)
                                        {
                                             minPointSqDis2 = pointSqDis;
                                             minPointInd2 = j;
                                        }
                                        else if (int(laserCloudSurfLast->points[j].intensity) < closestPointScanID && pointSqDis < minPointSqDis3)
                                        {
                                             minPointSqDis3 = pointSqDis;
                                             minPointInd3 = j;
                                        }
                                   }
                                   if (minPointInd2 >= 0 && minPointInd3 >= 0)
                                   {
                                        Eigen::Vector3d curr_point(surfPointsFlat->points[i].x,
                                                                   surfPointsFlat->points[i].y,
                                                                   surfPointsFlat->points[i].z);
                                        Eigen::Vector3d last_point_a(laserCloudSurfLast->points[closestPointInd].x,
                                                                     laserCloudSurfLast->points[closestPointInd].y,
                                                                     laserCloudSurfLast->points[closestPointInd].z);
                                        Eigen::Vector3d last_point_b(laserCloudSurfLast->points[minPointInd2].x,
                                                                     laserCloudSurfLast->points[minPointInd2].y,
                                                                     laserCloudSurfLast->points[minPointInd2].z);
                                        Eigen::Vector3d last_point_c(laserCloudSurfLast->points[minPointInd3].x,
                                                                     laserCloudSurfLast->points[minPointInd3].y,
                                                                     laserCloudSurfLast->points[minPointInd3].z);
                                        double s;
                                        if (DISTORTION)
                                        {
                                             s = (surfPointsFlat->points[i].intensity - int(surfPointsFlat->points[i].intensity)) / SCAN_PERIOD;
                                        }
                                        else s = 1;
                                        ceres::CostFunction* cost_function = LidarPlaneFactor::Crete(curr_point, last_point_a, last_point_b, last_point_c, s);
                                        problem.AddResidualBlock(cost_function, loss_function, para_q, para_t);
                                        plane_correspondence++;
                                   }
                              }
                         }
                         std::cout << "corner_correspondence: " << corner_correspondence << ", plane_correspondence: " << plane_correspondence << std::endl;
                         std::cout << "data association time: " << t_data.toc() << std::endl;
                         if (corner_correspondence + plane_correspondence < 10)
                         {
                              ROS_WARN("***********************less correspondence!***********************");
                         }

                         // ??????ceres?????????
                         TicToc t_solver;
                         ceres::Solver::Options options;
                         options.linear_solver_type = ceres::DENSE_QR;
                         options.max_num_iterations = 4;
                         options.minimizer_progress_to_stdout = false;
                         ceres::Solver::Summary summary;
                         ceres::Solve(options, &problem, &summary);
                         std::cout << "solver time: " << t_solver.toc() << "ms" << std::endl;
                    }
                    std::cout << "optimization twice time: " << t_opt.toc() << std::endl;
                    // ?????????w_curr ???????????? w_last
                    t_w_curr = t_w_curr + q_w_curr * t_last_curr;
                    q_w_curr = q_w_curr * q_last_curr;
               }
               TicToc t_pub;
               // ???????????????????????????
               nav_msgs::Odometry laserOdometry;
               laserOdometry.header.frame_id = "/camera_init";
               laserOdometry.child_frame_id = "/laser_odom";
               laserOdometry.header.stamp = ros::Time().fromSec(timeSurfPointsLessFlat);
               // ????????????????????????????????????
               laserOdometry.pose.pose.orientation.x = q_w_curr.x();
               laserOdometry.pose.pose.orientation.y = q_w_curr.y();
               laserOdometry.pose.pose.orientation.z = q_w_curr.z();
               laserOdometry.pose.pose.orientation.w = q_w_curr.w();
               laserOdometry.pose.pose.position.x = t_w_curr.x();
               laserOdometry.pose.pose.position.y = t_w_curr.y();
               laserOdometry.pose.pose.position.z = t_w_curr.z();
               pubLaserOdometry.publish(laserOdometry);

               geometry_msgs::PoseStamped laserPose;
               nav_msgs::Path laserPath;
               laserPose.header = laserOdometry.header;
               laserPose.pose = laserOdometry.pose.pose;
               laserPath.header.stamp = laserOdometry.header.stamp;
               laserPath.poses.push_back(laserPose);
               laserPath.header.frame_id = "/camera_init";
               pubLaserPath.publish(laserPath);

               pcl::PointCloud<PointType>::Ptr laserCloudTemp = cornerPointsLessSharp;
               cornerPointsLessSharp = laserCloudCornerLast;
               laserCloudCornerLast = laserCloudTemp;

               laserCloudTemp = surfPointsLessFlat;
               surfPointsLessFlat = laserCloudSurfLast;
               laserCloudSurfLast = laserCloudTemp;

               kdtreeCornerLast->setInputCloud(laserCloudCornerLast);
               kdtreeCornerLast->setInputCloud(laserCloudSurfLast);

               // ??????????????????????????????
               if (frameCount % skipFrameNum == 0)
               {
                    frameCount = 0;

                    // ??????????????????????????????????????????????????????????????????
                    sensor_msgs::PointCloud2 laserCloudCornerLast2;
                    pcl::toROSMsg(*laserCloudCornerLast, laserCloudCornerLast2);
                    laserCloudCornerLast2.header.stamp = ros::Time().fromSec(timeSurfPointsLessFlat);
                    laserCloudCornerLast2.header.frame_id = "/camera";
                    pubLaserCloudCornerLast.publish(laserCloudCornerLast2);

                    // ????????????????????????????????????????????????????????????????????????)
                    sensor_msgs::PointCloud2 laserCloudSurfLast2;
                    pcl::toROSMsg(*laserCloudSurfLast, laserCloudSurfLast2);
                    laserCloudSurfLast2.header.stamp = ros::Time().fromSec(timeSurfPointsLessFlat);
                    laserCloudSurfLast2.header.frame_id = "/camera";
                    pubLaserCloudSurfLast.publish(laserCloudSurfLast2);

                    // ?????????????????????????????????
                    sensor_msgs::PointCloud2 laserCloudFullRes3;
                    pcl::toROSMsg(*laserCloudFullRes, laserCloudFullRes3);
                    laserCloudFullRes3.header.stamp = ros::Time().fromSec(timeSurfPointsLessFlat);
                    laserCloudFullRes3.header.frame_id = "/camera";
                    pubLaserCloudFullRes.publish(laserCloudFullRes3);
               }
               std::cout << "publication time: " << t_pub.toc() << "ms \n";
               std::cout << "whole laserOdometry time: " << t_whole.toc() << "ms\n";
               if (t_whole.toc() > 100)
               {
                    ROS_WARN("odometry process over 100ms");
               }
               frameCount++;
          }
          rate.sleep();
     }
     return 0;
}