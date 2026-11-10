/* rosserial-compatible NodeHandle: the sketch drives initNode, advertise,
 * subscribe and spinOnce as under ros_lib, and the layer registers the
 * topics on the runtime topic table the r2s API speaks. */
#ifndef R2S_ROS_NODE_HANDLE_H
#define R2S_ROS_NODE_HANDLE_H

#include <stdint.h>

#include "r2s/runtime.hpp"

#include "ros2serial/ros/publisher.h"
#include "ros2serial/ros/service_client.h"
#include "ros2serial/ros/service_server.h"
#include "ros2serial/ros/subscriber.h"
#include "ros2serial/ros/time.h"

#ifdef ARDUINO
/// A flash-resident string on AVR, as Arduino declares it.
class __FlashStringHelper;
#endif

namespace ros
{

/// One spinOnce() outcome: the call served the link.
/**
 * Only SPIN_OK occurs yet: the timeout lands with setSpinTimeout (#417)
 * and the stop request with services.
 */
const int SPIN_OK = 0;
/// Reserved spinOnce() outcome for an unspecified failure.
const int SPIN_ERR = -1;
/// One spinOnce() outcome: the call ran past the spin timeout.
const int SPIN_TIMEOUT = -2;
/// Reserved spinOnce() outcome for a service stop request.
const int SPIN_TX_STOP_REQUESTED = -3;
/// Reserved spinOnce() outcome for the time channel.
const int SPIN_TIME_RECV = -4;

/// Stands in for the hardware type a ros_lib sketch names.
/**
 * The layer serves the port the r2s glue binds, so the type is accepted
 * and ignored.
 */
struct ArduinoHardware
{
};

template<class Hardware = ArduinoHardware, int MAX_SUBSCRIBERS = 25, int MAX_PUBLISHERS = 25,
  int INPUT_SIZE = 512, int OUTPUT_SIZE = 512>
/// The node a ported sketch drives.
/**
 * The template parameters mirror ros_lib and are accepted and ignored:
 * every registration runs on the runtime's fixed RAM table either way.
 *
 * \tparam Hardware the port hardware type, accepted and ignored.
 * \tparam MAX_SUBSCRIBERS the subscription limit, accepted and ignored.
 * \tparam MAX_PUBLISHERS the publisher limit, accepted and ignored.
 * \tparam INPUT_SIZE the receive buffer size, accepted and ignored.
 * \tparam OUTPUT_SIZE the send buffer size, accepted and ignored.
 */
class NodeHandle_
{
public:
  /// Owns the session's node.
  /**
   * rosserial sketches name no node, so the announce carries this one.
   */
  NodeHandle_()
  : m_node("rosserial")
  {
  }

  /// Starts the link.
  /**
   * On Arduino this begins Serial at the stack's default baud; elsewhere
   * the port opens out of band, as the runtime firmwares do, so there is
   * nothing to start here.
   */
  void initNode()
  {
#ifdef ARDUINO
    m_node.begin(Serial, 115200UL);
#endif
  }

  /// Announces the publisher's topic.
  /**
   * \param[in,out] p the publisher to announce.
   * \return True with the topic announced, false when the table already
   * closed with the first spin or is full, announcing nothing then.
   */
  bool advertise(Publisher & p)
  {
    uint8_t id = 0xFFu;

    if (m_node.register_publisher(p.topic_, p.m_table, p.m_type_index, p.m_type_name,
        p.m_type_hash,
                                      &id) != R2S_OK)
    {
      return false;
    }
    p.m_topic_id = id;
    p.id_ = (int)id;
    return true;
  }

