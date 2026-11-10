// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* rosserial spelling of the service client: the declaration a ported
 * sketch names, calling over X6. serviceClient announces it on the core
 * service table, and call() sends one request and spins until its answer.
 */
#ifndef R2S_ROS2SERIAL_ROS_SERVICE_CLIENT_H
#define R2S_ROS2SERIAL_ROS_SERVICE_CLIENT_H

#include <stdint.h>

#include "r2s/r2s_codec.h"
#include "r2s/r2s_services.h"
#include "r2s/runtime.hpp"
#ifdef R2S_WITH_SERVICES
#include "r2s/service.hpp"
#endif

namespace ros
{

template<typename MReq, typename MRes>
/// The declaration a ported sketch names.
/**
 * serviceClient announces it on the core service table; call() sends one
 * request and spins until its answer, a refusal, the timeout or the
 * disconnect.
 *
 * \tparam MReq the request message type this client sends.
 * \tparam MRes the response message type this client waits for.
 */
class ServiceClient
{
public:
  /// Keeps the service name for serviceClient.
  /**
   * \param[in] service_name the service to call.
   */
  ServiceClient(const char *service_name)
  : topic_(service_name), id_(-1), m_node(NULL), m_index(0xFFu)
  {
  }

  /// Announces the call on the node's service table.
  /**
   * Called by serviceClient; sketches do not call it. Refused past the
   * first spin, like a topic, and without the services channel the
   * announce reports the refusal, so one sketch builds for every tier.
   *
   * \param[in,out] node the node to announce against.
   * \return True with the call announced, false on refusal.
   */
  bool attach(r2s::Node & node)
  {
#ifdef R2S_WITH_SERVICES
    uint8_t index = r2s::service::detail::register_client(topic_,
        r2s::srv_traits<MReq>::service_name(), r2s::srv_traits<MReq>::type_hash());

    if (index == 0xFFu) {
      return false;
    }
    m_node = &node;
    m_index = index;
    id_ = (int)index;
    return true;
#else
    /* Without the channel the sketch still builds, and the announce
     * reports the refusal like a full table does. */
    (void)node;
    return false;
#endif
  }

  /// Calls the service once, waiting for its answer.
  /**
   * Spins the node while it waits, so the sketch keeps serving its own
   * services meanwhile. Fails without sending when the message does not
   * encode, the service was never announced, the channel is off, the link
   * is down, or another call is still outstanding. Unlike ros_lib, it
   * reports the outcome instead of blocking past it: ignoring the result
   * still compiles.
   *
   * \param[in] req the request to send.
   * \param[out] res the answer, valid only when the call returns true.
   * \return True with the host's ok answer, false on refusal, timeout or
   * disconnect.
   */
  bool call(const MReq & req, MRes & res)
  {
#ifdef R2S_WITH_SERVICES
    uint8_t payload[R2S_MAX_FRAME];
    r2s_codec_out_t out = {payload, R2S_MAX_FRAME, 0u, 0u};
    uint8_t field = 0xFFu;
    uint8_t req_id = 0xFFu;
    uint8_t take_req = 0xFFu;
    uint8_t status = 0xFFu;
    uint16_t len = 0u;

    if (m_node == NULL) {
      return false;
    }
    if (r2s_codec_encode(r2s::msg_traits<MReq>::table(), r2s::msg_traits<MReq>::type_index(),
                            &req, R2S_SERVICE_NARROW, &out, &field) != R2S_OK)
    {
      return false;
    }
    if (r2s_service_call(m_index, payload, out.len, &req_id) != R2S_OK) {
      return false;
    }
    for (;; ) {
      r2s_status_t rc;

      if (m_node->connected() == 0u) {
        return false;
      }
      m_node->spin_some();
      rc = r2s_service_take_response(&take_req, &status, payload, (uint16_t)sizeof payload,
        &len);
      if (rc == R2S_SVC_EMPTY) {
        continue;
      }
      if (take_req != req_id) {
        continue;
      }
      if (status != R2S_SVC_OK) {
        return false;
      }
      return r2s_codec_decode(r2s::msg_traits<MRes>::table(), r2s::msg_traits<MRes>::type_index(),
                 payload, len, R2S_SERVICE_NARROW, &res, &field) == R2S_OK;
    }
#else
    /* Without the channel the sketch still builds, and the call
     * reports the refusal. */
    (void)req;
    (void)res;
    return false;
#endif
  }

  const char *topic_;
    /* Set by NodeHandle when it announces the call; -1 until then. */
  int id_;

private:
  ServiceClient(const ServiceClient &);
  ServiceClient & operator=(const ServiceClient &);

  r2s::Node *m_node;
  uint8_t m_index;
};

} // namespace ros

#endif
