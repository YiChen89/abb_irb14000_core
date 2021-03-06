/*
 * Software License Agreement (BSD License)
 *
 * Copyright (c) 2017, Francisco Vina, francisco.vinab@gmail.com
 *               2018, Yoshua Nava, yoshua.nava.chocron@gmail.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *       * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *       * Neither the name of the Southwest Research Institute, nor the names
 *       of its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "yumi_hw/yumi_hw_egm.h"

#include <string>

#include <ros/ros.h>
#include <curl/curl.h>

#include <abb_egm_interface/egm_common.h>
#include <abb_librws/rws_common.h>


using namespace abb::egm_interface;
using namespace abb::rws;


YumiEGMInterface::YumiEGMInterface(const double& exponential_smoothing_alpha)
  :
    has_params_(false),
    rws_connection_ready_(false)
{
  left_arm_feedback_.reset(new proto::Feedback());
  left_arm_status_.reset(new proto::RobotStatus());
  right_arm_feedback_.reset(new proto::Feedback());
  right_arm_status_.reset(new proto::RobotStatus());

  left_arm_joint_vel_targets_.reset(new proto::JointSpace());
  right_arm_joint_vel_targets_.reset(new proto::JointSpace());

  // preallocate memory for feedback/command messages
  initEGMJointSpaceMessage(left_arm_feedback_->mutable_joints());
  initEGMJointSpaceMessage(right_arm_feedback_->mutable_joints());

  initEGMJointStateMessage(left_arm_joint_vel_targets_->mutable_speed(), left_arm_joint_vel_targets_->mutable_external_speed());
  initEGMJointStateMessage(right_arm_joint_vel_targets_->mutable_speed(), right_arm_joint_vel_targets_->mutable_external_speed());

  getParams();
}

YumiEGMInterface::~YumiEGMInterface()
{

}

void YumiEGMInterface::getParams()
{
  ros::NodeHandle nh("~");

  // RWS parameters
  nh.param("rws/delay_time", rws_delay_time_, 1.0);
  nh.param("rws/max_signal_retries", rws_max_signal_retries_, 5);

  // EGM parameters
  double timeout;
  nh.param("egm/comm_timeout", timeout, 30.0);
  egm_params_.comm_timeout = timeout;

  std::string tool_name;
  nh.param("egm/tool_name", tool_name, std::string("tool0"));
  egm_params_.tool_name = tool_name;

  std::string wobj_name;
  nh.param("egm/wobj_name", wobj_name, std::string("wobj0"));
  egm_params_.wobj_name = wobj_name;

  double cond_min_max;
  nh.param("egm/cond_min_max", cond_min_max, 0.5);
  egm_params_.cond_min_max = cond_min_max;

  double lp_filter;
  nh.param("egm/lp_filter", lp_filter, 0.0);
  egm_params_.lp_filter = lp_filter;

  double max_speed_deviation;
  nh.param("egm/max_speed_deviation", max_speed_deviation, 400.0);
  egm_params_.max_speed_deviation = max_speed_deviation;

  double condition_time;
  nh.param("egm/condition_time", condition_time, 10.0);
  egm_params_.cond_time = condition_time;

  double ramp_in_time;
  nh.param("egm/ramp_in_time", ramp_in_time, 0.1);
  egm_params_.ramp_in_time = ramp_in_time;

  double pos_corr_gain;
  nh.param("egm/pos_corr_gain", pos_corr_gain, 0.0);
  egm_params_.pos_corr_gain = pos_corr_gain;

  has_params_ = true;
}

bool YumiEGMInterface::init(const std::string& ip, const unsigned short& port)
{
  if (!has_params_)
  {
    ROS_ERROR_STREAM(ros::this_node::getName() << ": missing EGM/RWS parameters.");
    return false;
  }

  rws_ip_ = ip;
  rws_port_ = port;

  if(!initRWS())
  {
    return false;
  }

  if (!initEGM())
  {
    return false;
  }

  if(!startEGM())
  {
    return false;
  }

  return true;
}

bool YumiEGMInterface::stop()
{
  if(!stopEGM())
  {
    return false;
  }

  io_service_.stop();
  io_service_threads_.join_all();

  return true;
}

void YumiEGMInterface::getCurrentJointStates(float (&joint_pos)[N_YUMI_JOINTS],
                                             float (&joint_vel)[N_YUMI_JOINTS],
                                             float (&joint_acc)[N_YUMI_JOINTS])
{
  left_arm_egm_interface_->wait_for_data();
  left_arm_egm_interface_->read(left_arm_feedback_.get(), left_arm_status_.get());

  right_arm_egm_interface_->wait_for_data();
  right_arm_egm_interface_->read(right_arm_feedback_.get(), right_arm_status_.get());

  copyEGMJointSpaceToArray(left_arm_feedback_->joints(), joint_pos, joint_vel, joint_acc);
  copyEGMJointSpaceToArray(right_arm_feedback_->joints(), &joint_pos[7], &joint_vel[7], &joint_acc[7]);
}

void YumiEGMInterface::setJointVelTargets(float (&joint_vel_targets)[N_YUMI_JOINTS])
{
  copyArrayToEGMJointState(joint_vel_targets, left_arm_joint_vel_targets_->mutable_speed(), left_arm_joint_vel_targets_->mutable_external_speed());
  copyArrayToEGMJointState(&joint_vel_targets[7], right_arm_joint_vel_targets_->mutable_speed(), right_arm_joint_vel_targets_->mutable_external_speed());

  left_arm_egm_interface_->write(*left_arm_joint_vel_targets_);
  right_arm_egm_interface_->write(*right_arm_joint_vel_targets_);
}

void YumiEGMInterface::initEGMJointSpaceMessage(proto::JointSpace *joint_space_message)
{
  initEGMJointStateMessage(joint_space_message->mutable_position(), joint_space_message->mutable_external_position());
  initEGMJointStateMessage(joint_space_message->mutable_speed(), joint_space_message->mutable_external_speed());
  initEGMJointStateMessage(joint_space_message->mutable_acceleration(), joint_space_message->mutable_external_acceleration());
}

void YumiEGMInterface::initEGMJointStateMessage(google::protobuf::RepeatedField<double> *joint_states,
                                                google::protobuf::RepeatedField<double> *external_joint_states)
{
  joint_states->Clear();
  for (unsigned int i = 0; i < 6; ++i)
  {
    joint_states->Add(0.0);
  }

  external_joint_states->Clear();
  external_joint_states->Add(0.0);
}

void YumiEGMInterface::copyEGMJointStateToArray(const google::protobuf::RepeatedField<double> &joint_states,
                                                const google::protobuf::RepeatedField<double> &external_joint_states,
                                                float* joint_array) const
{
  joint_array[0] = (float)joint_states.Get(0)*M_PI/180.0;
  joint_array[1] = (float)joint_states.Get(1)*M_PI/180.0;
  joint_array[2] = (float)joint_states.Get(3)*M_PI/180.0;
  joint_array[3] = (float)joint_states.Get(4)*M_PI/180.0;
  joint_array[4] = (float)joint_states.Get(5)*M_PI/180.0;
  joint_array[5] = (float)external_joint_states.Get(0)*M_PI/180.0;
  joint_array[6] = (float)joint_states.Get(2)*M_PI/180.0;
}

void YumiEGMInterface::copyEGMJointSpaceToArray(const proto::JointSpace &joint_space,
                                                float* joint_pos, float* joint_vel, float* joint_acc ) const
{
  copyEGMJointStateToArray(joint_space.position(), joint_space.external_position(), joint_pos);
  copyEGMJointStateToArray(joint_space.speed(), joint_space.external_speed(), joint_vel);
  copyEGMJointStateToArray(joint_space.acceleration(), joint_space.external_acceleration(), joint_acc);
}


void YumiEGMInterface::copyArrayToEGMJointState(const float* joint_array,
                                                google::protobuf::RepeatedField<double>* joint_states,
                                                google::protobuf::RepeatedField<double>* external_joint_states) const
{
  joint_states->Set(0, (double) joint_array[0] * 180.0/M_PI);
  joint_states->Set(1, (double) joint_array[1] * 180.0/M_PI);
  joint_states->Set(2, (double) joint_array[6] * 180.0/M_PI);
  joint_states->Set(3, (double) joint_array[2] * 180.0/M_PI);
  joint_states->Set(4, (double) joint_array[3] * 180.0/M_PI);
  joint_states->Set(5, (double) joint_array[4] * 180.0/M_PI);
  external_joint_states->Set(0, (double) joint_array[5] * 180.0/M_PI);
}

bool YumiEGMInterface::initRWS()
{
  ROS_INFO_STREAM(ros::this_node::getName() << " starting RWS connection with IP & PORT: " << rws_ip_ << " / " << rws_port_);

  // rws_interface_.reset(new RWSInterfaceYuMi(rws_ip_, rws_port_));
  rws_interface_.reset(new RWSSimpleStateMachineInterface(rws_ip_, rws_port_));
  ros::Duration(rws_delay_time_).sleep();
    std::cout << "  RWS interface created" << std::endl;

  // Check that RAPID is running on the robot and that robot is in AUTO mode
  if(!rws_interface_->isRAPIDRunning())
  {
    ROS_ERROR_STREAM(ros::this_node::getName() << ": robot unavailable, make sure that the RAPID program is running on the flexpendant.");
    return false;
  }
    std::cout << "  RAPID running" << std::endl;
  ros::Duration(rws_delay_time_).sleep();

  if(!rws_interface_->isAutoMode())
  {
    ROS_ERROR_STREAM(ros::this_node::getName() << ": robot unavailable, make sure to set the robot to AUTO mode on the flexpendant.");
    return false;
  }
    std::cout << "  Auto mode" << std::endl;
  ros::Duration(rws_delay_time_).sleep();

  if(!sendEGMParams())
  {
    return false;
  }

  rws_connection_ready_ = true;
    std::cout << "Connection ready" << std::endl;
  ros::Duration(rws_delay_time_).sleep();

  if(!startEGM()) return false;

  // ros::NodeHandle nh;
  //rws_watchdog_timer_ = nh.createTimer(ros::Duration(rws_watchdog_period_), &YumiEGMInterface::rwsWatchdog, this);

  return true;
}

bool YumiEGMInterface::initEGM()
{
  left_arm_egm_interface_.reset(new EGMInterfaceDefault(io_service_, egm_common_values::communication::DEFAULT_PORT_NUMBER));
  right_arm_egm_interface_.reset(new EGMInterfaceDefault(io_service_, egm_common_values::communication::DEFAULT_PORT_NUMBER + 1));
  configureEGM(left_arm_egm_interface_);
  configureEGM(right_arm_egm_interface_);

  // create threads for EGM communication
  for(size_t i = 0; i < MAX_NUMBER_OF_EGM_CONNECTIONS; i++)
  {
    io_service_threads_.create_thread(boost::bind(&boost::asio::io_service::run, &io_service_));
  }

  return true;
}


bool YumiEGMInterface::sendEGMParams()
{
  EGMData left_arm_egm_params, right_arm_egm_params;
  bool right_arm_success = rws_interface_->getRAPIDSymbolData(SystemConstants::RAPID::TASK_ROB_L,
                                                              RWSSimpleStateMachineInterface::ProgramConstants::RAPID::Symbols::RAPID_EGM_DATA,
                                                              &left_arm_egm_params);
  bool left_arm_success = rws_interface_->getRAPIDSymbolData(SystemConstants::RAPID::TASK_ROB_R,
                                                             RWSSimpleStateMachineInterface::ProgramConstants::RAPID::Symbols::RAPID_EGM_DATA,
                                                             &right_arm_egm_params);
    std::cout << "  Left arm EGM Parameters" << std::endl;
    std::cout << "    success? " << right_arm_success << std::endl;
    std::cout << "    egm_data: " << left_arm_egm_params.constructRWSValueString() << std::endl;
    std::cout << "  Right arm EGM Parameters" << std::endl;
    std::cout << "    success? " << left_arm_success << std::endl;
    std::cout << "    egm_data: " << right_arm_egm_params.constructRWSValueString() << std::endl;

  if(!left_arm_success || !right_arm_success)
  {
    ROS_ERROR_STREAM(ros::this_node::getName() << ": robot unavailable, make sure to set the robot to AUTO mode on the flexpendant.");
    return false;
  }

  setEGMParams(&left_arm_egm_params);
  setEGMParams(&right_arm_egm_params);

  left_arm_success = rws_interface_->setRAPIDSymbolData(SystemConstants::RAPID::TASK_ROB_L,
                                                        RWSSimpleStateMachineInterface::ProgramConstants::RAPID::Symbols::RAPID_EGM_DATA,
                                                        left_arm_egm_params);
  right_arm_success = rws_interface_->setRAPIDSymbolData(SystemConstants::RAPID::TASK_ROB_R,
                                                         RWSSimpleStateMachineInterface::ProgramConstants::RAPID::Symbols::RAPID_EGM_DATA,
                                                         right_arm_egm_params);

  if(left_arm_success && right_arm_success)
  {
    ROS_INFO("EGM parameters correctly set.");
    return true;
  }

  return false;
}

void YumiEGMInterface::setEGMParams(EGMData* egm_data)
{
  egm_data->comm_timeout = egm_params_.comm_timeout;
  egm_data->tool_name = egm_params_.tool_name;
  egm_data->wobj_name = egm_params_.wobj_name;
  egm_data->cond_min_max = egm_params_.cond_min_max;
  egm_data->lp_filter = egm_params_.lp_filter;
  egm_data->max_speed_deviation = egm_params_.max_speed_deviation;
  egm_data->cond_time = egm_params_.cond_time;
  egm_data->ramp_in_time = egm_params_.ramp_in_time;
  egm_data->pos_corr_gain = egm_params_.pos_corr_gain;
}

void YumiEGMInterface::configureEGM(boost::shared_ptr<EGMInterfaceDefault> egm_interface)
{
  EGMInterfaceConfiguration configuration = egm_interface->getConfiguration();

  configuration.basic.use_conditions = false;
  configuration.basic.axes = EGMInterfaceConfiguration::Seven;
  configuration.basic.execution_mode = EGMInterfaceConfiguration::Direct;
  configuration.communication.use_speed = true;
  configuration.logging.use_logging = true;

  egm_interface->setConfiguration(configuration);
}

bool YumiEGMInterface::startEGM()
{
  bool egm_started = false;

  if(rws_interface_ && rws_connection_ready_)
  {
    for(int i = 0; i < rws_max_signal_retries_ && !egm_started; ++i)
    {
      egm_started = rws_interface_->signalEGMStartJoint();
      
      if(!egm_started)
      {
        ROS_ERROR_STREAM(ros::this_node::getName() << ": failed to send EGM start signal! [Attempt " << 
                         i+1 << "/" << rws_max_signal_retries_<< "]");
      }
    }
  }

  return egm_started;
}

bool YumiEGMInterface::stopEGM()
{
  bool egm_stopped = false;

  if(rws_interface_ && rws_connection_ready_)
  {
    for(int i = 0; i < rws_max_signal_retries_ && !egm_stopped; ++i)
    {
      egm_stopped = rws_interface_->signalEGMStop();
      if(!egm_stopped)
      {
        ROS_ERROR_STREAM(ros::this_node::getName() << ": failed to send EGM stop signal! [Attempt " << 
                          i+1 << "/" << rws_max_signal_retries_<< "]");
      }
    }
  }

  return egm_stopped;
}

//void YumiEGMInterface::rwsWatchdog(const ros::TimerEvent &e)
//{
//    bool rc_auto = false;
//    bool rc_rapid_running = true;

//    if(rws_interface_)
//    {
//        rc_auto = rws_interface_->isModeAuto();
//        ros::Duration(rws_delay_time_).sleep();
//        rc_rapid_running = rws_interface_->isRAPIDRunning();

//        if(rc_auto && rc_rapid_running)
//        {
//            //if(!rws_connection_ready_)
//            //{
//                if (sendEGMParams()) rws_connection_ready_ = true;
//            //}
//        }
//        else
//        {
//            ROS_WARN("Robot controller is unavailable (it needs to be in auto mode and also have RAPID started)");
//            rws_connection_ready_ = false;
//        }
//    }
//}


YumiHWEGM::YumiHWEGM(const double& exponential_smoothing_alpha)
  : 
    YumiHW(exponential_smoothing_alpha), 
    is_initialized_(false)
{ }

YumiHWEGM::~YumiHWEGM()
{
  yumi_egm_interface_.stop();
}

void YumiHWEGM::setup(const std::string& ip, const std::string& port)
{
  ip_ = ip;
  port_ = atoi(port.c_str());
}

bool YumiHWEGM::init()
{
  if(is_initialized_)
  {
    return false;
  }
  current_strategy_ = JOINT_VELOCITY;

  bool success = yumi_egm_interface_.init(ip_, port_);

  if(!success)
  {
    return false;
  }

  is_initialized_ = true;
  return true;
}


void YumiHWEGM::read(ros::Time time, ros::Duration period)
{
  if(!is_initialized_)
  {
    return;
  }

  data_buffer_mutex_.lock();

  yumi_egm_interface_.getCurrentJointStates(joint_pos_, joint_vel_, joint_acc_);

  for (int j = 0; j < n_joints_; j++)
  {
    joint_position_prev_[j] = joint_position_[j];
    joint_position_[j] = joint_pos_[j];

    // Estimation of joint velocity via finite differences method of first order
    // joint_velocity_[j] = (joint_position_[j] - joint_position_prev_[j]) / period.toSec();

    // Estimation of joint velocity via finite differences method of first order and exponential smoothing
    joint_velocity_[j] = filters::exponentialSmoothing((joint_position_[j] - joint_position_prev_[j]) / period.toSec(),
                                                       joint_velocity_[j], exponential_smoothing_alpha_);
  }

  data_buffer_mutex_.unlock();
}


void YumiHWEGM::write(ros::Time time, ros::Duration period)
{
  if(!is_initialized_)
  {
    return;
  }

  enforceLimits(period);

  data_buffer_mutex_.lock();

  for (int j = 0; j < n_joints_; j++)
  {
    joint_vel_targets_[j] = joint_velocity_command_[j];
  }

  yumi_egm_interface_.setJointVelTargets(joint_vel_targets_);

  data_buffer_mutex_.unlock();
}
