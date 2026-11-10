/* rosserial spelling of the service server: the declaration a ported
 * sketch names, answered over X6. advertiseService announces it on the
 * core service table, and the node's spin answers each request through
 * the callback. */
#ifndef R2S_ROS2SERIAL_ROS_SERVICE_SERVER_H
#define R2S_ROS2SERIAL_ROS_SERVICE_SERVER_H

#include <stdint.h>

#include "r2s/r2s_codec.h"
#include "r2s/r2s_error.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_services.h"
#include "r2s/runtime.hpp"
#ifdef R2S_WITH_SERVICES
#include "r2s/service.hpp"
#endif

namespace ros
{

template<typename MReq, typename MRes, typename ObjT = void>
/// Member-function service server.
/**
 * Decodes into its own store, as ros_lib does, then calls the object. No
 * virtuals: advertiseService takes the concrete type, so the layer never
 * erases it.
 *
 * \tparam MReq the request message type this server answers.
 * \tparam MRes the response message type this server sends.
 * \tparam ObjT the object type holding the callback.
 */
class ServiceServer
{
public:
  /// The callback shape: a member function answering the request.
  typedef void (ObjT::*CallbackT)(const MReq &, MRes &);

  /// Keeps the service name and the callback for advertiseService.
  /**
   * \param[in] topic_name the service to serve.
   * \param[in] callback the member function to answer with.
   * \param[in] obj the object to call it on.
   */
  ServiceServer(const char *topic_name, CallbackT callback, ObjT *obj)
  : topic_(topic_name), id_(-1), m_callback(callback), m_obj(obj), m_node(NULL), m_index(0xFFu),
    m_encode_len(0u), m_pending(false), m_pending_status(R2S_SVC_OK), m_pending_req(0xFFu)
  {
  }

