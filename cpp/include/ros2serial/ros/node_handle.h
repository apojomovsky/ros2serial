/* rosserial-compatible NodeHandle: the sketch drives initNode, advertise,
 * subscribe and spinOnce as under ros_lib, and the layer registers the
 * topics on the runtime topic table the r2s API speaks. */
#ifndef R2S_ROS_NODE_HANDLE_H
#define R2S_ROS_NODE_HANDLE_H

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "r2s/r2s_params.h"
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

  /// Announces the service server.
  /**
   * The server answers each request through its callback while the node
   * spins. Refused past the first spin, like a topic.
   *
   * \tparam MReq the request message type.
   * \tparam MRes the response message type.
   * \tparam ObjT the object type holding the callback.
   * \param[in,out] srv the server to announce.
   * \return True with the service announced, false on refusal.
   */
  template<typename MReq, typename MRes, typename ObjT>
  bool advertiseService(ServiceServer<MReq, MRes, ObjT> & srv)
  {
    if (m_spun) {
      return false;
    }
    return srv.attach(m_node);
  }

  /// Names the service client.
  /**
   * The client calls the service through call(), which spins the node
   * until its answer. Refused past the first spin, like a topic.
   *
   * \tparam MReq the request message type.
   * \tparam MRes the response message type.
   * \param[in,out] srv the client to name.
   * \return True with the call announced, false on refusal.
   */
  template<typename MReq, typename MRes> bool serviceClient(ServiceClient<MReq, MRes> & srv)
  {
    if (m_spun) {
      return false;
    }
    return srv.attach(m_node);
  }

  /// Reads an integer parameter the sketch declared.
  /**
   * The value is the live one the last applied set left, so it reflects
   * every set the node already spun past. Unlike rosserial, nothing
   * crosses the wire: the MCU owns its parameters and the host sets them
   * through the bridge. The timeout is accepted and ignored.
   *
   * \param[in] name the declared parameter to read.
   * \param[out] param where the value lands.
   * \param[in] length how many values to read, always 1.
   * \param[in] timeout accepted and ignored.
   * \return True with the value read, false on an unknown name, a type
   * mismatch, a length other than 1, or a value outside int range.
   */
  bool getParam(const char *name, int *param, int length = 1, int timeout = 1000)
  {
    (void)timeout;
#ifdef R2S_WITH_PARAMS
    const r2s_param_t *entry = find_param(name);

    if (param == NULL || length != 1 || entry == NULL || entry->type != R2S_PARAM_I64) {
      return false;
    }
    if (entry->value.i < (int64_t)INT_MIN || entry->value.i > (int64_t)INT_MAX) {
      return false;
    }
    *param = (int)entry->value.i;
    return true;
#else
    (void)name;
    (void)param;
    (void)length;
    return false;
#endif
  }

  /// Reads a float parameter the sketch declared.
  /**
   * The value is the live one the last applied set left. The timeout is
   * accepted and ignored, as above.
   *
   * \param[in] name the declared parameter to read.
   * \param[out] param where the value lands.
   * \param[in] length how many values to read, always 1.
   * \param[in] timeout accepted and ignored.
   * \return True with the value read, false on an unknown name, a type
   * mismatch, or a length other than 1.
   */
  bool getParam(const char *name, float *param, int length = 1, int timeout = 1000)
  {
    (void)timeout;
#ifdef R2S_WITH_PARAMS
    const r2s_param_t *entry = find_param(name);

    if (param == NULL || length != 1 || entry == NULL || entry->type != R2S_PARAM_F64) {
      return false;
    }
    *param = (float)entry->value.f;
    return true;
#else
    (void)name;
    (void)param;
    (void)length;
    return false;
#endif
  }

  /// Reads a string parameter the sketch declared.
  /**
   * Copies the live text into the caller's buffer, which holds the bound
   * plus the terminator, as ros_lib does. The timeout is accepted and
   * ignored, as above.
   *
   * \param[in] name the declared parameter to read.
   * \param[out] param where the text lands, one buffer per value.
   * \param[in] length how many values to read, always 1.
   * \param[in] timeout accepted and ignored.
   * \return True with the text copied, false on an unknown name, a type
   * mismatch, or a length other than 1.
   */
  bool getParam(const char *name, char **param, int length = 1, int timeout = 1000)
  {
    (void)timeout;
#ifdef R2S_WITH_PARAMS
    const r2s_param_t *entry = find_param(name);

    if (param == NULL || length != 1 || entry == NULL || entry->type != R2S_PARAM_STRING) {
      return false;
    }
    (void)strcpy(param[0], entry->value.s);
    return true;
#else
    (void)name;
    (void)param;
    (void)length;
    return false;
#endif
  }

  /// Reads a bool parameter the sketch declared.
  /**
   * The value is the live one the last applied set left. The timeout is
   * accepted and ignored, as above.
   *
   * \param[in] name the declared parameter to read.
   * \param[out] param where the value lands.
   * \param[in] length how many values to read, always 1.
   * \param[in] timeout accepted and ignored.
   * \return True with the value read, false on an unknown name, a type
   * mismatch, or a length other than 1.
   */
  bool getParam(const char *name, bool *param, int length = 1, int timeout = 1000)
  {
    (void)timeout;
#ifdef R2S_WITH_PARAMS
    const r2s_param_t *entry = find_param(name);

    if (param == NULL || length != 1 || entry == NULL || entry->type != R2S_PARAM_BOOL) {
      return false;
    }
    *param = entry->value.b != 0u;
    return true;
#else
    (void)name;
    (void)param;
    (void)length;
    return false;
#endif
  }

  /// Rejects a parameter type with no equivalent.
  /**
   * The declared parameters are bool, 64-bit int, 64-bit float and
   * string, so anything else fails to compile naming the rule instead of
   * reading a wrong value.
   *
   * \tparam T the unsupported value type.
   * \param[in] name the parameter that cannot be read.
   * \param[out] param where nothing lands.
   * \param[in] length how many values to read, always 1.
   * \param[in] timeout accepted and ignored.
   * \return False: this line is never reached.
   */
  template<typename T>
  bool getParam(const char *name, T *param, int length = 1, int timeout = 1000)
  {
    (void)name;
    (void)param;
    (void)length;
    (void)timeout;
    static_assert(
      !sizeof(T *),
      "getParam reads bool, int, float and char* parameters: arrays and other types have no equivalent");
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

    m_spun = true;
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

#ifdef R2S_WITH_PARAMS
  /// Finds a declared parameter by name.
  /**
   * \param[in] name the parameter to find.
   * \return The entry, or NULL on an unknown or unnamed table.
   */
  static const r2s_param_t * find_param(const char *name)
  {
    uint8_t i;

    if (name == NULL) {
      return NULL;
    }
    for (i = 0u; i < r2s_params_count(); i++) {
      const r2s_param_t *entry = r2s_params_get(i);

      if (entry != NULL && entry->name != NULL && strcmp(entry->name, name) == 0) {
        return entry;
      }
    }
    return NULL;
  }
#endif

  r2s::Node m_node;
  Hardware hardware_;
  uint32_t m_spin_timeout = 0u;
  bool m_spun = false;
};

/// The handle a ros_lib sketch names.
typedef NodeHandle_< > NodeHandle;

} // namespace ros

#endif
