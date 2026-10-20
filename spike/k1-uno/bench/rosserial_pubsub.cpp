// rosserial reference: Float32 publisher + Twist subscriber, the K1 workload.
// ROSSERIAL_TUNED selects the smallest NodeHandle that still negotiates
// (a TopicInfo for geometry_msgs/Twist needs about 90 bytes of TX buffer).
#include <Arduino.h>
#include <ros.h>
#include <std_msgs/Float32.h>
#include <geometry_msgs/Twist.h>

#ifdef ROSSERIAL_TUNED
ros::NodeHandle_<ArduinoHardware, 1, 1, 64, 128> nh;
#else
ros::NodeHandle nh;
#endif
std_msgs::Float32 battery;
ros::Publisher battery_pub("battery", &battery);
volatile float out_lx, out_az;
void on_cmd_vel(const geometry_msgs::Twist &m) { out_lx = m.linear.x; out_az = m.angular.z; }
ros::Subscriber<geometry_msgs::Twist> cmd_vel_sub("cmd_vel", &on_cmd_vel);

void setup() { nh.initNode(); nh.advertise(battery_pub); nh.subscribe(cmd_vel_sub); }
void loop() {
  battery.data = analogRead(A0) * 0.01f;
  battery_pub.publish(&battery);
  nh.spinOnce();
}
