// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* rosserial-compatible Publisher: the sketch hands it a topic and its message,
 * and advertise() announces it on the runtime topic table. The shape follows
 * ros_lib, so sketches port by changing the include. */
#ifndef R2S_ROS_PUBLISHER_H
#define R2S_ROS_PUBLISHER_H

#include <stdint.h>

#include "r2s/runtime.hpp"

namespace ros
{
template<class Hardware, int MAX_SUBSCRIBERS, int MAX_PUBLISHERS, int INPUT_SIZE,
  int OUTPUT_SIZE>
/// The size parameters of NodeHandle_, and the hardware type it names.
/**
 * \tparam Hardware the port hardware type the handle talks through.
 * \tparam MAX_SUBSCRIBERS how many subscriptions the handle holds.
 * \tparam MAX_PUBLISHERS how many publishers the handle holds.
 * \tparam INPUT_SIZE the receive buffer size in bytes.
 * \tparam OUTPUT_SIZE the send buffer size in bytes.
 */
class NodeHandle_;

/// Publishes one message type on one topic.
/**
 * The constructor only keeps the topic and the codec of the message it
 * points at; advertise() announces them, and publish() sends through
 * the announced id.
 */
class Publisher
{
public:
  /// Keeps the topic and the message's codec.
  /**
   * The endpoint rides along for sketches written against ros_lib and
   * changes nothing announced; the default is ros_lib's publisher
   * endpoint.
   *
   * \tparam MsgT the message type this publisher sends.
   * \param[in] topic what to announce.
   * \param[in] msg the message whose codec publish() sends.
   * \param[in] endpoint the ros_lib endpoint, accepted and ignored.
   */
  template<typename MsgT>
  Publisher(const char *topic, MsgT * msg, int endpoint = 0)
  : topic_(topic), msg_(msg), id_(-1), m_endpoint(endpoint),
    m_table(r2s::msg_traits<MsgT>::table()),
    m_type_index(r2s::msg_traits<MsgT>::type_index()),
    m_type_name(r2s::msg_traits<MsgT>::type_name()),
    m_type_hash(r2s::msg_traits<MsgT>::type_hash()), m_topic_id(0xFFu)
  {
  }

  /// Sends the message.
  /**
   * \tparam MsgT the message type being sent.
   * \param[in] msg the message to send.
   * \return 0 when the frame went out, and -1 when nothing was sent:
   * unannounced topic, silent link, refused topic, or a message that
   * does not fit.
   */
  template<typename MsgT> int publish(const MsgT *msg)
  {
    if (m_topic_id == 0xFFu) {
      return -1;
    }
    return r2s::detail::publish(m_topic_id, msg) == R2S_OK ? 0 : -1;
  }

  /// The ros_lib endpoint this publisher was built with.
  /**
   * \return The endpoint value from the constructor.
   */
  int getEndpointType()
  {
    return m_endpoint;
  }

  const char *topic_;
  const void *msg_;
    /* Set by NodeHandle when it advertises; -1 until then. */
  int id_;

private:
  template<class Hardware, int MAX_SUBSCRIBERS, int MAX_PUBLISHERS, int INPUT_SIZE,
    int OUTPUT_SIZE>
  friend class NodeHandle_;

  int m_endpoint;
  const r2s_codec_table_t *m_table;
  uint16_t m_type_index;
  const char *m_type_name;
  const uint8_t *m_type_hash;
  uint8_t m_topic_id;
};

} // namespace ros

#endif
