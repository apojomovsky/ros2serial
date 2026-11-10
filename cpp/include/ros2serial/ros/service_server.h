// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* rosserial spelling of the service server: the declaration a ported
 * sketch names, until X6 gives it a runtime. advertiseService refuses it. */
#ifndef R2S_ROS2SERIAL_ROS_SERVICE_SERVER_H
#define R2S_ROS2SERIAL_ROS_SERVICE_SERVER_H

namespace ros
{

template<typename MReq, typename MRes, typename ObjT = void>
/// The declaration a ported sketch names.
/**
 * Until X6 gives it a runtime, advertiseService refuses it.
 *
 * \tparam MReq the request message type.
 * \tparam MRes the response message type.
 * \tparam ObjT the object type holding the callback.
 */
class ServiceServer
{
public:
  /// The callback shape: a member function answering the request.
  typedef void (ObjT::*CallbackT)(const MReq &, MRes &);

  /// Keeps the service name and the callback for the X6 runtime.
  /**
   * \param[in] topic_name the service to serve.
   * \param[in] callback the member function to answer with.
   * \param[in] obj the object to call it on.
   */
  ServiceServer(const char *topic_name, CallbackT callback, ObjT *obj)
  : topic_(topic_name), callback_(callback), obj_(obj)
  {
  }

  const char *topic_;

private:
  CallbackT callback_;
  ObjT *obj_;

  ServiceServer(const ServiceServer &);
  ServiceServer & operator=(const ServiceServer &);
};

template<typename MReq, typename MRes>
/// The declaration a ported sketch names.
/**
 * The standalone-function spelling: a capture-less lambda converts to
 * the function pointer the constructor takes. Until X6 gives it a
 * runtime, advertiseService refuses it.
 *
 * \tparam MReq the request message type.
 * \tparam MRes the response message type.
 */
class ServiceServer<MReq, MRes, void>
{
public:
  /// The callback shape: a free function answering the request.
  typedef void (*CallbackT)(const MReq &, MRes &);

  /// Keeps the service name and the callback for the X6 runtime.
  /**
   * \param[in] topic_name the service to serve.
   * \param[in] callback the free function to answer with.
   */
  ServiceServer(const char *topic_name, CallbackT callback)
  : topic_(topic_name), callback_(callback)
  {
  }

  const char *topic_;

private:
  CallbackT callback_;

  ServiceServer(const ServiceServer &);
  ServiceServer & operator=(const ServiceServer &);
};

} // namespace ros

#endif