  /// Announces the service on the node's service table.
  /**
   * Called by advertiseService; sketches do not call it. Refused past
   * the first spin, like a topic, and without the services channel the
   * announce reports the refusal, so one sketch builds for every tier.
   *
   * \param[in,out] node the node to announce against.
   * \return True with the service announced, false on refusal.
   */
  bool attach(r2s::Node & node)
  {
#ifdef R2S_WITH_SERVICES
    uint8_t index = r2s::service::detail::register_server(topic_,
        r2s::srv_traits<MReq>::service_name(), r2s::srv_traits<MReq>::type_hash(), this,
        &dispatch_request, &dispatch_flush);

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

  const char *topic_;
    /* Set by NodeHandle when it advertises the service; -1 until then. */
  int id_;

  /// Decodes the request into the server's store and answers it.
  /**
   * Passed to the registry, which keeps the server's address as the
   * record, so the server must outlive the spinning node. A bound
   * violation reports the field like the runtime path, then answers the
   * request with the bad payload status.
   *
   * \param[in,out] record the server to fill.
   * \param[in] index the served entry the REQUEST names.
   * \param[in] req_id the req_id the RESPONSE must echo.
   * \param[in] payload the packed request bytes.
   * \param[in] len how many bytes payload holds.
   * \pre The server outlives the spinning node.
   */
  static void dispatch_request(
    void *record, uint8_t index, uint8_t req_id, const uint8_t *payload,
    uint16_t len)
  {
#ifdef R2S_WITH_SERVICES
    ServiceServer *self = static_cast<ServiceServer *>(record);
    uint8_t field = 0xFFu;

    switch (r2s_codec_decode(r2s::msg_traits<MReq>::table(), r2s::msg_traits<MReq>::type_index(),
        payload, len, R2S_SERVICE_NARROW, &self->m_req, &field))
    {
      case R2S_OK:
        break;
      case R2S_ERR_BOUNDS:
        r2s_error_note(R2S_ERROR_BOUND, 7u, index, 1u);
        r2s_framer_count_refused();
        self->stage_answer(req_id, R2S_SVC_BAD_PAYLOAD);
        return;
      default:
        r2s_framer_count_refused();
        self->stage_answer(req_id, R2S_SVC_BAD_PAYLOAD);
        return;
    }
    (self->m_obj->*self->m_callback)(self->m_req, self->m_res);
    {
      r2s_codec_out_t out = {self->m_encode, R2S_MAX_FRAME, 0u, 0u};
      uint8_t out_field = 0xFFu;

      if (r2s_codec_encode(r2s::msg_traits<MRes>::table(), r2s::msg_traits<MRes>::type_index(),
          &self->m_res, R2S_SERVICE_NARROW, &out, &out_field) != R2S_OK)
      {
        r2s_error_note(R2S_ERROR_BOUND, 7u, index, 1u);
        self->stage_answer(req_id, R2S_SVC_BAD_PAYLOAD);
        return;
      }
      self->m_encode_len = out.len;
    }
    self->stage_answer(req_id, R2S_SVC_OK);
#else
    (void)record;
    (void)index;
    (void)req_id;
    (void)payload;
    (void)len;
#endif
  }

  /// Sends the staged answer, keeping it while the stream is busy.
  /**
   * Passed to the registry beside the request dispatch.
   *
   * \param[in,out] record the server to flush.
   */
  static void dispatch_flush(void *record)
  {
    static_cast<ServiceServer *>(record)->try_flush();
  }

private:
  ServiceServer(const ServiceServer &);
  ServiceServer & operator=(const ServiceServer &);

  void stage_answer(uint8_t req_id, uint8_t status)
  {
    m_pending_req = req_id;
    m_pending_status = status;
    m_pending = true;
    try_flush();
  }

  void try_flush()
  {
#ifdef R2S_WITH_SERVICES
    const uint8_t *payload;
    uint16_t len;
    r2s_status_t rc;

    if (!m_pending) {
      return;
    }
    if (r2s_connected() == 0u) {
      m_pending = false;
      return;
    }
    if (m_pending_status == R2S_SVC_OK) {
      payload = m_encode;
      len = m_encode_len;
    } else {
      payload = NULL;
      len = 0u;
    }
    rc = r2s_service_send_response(m_pending_req, m_pending_status, payload, len);
    if (rc == R2S_OK || rc == R2S_ERR_NOT_CONNECTED || rc == R2S_ERR_BOUNDS) {
      m_pending = false;
    }
#else
    (void)0;
#endif
  }

  CallbackT m_callback;
  ObjT *m_obj;
  r2s::Node *m_node;
  uint8_t m_index;
  MReq m_req;
  MRes m_res;
  uint8_t m_encode[R2S_MAX_FRAME];
  uint16_t m_encode_len;
  bool m_pending;
  uint8_t m_pending_status;
  uint8_t m_pending_req;
};

template<typename MReq, typename MRes>
/// Standalone-function service server.
/**
 * A capture-less lambda converts to the function pointer the constructor
 * takes.
 *
 * \tparam MReq the request message type this server answers.
 * \tparam MRes the response message type this server sends.
 */
class ServiceServer<MReq, MRes, void>
{
public:
  /// The callback shape: a free function answering the request.
  typedef void (*CallbackT)(const MReq &, MRes &);

  /// Keeps the service name and the callback for advertiseService.
  /**
   * \param[in] topic_name the service to serve.
   * \param[in] callback the free function to answer with.
   */
  ServiceServer(const char *topic_name, CallbackT callback)
  : topic_(topic_name), id_(-1), m_callback(callback), m_node(NULL), m_index(0xFFu),
    m_encode_len(0u), m_pending(false), m_pending_status(R2S_SVC_OK), m_pending_req(0xFFu)
  {
  }

