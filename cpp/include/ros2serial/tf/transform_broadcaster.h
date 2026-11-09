/* rosserial-compatible tf broadcaster: init and sendTransform as in
 * ros_lib, over the compat Publisher, so a ported sketch keeps its shape. */
#ifndef R2S_ROS2SERIAL_TF_TRANSFORM_BROADCASTER_H
#define R2S_ROS2SERIAL_TF_TRANSFORM_BROADCASTER_H

#include "ros2serial/ros/node_handle.h"
#include "ros2serial/tf/tfMessage.h"

namespace tf
{

class TransformBroadcaster
{
public:
  TransformBroadcaster()
  : publisher_("/tf", &internal_msg_)
  {
  }

    /* Announces /tf carrying TFMessage, as in ros_lib. */
  void init(ros::NodeHandle & nh)
  {
    nh.advertise(publisher_);
  }

    /* Publishes the one stamped transform on /tf, as in ros_lib. */
  void sendTransform(geometry_msgs::TransformStamped & transform)
  {
    internal_msg_.transforms.size = 1u;
    internal_msg_.transforms.data[0] = transform;
    publisher_.publish(&internal_msg_);
  }

private:
  tf::tfMessage internal_msg_;
  ros::Publisher publisher_;

  TransformBroadcaster(const TransformBroadcaster &);
  TransformBroadcaster & operator=(const TransformBroadcaster &);
};

} // namespace tf

#endif
