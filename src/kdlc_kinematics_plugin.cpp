/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Willow Garage, Inc.
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
 *   * Neither the name of the Willow Garage nor the names of its
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
 * Author: Sachin Chitta, David Lu!!, Ugo Cupcic, Dave Coleman
 *********************************************************************/

#include <moveit/kdlc_kinematics_plugin/kdlc_kinematics_plugin.h>
#include <class_loader/class_loader.h>

//#include <tf/transform_datatypes.h>
#include <tf_conversions/tf_kdl.h>
#include <kdl_parser/kdl_parser.hpp>

// URDF, SRDF
#include <urdf_model/model.h>
#include <srdfdom/model.h>

#include <moveit/rdf_loader/rdf_loader.h>

// C++
#include <numeric>

static const double MAX_TIMEOUT_KDLC_PLUGIN = 5.0;

//register KDLCKinematics as a KinematicsBase implementation
CLASS_LOADER_REGISTER_CLASS(kdlc_kinematics_plugin::KDLCKinematicsPlugin, kinematics::KinematicsBase)

namespace kdlc_kinematics_plugin
{

KDLCKinematicsPlugin::KDLCKinematicsPlugin():active_(false){}

void KDLCKinematicsPlugin::getRandomConfiguration(KDL::JntArray &jnt_array) const
{
  std::vector<double> jnt_array_vector(dimension_,0.0);
  robot_state::JointStateGroup*  joint_state_group = kinematic_state_->getJointStateGroup(getGroupName());
  joint_state_group->setToRandomValues();
  joint_state_group->getVariableValues(jnt_array_vector);
  for(std::size_t i=0; i < dimension_; ++i)
    jnt_array(i) = jnt_array_vector[i];
}

void KDLCKinematicsPlugin::getRandomConfiguration(const KDL::JntArray &seed_state,
                                                  const std::vector<double> &consistency_limits,
                                                  KDL::JntArray &jnt_array) const
{
  std::vector<double> values, near;
  for(std::size_t i=0; i < dimension_; ++i)
  {
    near.push_back(seed_state(i));
  }
  robot_state::JointStateGroup*  joint_state_group = kinematic_state_->getJointStateGroup(getGroupName());
  joint_state_group->setToRandomValuesNearBy(near, consistency_limits);
  joint_state_group->getVariableValues(values);
  for(std::size_t i=0; i < dimension_; ++i)
  {
    jnt_array(i) = values[i];
  }
}

bool KDLCKinematicsPlugin::checkConsistency(const KDL::JntArray& seed_state,
                                            const std::vector<double> &consistency_limits,
                                            const KDL::JntArray& solution) const
{
  std::vector<double> seed_state_vector(dimension_), solution_vector(dimension_);
  for(std::size_t i = 0; i < dimension_; ++i)
  {
    seed_state_vector[i] = seed_state(i);
    solution_vector[i] = solution(i);
  }
  robot_state::JointStateGroup* joint_state_group = kinematic_state_->getJointStateGroup(getGroupName());
  robot_state::JointStateGroup* joint_state_group_2 = kinematic_state_2_->getJointStateGroup(getGroupName());
  joint_state_group->setVariableValues(seed_state_vector);
  joint_state_group_2->setVariableValues(solution_vector);

  const std::vector<robot_state::JointState*>& joint_state_vector = joint_state_group->getJointStateVector();
  const std::vector<robot_state::JointState*>& joint_state_vector_2 = joint_state_group_2->getJointStateVector();

  for(std::size_t i = 0; i < joint_state_vector.size(); ++i)
  {
    if(joint_state_vector[i]->distance(joint_state_vector_2[i]) > consistency_limits[i])
      return false;
  }

  return true;
}

bool KDLCKinematicsPlugin::initialize(const std::string &robot_description,
                                      const std::string& group_name,
                                      const std::string& base_frame,
                                      const std::string& tip_frame,
                                      double search_discretization)
{
  ROS_DEBUG_STREAM_NAMED("kdlc","Initializing kdlc solver");


  setValues(robot_description, group_name, base_frame, tip_frame, search_discretization);

  ros::NodeHandle private_handle("~");
  rdf_loader::RDFLoader rdf_loader(robot_description_);
  const boost::shared_ptr<srdf::Model> &srdf = rdf_loader.getSRDF();
  const boost::shared_ptr<urdf::ModelInterface>& urdf_model = rdf_loader.getURDF();

  kinematic_model_.reset(new robot_model::RobotModel(urdf_model, srdf));

  if(!kinematic_model_->hasJointModelGroup(group_name))
  {
    ROS_ERROR("Kinematic model does not contain group %s",group_name.c_str());
    return false;
  }
  robot_model::JointModelGroup* joint_model_group = kinematic_model_->getJointModelGroup(group_name);
  if(!joint_model_group->isChain())
  {
    ROS_ERROR("Group is not a chain");
    return false;
  }

  KDL::Tree kdl_tree;

  if (!kdl_parser::treeFromUrdfModel(*urdf_model, kdl_tree))
  {
    ROS_ERROR("Could not initialize tree object");
    return false;
  }
  if (!kdl_tree.getChain(base_frame_, tip_frame_, kdl_chain_))
  {
    ROS_ERROR("Could not initialize chain object");
    return false;
  }

  dimension_ = joint_model_group->getVariableCount();
  jnt_seed_state_.resize(dimension_);
  jnt_pos_in_.resize(dimension_);
  jnt_pos_out_.resize(dimension_);
  ik_chain_info_.joint_names = joint_model_group->getJointModelNames();
  ik_chain_info_.limits = joint_model_group->getVariableLimits();
  fk_chain_info_.joint_names = ik_chain_info_.joint_names;
  fk_chain_info_.limits = ik_chain_info_.limits;

  if(!joint_model_group->hasLinkModel(tip_frame_))
  {
    ROS_ERROR("Could not find tip name in joint group");
    return false;
  }
  ik_chain_info_.link_names.push_back(tip_frame_);
  fk_chain_info_.link_names = joint_model_group->getLinkModelNames();

  joint_min_.resize(ik_chain_info_.limits.size());
  joint_max_.resize(ik_chain_info_.limits.size());

  for(unsigned int i=0; i < ik_chain_info_.limits.size(); i++)
  {
    joint_min_(i) = ik_chain_info_.limits[i].min_position;
    joint_max_(i) = ik_chain_info_.limits[i].max_position;
  }

  // Get Solver Parameters
  int max_solver_iterations;
  double epsilon;

  private_handle.param("max_solver_iterations", max_solver_iterations, 500);
  private_handle.param("epsilon", epsilon, 1e-5);

  // Build Solvers
  fk_solver_.reset(new KDL::ChainFkSolverPos_recursive(kdl_chain_));
  ik_solver_vel_.reset(new KDL::ChainIkSolverVel_pinv(kdl_chain_));
  ik_solver_pos_.reset(new KDL::ChainIkSolverPos_NR_JL(kdl_chain_, joint_min_, joint_max_,*fk_solver_, *ik_solver_vel_, max_solver_iterations, epsilon));

  // Setup the joint state groups that we need
  kinematic_state_.reset(new robot_state::RobotState((const robot_model::RobotModelConstPtr) kinematic_model_));
  kinematic_state_2_.reset(new robot_state::RobotState((const robot_model::RobotModelConstPtr) kinematic_model_));



  // -----------------------------------------------------------------------------------------------
  // DTC

  // Assign id to this instance of the object
  static int num_instances = 0;
  this_instance_id_ = num_instances;
  num_instances ++;

  ROS_WARN_STREAM("Initializing kdlc solver " << this_instance_id_);

  // Check if we need to load the cache_
  static bool cache_loaded = false;
  if( !cache_loaded )
  {
    ROS_INFO_STREAM_NAMED("kdlc","Cache is not loaded, opening file");

    // Get path to data file      TODO: make this cross-platform
    cache_location_ = "/home/dave"; //getenv("HOME");
    cache_location_ = cache_location_ + "/.ros/kdlc_cache.dat";
    ROS_INFO_STREAM_NAMED("kdlc","Using cache at " << cache_location_);

    // Load IK Cache
    // TODO: dynamically set the joint and pose limits
    bool verbose_cache = false;
    cache_.reset(new simple_cache::SimpleCache(dimension_, verbose_cache, 2.7, -2.7, 1.0, -1.0));

    // Open the data file
    cache_->readFile(cache_location_);

    // Setup the data file to auto-write to disk
    cache_->startAppend(cache_location_);

    // Remember we have loaded it
    cache_loaded = true;
  }

  // DTC
  // -----------------------------------------------------------------------------------------------


  active_ = true;
  ROS_DEBUG_STREAM_NAMED("kdlc","KDLC solver initialized");
  return true;
}

int KDLCKinematicsPlugin::getJointIndex(const std::string &name) const
{
  for (unsigned int i=0; i < ik_chain_info_.joint_names.size(); i++) {
    if (ik_chain_info_.joint_names[i] == name)
      return i;
  }
  return -1;
}

int KDLCKinematicsPlugin::getKDLSegmentIndex(const std::string &name) const
{
  int i=0;
  while (i < (int)kdl_chain_.getNrOfSegments()) {
    if (kdl_chain_.getSegment(i).getName() == name) {
      return i+1;
    }
    i++;
  }
  return -1;
}

bool KDLCKinematicsPlugin::timedOut(const ros::WallTime &start_time, double duration) const
{
  return ((ros::WallTime::now()-start_time).toSec() >= duration);
}

bool KDLCKinematicsPlugin::getPositionIK(const geometry_msgs::Pose &ik_pose,
                                         const std::vector<double> &ik_seed_state,
                                         std::vector<double> &solution,
                                         moveit_msgs::MoveItErrorCodes &error_code) const
{
  const IKCallbackFn solution_callback = 0;
  std::vector<double> consistency_limits;

  return searchPositionIK(ik_pose,
                          ik_seed_state,
                          MAX_TIMEOUT_KDLC_PLUGIN,
                          solution,
                          solution_callback,
                          error_code,
                          consistency_limits);
}

bool KDLCKinematicsPlugin::searchPositionIK(const geometry_msgs::Pose &ik_pose,
                                            const std::vector<double> &ik_seed_state,
                                            double timeout,
                                            std::vector<double> &solution,
                                            moveit_msgs::MoveItErrorCodes &error_code) const
{
  const IKCallbackFn solution_callback = 0;
  std::vector<double> consistency_limits;

  return searchPositionIK(ik_pose,
                          ik_seed_state,
                          timeout,
                          solution,
                          solution_callback,
                          error_code,
                          consistency_limits);
}

bool KDLCKinematicsPlugin::searchPositionIK(const geometry_msgs::Pose &ik_pose,
                                            const std::vector<double> &ik_seed_state,
                                            double timeout,
                                            const std::vector<double> &consistency_limits,
                                            std::vector<double> &solution,
                                            moveit_msgs::MoveItErrorCodes &error_code) const
{
  const IKCallbackFn solution_callback = 0;
  return searchPositionIK(ik_pose,
                          ik_seed_state,
                          timeout,
                          solution,
                          solution_callback,
                          error_code,
                          consistency_limits);
}

bool KDLCKinematicsPlugin::searchPositionIK(const geometry_msgs::Pose &ik_pose,
                                            const std::vector<double> &ik_seed_state,
                                            double timeout,
                                            std::vector<double> &solution,
                                            const IKCallbackFn &solution_callback,
                                            moveit_msgs::MoveItErrorCodes &error_code) const
{
  std::vector<double> consistency_limits;
  return searchPositionIK(ik_pose,
                          ik_seed_state,
                          timeout,
                          solution,
                          solution_callback,
                          error_code,
                          consistency_limits);
}

bool KDLCKinematicsPlugin::searchPositionIK(const geometry_msgs::Pose &ik_pose,
                                            const std::vector<double> &ik_seed_state,
                                            double timeout,
                                            const std::vector<double> &consistency_limits,
                                            std::vector<double> &solution,
                                            const IKCallbackFn &solution_callback,
                                            moveit_msgs::MoveItErrorCodes &error_code) const
{
  return searchPositionIK(ik_pose,
                          ik_seed_state,
                          timeout,
                          solution,
                          solution_callback,
                          error_code,
                          consistency_limits);
}

bool KDLCKinematicsPlugin::searchPositionIK(const geometry_msgs::Pose &ik_pose,
                                            const std::vector<double> &ik_seed_state,
                                            double timeout,
                                            std::vector<double> &solution,
                                            const IKCallbackFn &solution_callback,
                                            moveit_msgs::MoveItErrorCodes &error_code,
                                            const std::vector<double> &consistency_limits) const
{
  ros::WallTime n1 = ros::WallTime::now();
  if(!active_)
  {
    ROS_ERROR("kinematics not active");
    error_code.val = error_code.NO_IK_SOLUTION;
    return false;
  }

  // --------------------------------------------------------------------------------------------------------
  // DTC

  // Get seed state from cache if one is available
  std::vector<double> ik_seed_state_new = ik_seed_state; // copy to non-const vector

  simple_cache::results_t cache_result = cache_->get(ik_pose, ik_seed_state_new);
  if( cache_result == simple_cache::SUCCESS )
  {
    // Since we are pulling the result from cache, we can lower the timeout
    timeout = timeout * 0.00001;

    ROS_DEBUG_STREAM_NAMED("kdlc","ik result from cache. new timeout is " << timeout);  
  }
  else if( cache_result == simple_cache::NOSOLUTION)
  {
    ROS_DEBUG_STREAM_NAMED("kdlc","ik result from cache- no solution");
    error_code.val = error_code.NO_IK_SOLUTION;
    return false;
  }
  else
  {
    //ROS_ERROR_STREAM_NAMED("kdlc","pose not in ik cache");
  }

  // DTC
  // --------------------------------------------------------------------------------------------------------

  if(ik_seed_state_new.size() != dimension_)
  {
    ROS_ERROR_STREAM("Seed state must have size " << dimension_ << " instead of size " << ik_seed_state_new.size());
    error_code.val = error_code.NO_IK_SOLUTION;
    return false;
  }

  if(!consistency_limits.empty() && consistency_limits.size() != dimension_)
  {
    ROS_ERROR_STREAM("Consistency limits be empty or must have size " << dimension_ << " instead of size " << consistency_limits.size());
    error_code.val = error_code.NO_IK_SOLUTION;
    return false;
  }

  solution.resize(dimension_);

  KDL::Frame pose_desired;
  tf::poseMsgToKDL(ik_pose, pose_desired);

  ROS_DEBUG_STREAM_NAMED("kdlc_kdl","searchPositionIK2: Position request pose is " <<
                         ik_pose.position.x << " " <<
                         ik_pose.position.y << " " <<
                         ik_pose.position.z << " " <<
                         ik_pose.orientation.x << " " <<
                         ik_pose.orientation.y << " " <<
                         ik_pose.orientation.z << " " <<
                         ik_pose.orientation.w);
  //Do the IK
  for(unsigned int i=0; i < dimension_; i++)
    jnt_seed_state_(i) = ik_seed_state_new[i];
  jnt_pos_in_ = jnt_seed_state_;

  unsigned int counter(0);
  bool result = false; // state the function will return in
  while(1)
  {
    //    ROS_DEBUG_STREAM_NAMED("kdlc_kdl","Iteration: %d, time: %f, Timeout: %f",counter,(ros::WallTime::now()-n1).toSec(),timeout);
    counter++;
    if(timedOut(n1,timeout))
    {
      ROS_DEBUG_STREAM_NAMED("kdlc","IK timed out");
      error_code.val = error_code.TIMED_OUT;
      result = false;
      break;
    }
    int ik_valid = ik_solver_pos_->CartToJnt(jnt_pos_in_,pose_desired,jnt_pos_out_);
    if(!consistency_limits.empty())
    {
      getRandomConfiguration(jnt_seed_state_, consistency_limits, jnt_pos_in_);
      if(ik_valid < 0 || !checkConsistency(jnt_seed_state_, consistency_limits, jnt_pos_out_))
      {
        ROS_DEBUG_NAMED("kdlc_kdl","Could not find IK solution");
        continue;
      }
    }
    else
    {
      getRandomConfiguration(jnt_pos_in_);
      if(ik_valid < 0)
      {
        ROS_DEBUG_NAMED("kdlc_kdl","Could not find IK solution");
        continue;
      }
    }
    ROS_DEBUG_STREAM_NAMED("kdlc","Found IK solution");
    for(unsigned int j=0; j < dimension_; j++)
      solution[j] = jnt_pos_out_(j);
    if(!solution_callback.empty())
      solution_callback(ik_pose,solution,error_code);
    else
      error_code.val = error_code.SUCCESS;

    if(error_code.val == error_code.SUCCESS)
    {
      //ROS_DEBUG_STREAM("Solved after " << counter << " iterations");
      result = true;
      break;
    }
  } // while

  // --------------------------------------------------------------------------------------------------------
  // DTC
  // if the cache did not have an entry, add it
  if( cache_result != simple_cache::SUCCESS && cache_result != simple_cache::NOSOLUTION)
  {
    //ROS_WARN_STREAM_NAMED("grasp","inserting into ik cache");

    if( !result ) // no solution found
    {
      // check if vector is all zeros
      double sum = std::accumulate(solution.begin(),solution.end(),0);
      simple_cache::results_t cache_result2;
      if( !sum ) // all zeros, so no solution for real. TODO: this might cause us to miss future good poses
      {
        cache_result2 = cache_->insert(ik_pose, solution, true);
      }
      else // we will insert our closest approximation TODO: is this useful at all?
      {
        cache_result2 = cache_->insert(ik_pose, solution);
      }
      if( cache_result2 == simple_cache::SUCCESS )
      {
        //ROS_INFO_STREAM_NAMED("grasp","insertion sucessful. cache is now size " << cache_->getSize());
      }
    }
    //ROS_WARN_STREAM_NAMED("grasp","cache in thread has size " << cache_->getSize());
  }
  // DTC
  // --------------------------------------------------------------------------------------------------------

  /*
    ROS_DEBUG_STREAM_NAMED("kdlc","An IK that satisifes the constraints and is collision free could not be found");
    error_code.val = error_code.NO_IK_SOLUTION;
    return false;
  */

  return result;
}

bool KDLCKinematicsPlugin::getPositionFK(const std::vector<std::string> &link_names,
                                         const std::vector<double> &joint_angles,
                                         std::vector<geometry_msgs::Pose> &poses) const
{
  ros::WallTime n1 = ros::WallTime::now();
  if(!active_)
  {
    ROS_ERROR("kinematics not active");
    return false;
  }
  if(poses.size() != link_names.size())
  {
    ROS_ERROR("Poses vector must have size: %zu",link_names.size());
    return false;
  }
  if(joint_angles.size() != dimension_)
  {
    ROS_ERROR("Joint angles vector must have size: %d",dimension_);
    return false;
  }

  KDL::Frame p_out;
  geometry_msgs::PoseStamped pose;
  tf::Stamped<tf::Pose> tf_pose;

  for(unsigned int i=0; i < dimension_; i++)
  {
    jnt_pos_in_(i) = joint_angles[i];
  }

  bool valid = true;
  for(unsigned int i=0; i < poses.size(); i++)
  {
    ROS_DEBUG_STREAM_NAMED("kdlc_kdl","End effector index: " << getKDLSegmentIndex(link_names[i]));
    if(fk_solver_->JntToCart(jnt_pos_in_,p_out,getKDLSegmentIndex(link_names[i])) >=0)
    {
      tf::poseKDLToMsg(p_out,poses[i]);
    }
    else
    {
      ROS_ERROR("Could not compute FK for %s",link_names[i].c_str());
      valid = false;
    }
  }
  return valid;
}

const std::vector<std::string>& KDLCKinematicsPlugin::getJointNames() const
{
  return ik_chain_info_.joint_names;
}

const std::vector<std::string>& KDLCKinematicsPlugin::getLinkNames() const
{
  return ik_chain_info_.link_names;
}

} // namespace