  /// Announces the service on the node's service table.
  /**
   * Called by advertiseService; sketches do not call it. Refused past
   * the first spin, like a topic, and without the services channel the
   * announce reports the refusal, so one sketch builds for every tier.
   *
   * \param[in,out] node the node to announce against.
   * \return True with the service announced, false on refusal.
   */
  bool attach(r2s::Node & node)
  {
#ifdef R2S_WITH_SERVICES
    uint8_t index = r2s::service::detail::register_server(topic_,
        r2s::srv_traits<MReq>::service_name(), r2s::srv_traits<MReq>::type_hash(), this,
        &dispatch_request, &dispatch_flush);

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

  const char *topic_;
    /* Set by NodeHandle when it advertises the service; -1 until then. */
  int id_;

  /// Decodes the request into the server's store and answers it.
  /**
   * Passed to the registry, which keeps the server's address as the
   * record, so the server must outlive the spinning node. A bound
   * violation reports the field like the runtime path, then answers the
   * request with the bad payload status.
   *
   * \param[in,out] record the server to fill.
   * \param[in] index the served entry the REQUEST names.
   * \param[in] req_id the req_id the RESPONSE must echo.
   * \param[in] payload the packed request bytes.
   * \param[in] len how many bytes payload holds.
   * \pre The server outlives the spinning node.
   */
  static void dispatch_request(
    void *record, uint8_t index, uint8_t req_id, const uint8_t *payload,
    uint16_t len)
  {
#ifdef R2S_WITH_SERVICES
    ServiceServer *self = static_cast<ServiceServer *>(record);
    uint8_t field = 0xFFu;

    switch (r2s_codec_decode(r2s::msg_traits<MReq>::table(), r2s::msg_traits<MReq>::type_index(),
        payload, len, R2S_SERVICE_NARROW, &self->m_req, &field))
    {
      case R2S_OK:
        break;
      case R2S_ERR_BOUNDS:
        r2s_error_note(R2S_ERROR_BOUND, 7u, index, 1u);
        r2s_framer_count_refused();
        self->stage_answer(req_id, R2S_SVC_BAD_PAYLOAD);
        return;
      default:
        r2s_framer_count_refused();
        self->stage_answer(req_id, R2S_SVC_BAD_PAYLOAD);
        return;
    }
    self->m_callback(self->m_req, self->m_res);
    {
      r2s_codec_out_t out = {self->m_encode, R2S_MAX_FRAME, 0u, 0u};
      uint8_t out_field = 0xFFu;

      if (r2s_codec_encode(r2s::msg_traits<MRes>::table(), r2s::msg_traits<MRes>::type_index(),
          &self->m_res, R2S_SERVICE_NARROW, &out, &out_field) != R2S_OK)
      {
        r2s_error_note(R2S_ERROR_BOUND, 7u, index, 1u);
        self->stage_answer(req_id, R2S_SVC_BAD_PAYLOAD);
        return;
      }
      self->m_encode_len = out.len;
    }
    self->stage_answer(req_id, R2S_SVC_OK);
#else
    (void)record;
    (void)index;
    (void)req_id;
    (void)payload;
    (void)len;
#endif
  }

  /// Sends the staged answer, keeping it while the stream is busy.
  /**
   * Passed to the registry beside the request dispatch.
   *
   * \param[in,out] record the server to flush.
   */
  static void dispatch_flush(void *record)
  {
    static_cast<ServiceServer *>(record)->try_flush();
  }

private:
  ServiceServer(const ServiceServer &);
  ServiceServer & operator=(const ServiceServer &);

  void stage_answer(uint8_t req_id, uint8_t status)
  {
    m_pending_req = req_id;
    m_pending_status = status;
    m_pending = true;
    try_flush();
  }

  void try_flush()
  {
#ifdef R2S_WITH_SERVICES
    const uint8_t *payload;
    uint16_t len;
    r2s_status_t rc;

    if (!m_pending) {
      return;
    }
    if (r2s_connected() == 0u) {
      m_pending = false;
      return;
    }
    if (m_pending_status == R2S_SVC_OK) {
      payload = m_encode;
      len = m_encode_len;
    } else {
      payload = NULL;
      len = 0u;
    }
    rc = r2s_service_send_response(m_pending_req, m_pending_status, payload, len);
    if (rc == R2S_OK || rc == R2S_ERR_NOT_CONNECTED || rc == R2S_ERR_BOUNDS) {
      m_pending = false;
    }
#else
    (void)0;
#endif
  }

  CallbackT m_callback;
  r2s::Node *m_node;
  uint8_t m_index;
  MReq m_req;
  MRes m_res;
  uint8_t m_encode[R2S_MAX_FRAME];
  uint16_t m_encode_len;
  bool m_pending;
  uint8_t m_pending_status;
  uint8_t m_pending_req;
};

} // namespace ros

#endif
