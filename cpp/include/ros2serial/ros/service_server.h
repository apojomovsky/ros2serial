/* rosserial spelling of the service server: the declaration a ported
 * sketch names, until X6 gives it a runtime. advertiseService refuses it. */
#ifndef R2S_ROS2SERIAL_ROS_SERVICE_SERVER_H
#define R2S_ROS2SERIAL_ROS_SERVICE_SERVER_H

namespace ros
{

template <typename MReq, typename MRes, typename ObjT = void> class ServiceServer
{
  public:
    typedef void (ObjT::*CallbackT)(const MReq &, MRes &);

    ServiceServer(const char *topic_name, CallbackT callback, ObjT *obj)
        : topic_(topic_name), callback_(callback), obj_(obj)
    {
    }

    const char *topic_;

  private:
    CallbackT callback_;
    ObjT *obj_;

    ServiceServer(const ServiceServer &);
    ServiceServer &operator=(const ServiceServer &);
};

template <typename MReq, typename MRes> class ServiceServer<MReq, MRes, void>
{
  public:
    typedef void (*CallbackT)(const MReq &, MRes &);

    ServiceServer(const char *topic_name, CallbackT callback) : topic_(topic_name), callback_(callback)
    {
    }

    const char *topic_;

  private:
    CallbackT callback_;

    ServiceServer(const ServiceServer &);
    ServiceServer &operator=(const ServiceServer &);
};

} // namespace ros

#endif