  /// Announces the subscription.
  /**
   * \tparam MsgT the message type being subscribed.
   * \tparam ObjT the object type holding the callback.
   * \param[in,out] s the subscription to announce.
   * \return True with the topic announced, false under the same refusals
   * as advertise().
   */
  template<typename MsgT, typename ObjT> bool subscribe(Subscriber<MsgT, ObjT> & s)
  {
    uint8_t id = 0xFFu;

    if (m_node.register_subscriber(s.topic_, r2s::msg_traits<MsgT>::type_name(),
                                       r2s::msg_traits<MsgT>::type_hash(), &s,
                                       &Subscriber<MsgT, ObjT>::dispatch, &id) != R2S_OK)
    {
      return false;
    }
    s.id_ = (int)id;
    return true;
  }

  /// Names a service server, which fails to compile with a message.
  /**
   * Services land in X6, so naming one fails to compile instead of
   * linking a half present runtime.
   *
   * \tparam MReq the request message type.
   * \tparam MRes the response message type.
   * \tparam ObjT the object type holding the callback.
   * \param[in] srv the server that cannot be announced.
   * \return False: this line is never reached.
   */
  template<typename MReq, typename MRes, typename ObjT>
  bool advertiseService(ServiceServer<MReq, MRes, ObjT> & srv)
  {
    (void)srv;
    static_assert(!sizeof(MReq *),
        "service servers land in X6: advertiseService cannot announce one");
    return false;
  }

  /// Names a service client, which fails to compile with a message.
  /**
   * Services land in X6, so naming one fails to compile instead of
   * linking a half present runtime.
   *
   * \tparam MReq the request message type.
   * \tparam MRes the response message type.
   * \param[in] srv the client that cannot be named.
   * \return False: this line is never reached.
   */
  template<typename MReq, typename MRes> bool serviceClient(ServiceClient<MReq, MRes> & srv)
  {
    (void)srv;
    static_assert(!sizeof(MReq *), "service clients land in X6: serviceClient cannot name one");
    return false;
  }

  /// Serves the link once.
  /**
   * The first call binds the table and starts the handshake, the later
   * ones move bytes and callbacks.
   *
   * \return SPIN_OK having served, or SPIN_TIMEOUT when the call ran
   * past the spin timeout.
   */
  int spinOnce()
  {
    uint32_t start = m_node.now();

    m_node.spin_some();
    if (m_spin_timeout > 0u && (m_node.now() - start) > m_spin_timeout) {
      return SPIN_TIMEOUT;
    }
    return SPIN_OK;
  }

  /// True while the handshake completed and user traffic may flow.
  /**
   * \return Whether the session is connected.
   */
  bool connected()
  {
    return m_node.connected();
  }

  /// The shared clock.
  /**
   * The host correction's anchor plus the ticks since it, {0, 0} while
   * no correction anchors the clock. Without the time channel it is
   * millis since boot, as an unsynced ros_lib clock is.
   *
   * \return The current time.
   */
  Time now()
  {
    Time t;
#ifdef R2S_WITH_TIME
    r2s_time_t stamp = r2s_now();

    t.sec = (uint32_t)stamp.sec;
    t.nsec = stamp.nanosec;
#else
    uint32_t ms = m_node.now();

    t.sec = ms / 1000u;
    t.nsec = (ms % 1000u) * 1000000u;
#endif
    return t;
  }

  /// Bounds one spinOnce() call in milliseconds.
  /**
   * A call running past it returns SPIN_TIMEOUT. Zero disables the
   * bound, as in ros_lib.
   *
   * \param[in] timeout the bound in milliseconds, or zero to disable.
   */
  void setSpinTimeout(const uint32_t & timeout)
  {
    m_spin_timeout = timeout;
  }

  /// The port handle sketches expect: the node's own hardware instance.
  /**
   * \return The node's hardware instance.
   */
  Hardware * getHardware()
  {
    return &hardware_;
  }

  /// Logs a debug line on the log channel, one frame per call.
  /**
   * Without the log channel it compiles out, so one sketch builds for
   * every tier.
   *
   * \param[in] msg the line to log.
   */
  void logdebug(const char *msg)
  {
#ifdef R2S_WITH_LOG
    r2s_log_debug_ram(msg);
#else
    (void)msg;
#endif
  }

