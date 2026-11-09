/* rosserial spelling of the service client: the declaration a ported
 * sketch names, until X6 gives it a runtime. serviceClient refuses it. */
#ifndef R2S_ROS2SERIAL_ROS_SERVICE_CLIENT_H
#define R2S_ROS2SERIAL_ROS_SERVICE_CLIENT_H

namespace ros
{

template<typename MReq, typename MRes>
class ServiceClient
{
public:
  ServiceClient(const char *service_name)
  : topic_(service_name)
  {
  }

  const char *topic_;

private:
  ServiceClient(const ServiceClient &);
  ServiceClient & operator=(const ServiceClient &);
};

} // namespace ros

#endif
