/*******************************************************************************
 * BSD 3-Clause License
 *
 * Copyright (c) 2021, PickNik Robotics
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *******************************************************************************/

/* Author: Jack Center, Wyatt Rees, Andy Zelenak */

#include <algorithm>
#include <cmath>
#include <Eigen/Geometry>
#include <limits>
#include <moveit/trajectory_processing/ruckig_traj_smoothing.h>
#include <rclcpp/rclcpp.hpp>
#include <vector>

namespace trajectory_processing
{
namespace
{
const rclcpp::Logger LOGGER = rclcpp::get_logger("moveit_trajectory_processing.ruckig_traj_smoothing");
constexpr double DEFAULT_MAX_VELOCITY = 5;           // rad/s
constexpr double DEFAULT_MAX_ACCELERATION = 10;      // rad/s^2
constexpr double DEFAULT_MAX_JERK = 20;              // rad/s^3
constexpr double IDENTICAL_POSITION_EPSILON = 1e-3;  // rad
constexpr double MAX_DURATION_EXTENSION_FACTOR = 5.0;
constexpr double DURATION_EXTENSION_FRACTION = 1.1;
constexpr double MINIMUM_VELOCITY_SEARCH_MAGNITUDE = 0.01;  // rad/s. Stop searching when velocity drops below this
constexpr double DEFAULT_RUCKIG_TIMESTEP = 0.001;           // sec
}  // namespace

bool RuckigSmoothing::applySmoothing(robot_trajectory::RobotTrajectory& trajectory,
                                     const double max_velocity_scaling_factor,
                                     const double max_acceleration_scaling_factor)
{
  const moveit::core::JointModelGroup* group = trajectory.getGroup();
  if (!group)
  {
    RCLCPP_ERROR(LOGGER, "It looks like the planner did not set the group the plan was computed for");
    return false;
  }

  size_t num_waypoints = trajectory.getWayPointCount();
  if (num_waypoints < 2)
  {
    RCLCPP_ERROR(LOGGER, "Trajectory does not have enough points to smooth with Ruckig");
    return false;
  }

  const size_t num_dof = group->getVariableCount();

  // This lib does not actually work properly when angles wrap around, so we need to unwind the path first
  trajectory.unwind();

  // Remove repeated waypoints with no change in position. Ruckig does not handle this well and there's really no
  // need to smooth it. Repeated waypoints cause circular motions.
  for (size_t waypoint_idx = 0; waypoint_idx < num_waypoints - 1; ++waypoint_idx)
  {
    bool identical_waypoint = checkForIdenticalWaypoints(
        *trajectory.getWayPointPtr(waypoint_idx), *trajectory.getWayPointPtr(waypoint_idx + 1), trajectory.getGroup());
    if (identical_waypoint)
    {
      continue;
    }
    else
    {
      trajectory.addSuffixWayPoint(trajectory.getWayPoint(waypoint_idx),
                                   trajectory.getWayPointDurationFromPrevious(waypoint_idx));
    }
  }

  // Trajectory for output.
  // The first waypoint exactly equals the first input waypoint
  robot_trajectory::RobotTrajectory output_trajectory = trajectory;
  output_trajectory.clear();
  output_trajectory.addPrefixWayPoint(trajectory.getWayPoint(0), 0);

  num_waypoints = trajectory.getWayPointCount();
  RCLCPP_ERROR_STREAM(LOGGER, "Requested num_waypoints: " << num_waypoints);
  if (num_waypoints < 2)
  {
    RCLCPP_ERROR(LOGGER, "Trajectory does not have enough points to smooth with Ruckig");
    return false;
  }

  // Instantiate the smoother
  std::unique_ptr<ruckig::Ruckig<0>> ruckig_ptr;
  ruckig_ptr = std::make_unique<ruckig::Ruckig<0>>(num_dof, DEFAULT_RUCKIG_TIMESTEP);
  if (trajectory.getAverageSegmentDuration() < DEFAULT_RUCKIG_TIMESTEP)
  {
    RCLCPP_ERROR(LOGGER, "The default Ruckig timestep is not sufficiently short.");
    return false;
  }
  ruckig::InputParameter<0> ruckig_input{ num_dof };
  ruckig::OutputParameter<0> ruckig_output{ num_dof };

  // Initialize the smoother
  const std::vector<int>& idx = group->getVariableIndexList();
  initializeRuckigState(ruckig_input, ruckig_output, *trajectory.getFirstWayPointPtr(), num_dof, idx);

  // Kinematic limits (vel/accel/jerk)
  const std::vector<std::string>& vars = group->getVariableNames();
  const moveit::core::RobotModel& rmodel = group->getParentModel();
  for (size_t i = 0; i < num_dof; ++i)
  {
    // TODO(andyz): read this from the joint group if/when jerk limits are added to the JointModel
    ruckig_input.max_jerk.at(i) = DEFAULT_MAX_JERK;

    const moveit::core::VariableBounds& bounds = rmodel.getVariableBounds(vars.at(i));

    // This assumes min/max bounds are symmetric
    if (bounds.velocity_bounded_)
    {
      ruckig_input.max_velocity.at(i) = max_velocity_scaling_factor * bounds.max_velocity_;
    }
    else
    {
      ruckig_input.max_velocity.at(i) = max_velocity_scaling_factor * DEFAULT_MAX_VELOCITY;
    }
    if (bounds.acceleration_bounded_)
    {
      ruckig_input.max_acceleration.at(i) = max_acceleration_scaling_factor * bounds.max_acceleration_;
    }
    else
    {
      ruckig_input.max_acceleration.at(i) = max_acceleration_scaling_factor * DEFAULT_MAX_ACCELERATION;
    }
  }

  for (size_t waypoint_idx = 0; waypoint_idx < num_waypoints - 1; ++waypoint_idx)
  {
    RCLCPP_ERROR_STREAM(LOGGER, "Starting next waypoint!");
    moveit::core::RobotStatePtr target_waypoint = trajectory.getWayPointPtr(waypoint_idx + 1);

    double velocity_magnitude = DBL_MAX;
    bool backward_motion_detected = true;
    ruckig::Result ruckig_result = ruckig::Result::Working;

    while (backward_motion_detected || (ruckig_result != ruckig::Result::Finished))
    {
      // Run Ruckig
      ruckig_result = ruckig_ptr->update(ruckig_input, ruckig_output);
      // TODO(andyz): I get a Ruckig result -100: Error in the input parameter
      RCLCPP_WARN_STREAM(LOGGER, "Ruckig code: " << ruckig_result);
      RCLCPP_WARN_STREAM(LOGGER, ruckig_input.to_string());

      // check for backward motion
      backward_motion_detected = checkForLaggingMotion(num_dof, ruckig_input, ruckig_output);

      // decrease target velocity
      if (backward_motion_detected)
      {
        for (size_t joint = 0; joint < num_dof; ++joint)
        {
          ruckig_input.target_velocity.at(joint) *= 0.9;
          // Propogate the change in velocity to acceleration, too.
          // We don't change the position to ensure the exact target position is achieved.
          ruckig_input.target_acceleration.at(joint) =
              (ruckig_input.target_velocity.at(joint) - ruckig_output.new_velocity.at(joint)) / DEFAULT_RUCKIG_TIMESTEP;
        }
        velocity_magnitude = getTargetVelocityMagnitude(ruckig_input, num_dof);
      }

      if (velocity_magnitude < MINIMUM_VELOCITY_SEARCH_MAGNITUDE)
      {
        RCLCPP_ERROR_STREAM(LOGGER, "Could not prevent backward motion");
        return false;
      }

      // Add this waypoint to the output trajectory
      if (!backward_motion_detected &&
          ((ruckig_result == ruckig::Result::Working) || (ruckig_result == ruckig::Result::Finished)))
      {
        moveit::core::RobotState new_waypoint = *target_waypoint;
        for (size_t joint = 0; joint < num_dof; ++joint)
        {
          new_waypoint.setVariablePosition(idx.at(joint), ruckig_output.new_position.at(joint));
          new_waypoint.setVariableVelocity(idx.at(joint), ruckig_output.new_velocity.at(joint));
          new_waypoint.setVariableAcceleration(idx.at(joint), ruckig_output.new_acceleration.at(joint));
        }
        new_waypoint.update();
        RCLCPP_ERROR_STREAM(LOGGER, "Added waypoint! Ruckig code: " << ruckig_result);
        output_trajectory.addSuffixWayPoint(new_waypoint, DEFAULT_RUCKIG_TIMESTEP);
      }

      if (ruckig_result == ruckig::Result::Finished)
      {
        RCLCPP_WARN_STREAM(LOGGER, "Waypoint is Finished, according to Ruckig. Moving to the next one.");
      }

      getNextRuckigInput(ruckig_output, target_waypoint, num_dof, idx, ruckig_input);
    }

    /*
        if (ruckig_result == ruckig::Result::Finished)
        {
          smoothing_complete = true;
        }
        // If ruckig failed, the duration of the seed trajectory likely wasn't long enough.
        // Try duration extension several times.
        // TODO: see issue 767.  (https://github.com/ros-planning/moveit2/issues/767)
        else
        {
          // Take another step towards the same waypoint



          // If Ruckig failed, it's likely because the original seed trajectory did not have a long enough duration when
          // jerk is taken into account. Extend the duration and try again.
          initializeRuckigState(ruckig_input, ruckig_output, *trajectory.getFirstWayPointPtr(), num_dof, idx);
          duration_extension_factor *= DURATION_EXTENSION_FRACTION;
          for (size_t waypoint_idx = 1; waypoint_idx < num_waypoints; ++waypoint_idx)
          {
            trajectory.setWayPointDurationFromPrevious(
                waypoint_idx, duration_extension_factor * trajectory.getWayPointDurationFromPrevious(waypoint_idx));
            // TODO(andyz): re-calculate waypoint velocity and acceleration here?
          }
        }
    */
  }

  // if (ruckig_result != ruckig::Result::Finished)
  // {
  //   RCLCPP_ERROR_STREAM(LOGGER, "Ruckig trajectory smoothing failed. Ruckig error: " << ruckig_result);
  //   return false;
  // }

  trajectory = output_trajectory;
  return true;
}

void RuckigSmoothing::initializeRuckigState(ruckig::InputParameter<0>& ruckig_input,
                                            ruckig::OutputParameter<0>& ruckig_output,
                                            const moveit::core::RobotState& first_waypoint, size_t num_dof,
                                            const std::vector<int>& idx)
{
  std::vector<double> current_positions_vector(num_dof);
  std::vector<double> current_velocities_vector(num_dof);
  std::vector<double> current_accelerations_vector(num_dof);

  for (size_t i = 0; i < num_dof; ++i)
  {
    current_positions_vector.at(i) = first_waypoint.getVariablePosition(idx.at(i));
    current_velocities_vector.at(i) = first_waypoint.getVariableVelocity(idx.at(i));
    current_accelerations_vector.at(i) = first_waypoint.getVariableAcceleration(idx.at(i));
  }
  std::copy_n(current_positions_vector.begin(), num_dof, ruckig_input.current_position.begin());
  std::copy_n(current_velocities_vector.begin(), num_dof, ruckig_input.current_velocity.begin());
  std::copy_n(current_accelerations_vector.begin(), num_dof, ruckig_input.current_acceleration.begin());
  // Initialize output data struct
  ruckig_output.new_position = ruckig_input.current_position;
  ruckig_output.new_velocity = ruckig_input.current_velocity;
  ruckig_output.new_acceleration = ruckig_input.current_acceleration;
}

bool RuckigSmoothing::checkForIdenticalWaypoints(const moveit::core::RobotState& prev_waypoint,
                                                 const moveit::core::RobotState& target_waypoint,
                                                 const moveit::core::JointModelGroup* joint_group)
{
  double magnitude_position_difference = prev_waypoint.distance(target_waypoint, joint_group);

  return (magnitude_position_difference <= IDENTICAL_POSITION_EPSILON);
}

double RuckigSmoothing::getTargetVelocityMagnitude(const ruckig::InputParameter<0>& ruckig_input, size_t num_dof)
{
  double vel_magnitude = 0;
  for (size_t joint = 0; joint < num_dof; ++joint)
  {
    vel_magnitude += ruckig_input.target_velocity.at(joint) * ruckig_input.target_velocity.at(joint);
  }
  return sqrt(vel_magnitude);
}

bool RuckigSmoothing::checkForLaggingMotion(const size_t num_dof, const ruckig::InputParameter<0>& ruckig_input,
                                            const ruckig::OutputParameter<0>& ruckig_output)
{
  // Check for backward motion of any joint
  for (size_t joint = 0; joint < num_dof; ++joint)
  {
    // This indicates the jerk-limited output lags the target output
    if ((ruckig_output.new_velocity.at(joint) / ruckig_input.target_velocity.at(joint)) < 1)
    {
      return true;
    }
  }
  return false;
}

void RuckigSmoothing::getNextRuckigInput(const ruckig::OutputParameter<0>& ruckig_output,
                                         const moveit::core::RobotStatePtr& target_waypoint, size_t num_dof,
                                         const std::vector<int>& idx, ruckig::InputParameter<0>& ruckig_input)
{
  // TODO(andyz): https://github.com/ros-planning/moveit2/issues/766
  // ruckig_output.pass_to_input(ruckig_input);

  for (size_t joint = 0; joint < num_dof; ++joint)
  {
    // Feed output from the previous timestep back as input
    ruckig_input.current_position.at(joint) = ruckig_output.new_position.at(joint);
    ruckig_input.current_velocity.at(joint) = ruckig_output.new_velocity.at(joint);
    ruckig_input.current_acceleration.at(joint) = ruckig_output.new_acceleration.at(joint);

    // Target state is the next waypoint
    ruckig_input.target_position.at(joint) = target_waypoint->getVariablePosition(idx.at(joint));
    ruckig_input.target_velocity.at(joint) = target_waypoint->getVariableVelocity(idx.at(joint));
    ruckig_input.target_acceleration.at(joint) = target_waypoint->getVariableAcceleration(idx.at(joint));
  }
}
}  // namespace trajectory_processing