  /// Logs an info line on the log channel, one frame per call.
  /**
   * Without the log channel it compiles out, so one sketch builds for
   * every tier.
   *
   * \param[in] msg the line to log.
   */
  void loginfo(const char *msg)
  {
#ifdef R2S_WITH_LOG
    r2s_log_info_ram(msg);
#else
    (void)msg;
#endif
  }

  /// Logs a warning line on the log channel, one frame per call.
  /**
   * Without the log channel it compiles out, so one sketch builds for
   * every tier.
   *
   * \param[in] msg the line to log.
   */
  void logwarn(const char *msg)
  {
#ifdef R2S_WITH_LOG
    r2s_log_warn_ram(msg);
#else
    (void)msg;
#endif
  }

  /// Logs an error line on the log channel, one frame per call.
  /**
   * Without the log channel it compiles out, so one sketch builds for
   * every tier.
   *
   * \param[in] msg the line to log.
   */
  void logerror(const char *msg)
  {
#ifdef R2S_WITH_LOG
    r2s_log_error_ram(msg);
#else
    (void)msg;
#endif
  }

  /// Logs a fatal line on the log channel, one frame per call.
  /**
   * Without the log channel it compiles out, so one sketch builds for
   * every tier.
   *
   * \param[in] msg the line to log.
   */
  void logfatal(const char *msg)
  {
#ifdef R2S_WITH_LOG
    r2s_log_fatal_ram(msg);
#else
    (void)msg;
#endif
  }

#ifdef ARDUINO
  /// Logs a debug line kept in flash on AVR.
  /**
   * F() strings stay in flash instead of moving to RAM.
   *
   * \param[in] msg the line to log.
   */
  void logdebug(const __FlashStringHelper *msg)
  {
#ifdef R2S_WITH_LOG
    r2s_log_debug(reinterpret_cast<r2s_fstr_t>(msg));
#else
    (void)msg;
#endif
  }

  /// Logs an info line kept in flash on AVR.
  /**
   * F() strings stay in flash instead of moving to RAM.
   *
   * \param[in] msg the line to log.
   */
  void loginfo(const __FlashStringHelper *msg)
  {
#ifdef R2S_WITH_LOG
    r2s_log_info(reinterpret_cast<r2s_fstr_t>(msg));
#else
    (void)msg;
#endif
  }

  /// Logs a warning line kept in flash on AVR.
  /**
   * F() strings stay in flash instead of moving to RAM.
   *
   * \param[in] msg the line to log.
   */
  void logwarn(const __FlashStringHelper *msg)
  {
#ifdef R2S_WITH_LOG
    r2s_log_warn(reinterpret_cast<r2s_fstr_t>(msg));
#else
    (void)msg;
#endif
  }

  /// Logs an error line kept in flash on AVR.
  /**
   * F() strings stay in flash instead of moving to RAM.
   *
   * \param[in] msg the line to log.
   */
  void logerror(const __FlashStringHelper *msg)
  {
#ifdef R2S_WITH_LOG
    r2s_log_error(reinterpret_cast<r2s_fstr_t>(msg));
#else
    (void)msg;
#endif
  }

  /// Logs a fatal line kept in flash on AVR.
  /**
   * F() strings stay in flash instead of moving to RAM.
   *
   * \param[in] msg the line to log.
   */
  void logfatal(const __FlashStringHelper *msg)
  {
#ifdef R2S_WITH_LOG
    r2s_log_fatal(reinterpret_cast<r2s_fstr_t>(msg));
#else
    (void)msg;
#endif
  }
#endif

private:
  NodeHandle_(const NodeHandle_ &);
  NodeHandle_ & operator=(const NodeHandle_ &);

  r2s::Node m_node;
  Hardware hardware_;
  uint32_t m_spin_timeout = 0u;
};

/// The handle a ros_lib sketch names.
typedef NodeHandle_< > NodeHandle;

} // namespace ros

#endif
