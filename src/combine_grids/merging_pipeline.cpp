/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2015-2016, Jiri Horner.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Jiri Horner nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *********************************************************************/

#include <combine_grids/grid_compositor.h>
#include <combine_grids/grid_warper.h>
#include <combine_grids/merging_pipeline.h>
#include <ros/assert.h>
#include <ros/console.h>

#include <opencv2/stitching/detail/matchers.hpp>
#include <opencv2/stitching/detail/motion_estimators.hpp>

#include "estimation_internal.h"

namespace combine_grids
{
bool MergingPipeline::estimateTransforms(FeatureType feature_type,
                                         double confidence)
{
  std::vector<cv::detail::ImageFeatures> image_features;
  std::vector<cv::detail::MatchesInfo> pairwise_matches;
  std::vector<cv::detail::CameraParams> transforms;
  std::vector<int> good_indices;
  // TODO investigate value translation effect on features
  auto finder = internal::chooseFeatureFinder(feature_type);
  cv::Ptr<cv::detail::FeaturesMatcher> matcher =
      cv::makePtr<cv::detail::AffineBestOf2NearestMatcher>();
  cv::Ptr<cv::detail::Estimator> estimator =
      cv::makePtr<cv::detail::AffineBasedEstimator>();
  cv::Ptr<cv::detail::BundleAdjusterBase> adjuster =
      cv::makePtr<cv::detail::BundleAdjusterAffinePartial>();

  if (images_.empty()) {
    return true;
  }

  /* find features in images */
  ROS_DEBUG("computing features");
  image_features.reserve(images_.size());
  for (const cv::Mat& image : images_) {
    image_features.emplace_back();
    if (!image.empty()) {
#if CV_VERSION_MAJOR >= 4
      cv::detail::computeImageFeatures(finder, image, image_features.back());
#else
      (*finder)(image, image_features.back());
#endif
    }
  }
  finder = {};

  /* find corespondent features */
  ROS_DEBUG("pairwise matching features");
  (*matcher)(image_features, pairwise_matches);
  matcher = {};

#ifndef NDEBUG
  internal::writeDebugMatchingInfo(images_, image_features, pairwise_matches);
#endif

  /* use only matches that has enough confidence. leave out matches that are not
   * connected (small components) */
  good_indices = cv::detail::leaveBiggestComponent(
      image_features, pairwise_matches, static_cast<float>(confidence));

  // no match found. try set first non-empty grid as reference frame. we try to
  // avoid setting empty grid as reference frame, in case some maps never
  // arrive. If all is empty just set null transforms.
  if (good_indices.size() == 1) {
    transforms_.clear();
    transforms_.resize(images_.size());
    for (size_t i = 0; i < images_.size(); ++i) {
      if (!images_[i].empty()) {
        // set identity
        transforms_[i] = cv::Mat::eye(3, 3, CV_64F);
        break;
      }
    }
    return true;
  }

  /* estimate transform */
  ROS_DEBUG("calculating transforms in global reference frame");
  // note: currently used estimator never fails
  if (!(*estimator)(image_features, pairwise_matches, transforms)) {
    return false;
  }

  /* levmarq optimization */
  // openCV just accepts float transforms
  for (auto& transform : transforms) {  
    transform.R.convertTo(transform.R, CV_32F);
  }
  ROS_DEBUG("optimizing global transforms");
  adjuster->setConfThresh(confidence);
  if (!(*adjuster)(image_features, pairwise_matches, transforms)) {
    ROS_WARN("Bundle adjusting failed. Could not estimate transforms.");
    return false;
  }

  transforms_.clear();
  transforms_.resize(images_.size());
  size_t i = 0;
  std::cout << "good_indices:" << good_indices.size() << std::endl;

  for (auto& j : good_indices) {
    std::cout << j << std::endl;
    // we want to work with transforms as doubles
    transforms[i].R.convertTo(transforms_[static_cast<size_t>(j)], CV_64F);
    ++i;
  }

  

  return true;
}

// checks whether given matrix is an identity, i.e. exactly appropriate Mat::eye
static inline bool isIdentity(const cv::Mat& matrix)
{
  if (matrix.empty()) {
    return false;
  }
  cv::MatExpr diff = matrix != cv::Mat::eye(matrix.size(), matrix.type());
  return cv::countNonZero(diff) == 0;
}

nav_msgs::OccupancyGrid::Ptr MergingPipeline::composeGrids()
{
  ROS_ASSERT(images_.size() == transforms_.size());
  ROS_ASSERT(images_.size() == grids_.size());

  if (images_.empty()) {
    std::cout << "no map images" << std::endl;
    return nullptr;
  }

  ROS_DEBUG("warping grids");
  internal::GridWarper warper;
  std::vector<cv::Mat> imgs_warped;
  imgs_warped.reserve(images_.size());
  std::vector<cv::Rect> rois;
  rois.reserve(images_.size());

  for (size_t i = 0; i < images_.size(); ++i) {
    if (!transforms_[i].empty() && !images_[i].empty()) {
      std::cout << i << " transfrom and image not empty" << std::endl;
      imgs_warped.emplace_back();
      rois.emplace_back(
          warper.warp(images_[i], transforms_[i], imgs_warped.back()));
    }
  }

  if (imgs_warped.empty()) {
    std::cout << "no imgs_warped" << std::endl;
    return nullptr;
  }

  ROS_DEBUG("compositing result grid");
  nav_msgs::OccupancyGrid::Ptr result;
  internal::GridCompositor compositor;
  result = compositor.compose(imgs_warped, rois);
  
  // set correct resolution to output grid. use resolution of identity (works
  // for estimated trasforms), or any resolution (works for know_init_positions)
  // - in that case all resolutions should be the same.
  float any_resolution = 0.0;
  for (size_t i = 0; i < transforms_.size(); ++i) {
    // check if this transform is the reference frame
    if (isIdentity(transforms_[i])) {
      result->info.resolution = grids_[i]->info.resolution;
      break;
    }
    if (grids_[i]) {
      any_resolution = grids_[i]->info.resolution;
    }
  }
  if (result->info.resolution <= 0.f) {
    result->info.resolution = any_resolution;
  }

  /********
   * Set correct transfrom for merged map
   * ******/
  if(!transforms_.empty())
  {
    double x = -transforms_[0].at<double>(0,2) - rois[0].tl().x;
    double y = -transforms_[0].at<double>(1,2) - rois[0].tl().y;
    result->info.origin.position.x = x*grids_[0]->info.resolution;
    result->info.origin.position.y = y*grids_[0]->info.resolution;
  }

  /********
   * Set vacancy for robots' positions
   * ******/
  tf::TransformListener listener;
  for(int i=0; i<3; i++)
  {
    tf::StampedTransform robotTF;
    int robotPixelPosX, robotPixelPosY = 0;
    geometry_msgs::PoseStamped robotPosBase;
    geometry_msgs::PoseStamped robotPosMap;
    robotPosBase.header.frame_id = "/tb3_"+std::to_string(i)+"/base_link";
    robotPosBase.pose.orientation.w = 1;
    listener.waitForTransform("/tb3_"+std::to_string(i)+"/base_link", "/map", ros::Time(0), ros::Duration(3.0));
    try{
      listener.transformPose("map", robotPosBase, robotPosMap);
    }
    catch(tf::TransformException &ex){
      ROS_ERROR("%s", ex.what());
    }
    robotPixelPosX = int((robotPosMap.pose.position.x - result->info.origin.position.x)/result->info.resolution);
    robotPixelPosY = int((robotPosMap.pose.position.y - result->info.origin.position.y)/result->info.resolution);
    std::cout << "robotPixelPosX: " << robotPixelPosX << std::endl;
    std::cout << "robotPixelPosY: " << robotPixelPosY << std::endl;
    for(int x=-3; x<3; x++)
    {
      for(int y=-3; y<3; y++)
      {
        if((robotPixelPosY + y)<result->info.height && (robotPixelPosX + x)<result->info.width &&
          (robotPixelPosY + y)>0 && (robotPixelPosX + x)>0)
        {
          result->data[(robotPixelPosY + y)*result->info.width + (robotPixelPosX + x)] = 0;
        }
      }
    }
  }

/*
  // set grid origin to its centre
  result->info.origin.position.x =
      -(result->info.width / 2.0) * double(result->info.resolution);
  result->info.origin.position.y =
      -(result->info.height / 2.0) * double(result->info.resolution);
  result->info.origin.orientation.w = 1.0;
*/
/*
  // /map to /map_tl
  tf::Transform tfMap2MapTL;
  tfMap2MapTL.setOrigin(tf::Vector3(
    result->info.origin.position.x,
    result->info.origin.position.y, //Top left corner
    0));
  tf::Quaternion qMap2MapTL; 
  qMap2MapTL.setRPY(0,0,0); //No rotation
  tfMap2MapTL.setRotation(qMap2MapTL);
  tfBroadcaster.sendTransform(tf::StampedTransform(tfMap2MapTL,ros::Time::now(),"/map","/map_tl"));

  for(size_t i = 0; i < images_.size(); ++i)
  {
    if (!transforms_[i].empty() && !images_[i].empty()) {
      std::cout << transforms_[i] << std::endl;
      // /map_tl to /tb3_* /map_tl
      tf::Transform tfMapTL2RobotMapTL;
      tfMapTL2RobotMapTL.setOrigin(tf::Vector3(
        transforms_[i].at<double>(0, 2)*result->info.resolution, //0.05 is the resolution of the grid map
        transforms_[i].at<double>(1, 2)*result->info.resolution,
        0.0));
      tf::Quaternion qTL2RobotMapTL;
      if(transforms_[i].at<double>(0, 0)>1)
      {
        qTL2RobotMapTL.setEulerZYX(0,0,0);
      }
      else
      {
        qTL2RobotMapTL.setEulerZYX(acos(transforms_[i].at<double>(0, 0)),0,0);
      }
      tfMapTL2RobotMapTL.setRotation(qTL2RobotMapTL);
      tfBroadcaster.sendTransform(tf::StampedTransform(tfMapTL2RobotMapTL,ros::Time::now(),"/map_tl","/tb3_"+std::to_string(i)+"/map_tl"));  
      // /tb3_* /map_tl to /tb3_* /map
      tf::Transform tfRobotMapTL2RobotMap;
      tfRobotMapTL2RobotMap.setOrigin(tf::Vector3(
        -grids_[i]->info.origin.position.x, //0.05 is the resolution of the grid map
        -grids_[i]->info.origin.position.y,
        0.0));
      tf::Quaternion q;
      q.setEulerZYX(0,0,0);
      tfMapTL2RobotMapTL.setRotation(q);
      tfBroadcaster.sendTransform(tf::StampedTransform(tfRobotMapTL2RobotMap,ros::Time::now(),"/tb3_"+std::to_string(i)+"/map_tl","/tb3_"+std::to_string(i)+"/map"));
    }  
  }
  */
  return result;
}

std::vector<geometry_msgs::Transform> MergingPipeline::getTransforms() const
{
  std::vector<geometry_msgs::Transform> result;
  result.reserve(transforms_.size());

  for (auto& transform : transforms_) {
    if (transform.empty()) {
      result.emplace_back();
      continue;
    }

    ROS_ASSERT(transform.type() == CV_64F);
    geometry_msgs::Transform ros_transform;
    ros_transform.translation.x = transform.at<double>(0, 2);
    ros_transform.translation.y = transform.at<double>(1, 2);
    ros_transform.translation.z = 0.;

    // our rotation is in fact only 2D, thus quaternion can be simplified
    double a = transform.at<double>(0, 0);
    double b = transform.at<double>(1, 0);
    ros_transform.rotation.w = std::sqrt(2. + 2. * a) * 0.5;
    ros_transform.rotation.x = 0.;
    ros_transform.rotation.y = 0.;
    ros_transform.rotation.z = std::copysign(std::sqrt(2. - 2. * a) * 0.5, b);

    result.push_back(ros_transform);
  }

  return result;
}

}  // namespace combine_grids
