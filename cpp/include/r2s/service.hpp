/* service: the ROS 2-named service API. Servers and clients are registered
 * while the firmware runs, into the core's RAM service table, so a sketch
 * needs no generated service list. The pump serves taken requests through
 * the servers and the blocking call takes its own answer, and the core
 * itself keeps no function pointer. */
#ifndef R2S_SERVICE_HPP
#define R2S_SERVICE_HPP

#ifndef R2S_WITH_SERVICES
#error "Services need R2S_WITH_SERVICES: the core gates the services channel on it."
#endif

#include <float.h>
#include <stddef.h>
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_error.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_services.h"
#include "r2s/runtime.hpp"

/// Set when the build's double is 32 bits, so every service travels narrow.
/**
 * A build whose double is 32 bits encodes float64 fields as binary32.
 * Services announce no flags, so the width is the build's own (ENC-12).
 */
#if DBL_MANT_DIG == FLT_MANT_DIG
#define R2S_SERVICE_NARROW 1u
#else
/// Clear when the build's double is 64 bits; services travel full width.
#define R2S_SERVICE_NARROW 0u
#endif

namespace r2s
{
template<typename T>
/// What every service type must provide.
/**
 * The request and response message types the sketch's service struct
 * names, and the announced service name with the first eight bytes of
 * its type hash. The generated service headers name them as nested types
 * and static functions, so a sketch names a service and never a part.
 *
 * \tparam T the service type being described.
 */
struct srv_traits
{
  /// The request message type this service answers.
  using Request = typename T::Request;
  /// The response message type this service sends.
  using Response = typename T::Response;

  /// The announced service type name.
  /**
   * \return The canonical service type name.
   */
  static const char * service_name()
  {
    return T::service_name();
  }

  /// The first eight bytes of the service type hash.
  /**
   * \return The hash the DECLARE frame carries.
   */
  static const uint8_t * type_hash()
  {
    return T::type_hash();
  }
};

namespace service
{
namespace detail
{
/// Registers one server, binding the core table.
/**
 * Hands back the server's index, or 0xFF when the registry refuses it:
 * past the first spin, a full table, or a name, type or hash the DECLARE
 * frame cannot carry.
 *
 * \param[in] service_name what to serve.
 * \param[in] type_name the announced service type name.
 * \param[in] type_hash the first eight bytes of the service type hash.
 * \param[in] record the typed server record the dispatches receive.
 * \param[in] dispatch_request the typed request upcall.
 * \param[in] dispatch_flush the typed pending-answer flush.
 * \return The server's index, or 0xFF on refusal.
 */
uint8_t register_server(
  const char *service_name, const char *type_name, const uint8_t *type_hash,
  void *record,
  void (*dispatch_request)(void *, uint8_t, uint8_t, const uint8_t *, uint16_t),
  void (*dispatch_flush)(void *));
/// Registers one client, binding the core table.
/**
 * Hands back the client's index, or 0xFF under the same refusals as the
 * server registration.
 *
 * \param[in] service_name what to call.
 * \param[in] type_name the announced service type name.
 * \param[in] type_hash the first eight bytes of the service type hash.
 * \return The client's index, or 0xFF on refusal.
 */
uint8_t register_client(
  const char *service_name, const char *type_name, const uint8_t *type_hash);
/// True when the firmware asked for something the registry refused.
/**
 * A table the session would not bind, more services than the core holds,
 * or a registration past the first spin.
 *
 * \return Whether a registration failed.
 */
bool registration_error(void);
/// Closes the table with the first spin, as the topic table does.
/**
 * A registration after this is refused instead of announced.
 */
void note_bound(void);
/// Serves the services once: flushes pending answers, then takes requests.
/**
 * Runs inside the node's spin, so sketches keep serving while they call.
 */
void pump_services(void);
} // namespace detail

/// True when the firmware asked for something the registry refused.
/**
 * \return Whether a registration failed.
 */
inline bool registration_error()
{
  return detail::registration_error();
}
} // namespace service

template<typename Svc>
/// The MCU side of one service: answers each request through its callback.
/**
 * Mirrors rclcpp::Service. The constructor registers the service against
 * the core table; the callback answers each request the pump takes.
 * Capture-less lambdas convert to the function pointer the callback takes.
 * Keep the server alive while the node spins: the registry keeps the
 * address of its record.
 *
 * \tparam Svc the service type this server serves.
 */
class Service
{
public:
  /// The request message type this server answers.
  using Request = typename Svc::Request;
  /// The response message type this server sends.
  using Response = typename Svc::Response;

  /// Registers the service against the core table.
  /**
   * A registration past the first spin is refused, like a topic: the
   * index reads 0xFF then.
   *
   * \param[in] owner the node whose session the declaration belongs to.
   * \param[in] name what to serve.
   * \param[in] cb the callback answering each request.
   */
  Service(Node & owner, const char *name, void (*cb)(const Request &, Response &))
  : m_callback(cb), m_index(0xFFu), m_encode_len(0u), m_pending(false),
    m_pending_status(R2S_SVC_OK), m_pending_req(0xFFu)
  {
    (void)owner;
    m_index = service::detail::register_server(name, srv_traits<Svc>::service_name(),
        srv_traits<Svc>::type_hash(), this, &dispatch_request, &dispatch_flush);
  }

  /// The registry index of this server, or 0xFF when registration failed.
  /**
   * \return The server's registry index.
   */
  uint8_t index() const
  {
    return m_index;
  }

private:
  Service(const Service &);
  Service & operator=(const Service &);

  static void dispatch_request(
    void *record, uint8_t index, uint8_t req_id, const uint8_t *payload,
    uint16_t len)
  {
    Service<Svc> *self = static_cast<Service<Svc> *>(record);
    uint8_t field = 0xFFu;

    switch (r2s_codec_decode(msg_traits<Request>::table(), msg_traits<Request>::type_index(),
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

      if (r2s_codec_encode(msg_traits<Response>::table(), msg_traits<Response>::type_index(),
          &self->m_res, R2S_SERVICE_NARROW, &out, &out_field) != R2S_OK)
      {
        r2s_error_note(R2S_ERROR_BOUND, 7u, index, 1u);
        self->stage_answer(req_id, R2S_SVC_BAD_PAYLOAD);
        return;
      }
      self->m_encode_len = out.len;
    }
    self->stage_answer(req_id, R2S_SVC_OK);
  }

  static void dispatch_flush(void *record)
  {
    static_cast<Service<Svc> *>(record)->try_flush();
  }

  void stage_answer(uint8_t req_id, uint8_t status)
  {
    m_pending_req = req_id;
    m_pending_status = status;
    m_pending = true;
    try_flush();
  }

  void try_flush()
  {
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
  }

  void (*m_callback)(const Request &, Response &);
  uint8_t m_index;
  Request m_req;
  Response m_res;
  uint8_t m_encode[R2S_MAX_FRAME];
  uint16_t m_encode_len;
  bool m_pending;
  uint8_t m_pending_status;
  uint8_t m_pending_req;
};

template<typename Svc>
/// The MCU side of one service call: calls the service and waits.
/**
 * Mirrors rclcpp::Client. The constructor registers the call against the
 * core table; call() sends one request and spins until its answer, a
 * refusal, the timeout or the disconnect. Keep the client alive while the
 * node spins and across its calls.
 *
 * \tparam Svc the service type this client calls.
 */
class Client
{
public:
  /// The request message type this client sends.
  using Request = typename Svc::Request;
  /// The response message type this client waits for.
  using Response = typename Svc::Response;

  /// Registers the call against the core table.
  /**
   * A registration past the first spin is refused, like a topic: the
   * index reads 0xFF then.
   *
   * \param[in,out] owner the node to spin while the call waits.
   * \param[in] name what to call.
   */
  Client(Node & owner, const char *name)
  : m_owner(&owner), m_index(0xFFu)
  {
    m_index = service::detail::register_client(name, srv_traits<Svc>::service_name(),
        srv_traits<Svc>::type_hash());
  }

  /// The registry index of this client, or 0xFF when registration failed.
  /**
   * \return The client's registry index.
   */
  uint8_t index() const
  {
    return m_index;
  }

  /// Calls the service once, waiting for its answer.
  /**
   * Spins the node while it waits, so the sketch keeps serving its own
   * services meanwhile. Fails without sending when the message does not
   * encode, the link is down, or another call is still outstanding.
   *
   * \param[in] req the request to send.
   * \param[out] res the answer, valid only when the call returns true.
   * \return True with the host's ok answer, false on refusal, timeout or
   * disconnect.
   */
  bool call(const Request & req, Response & res)
  {
    uint8_t payload[R2S_MAX_FRAME];
    r2s_codec_out_t out = {payload, R2S_MAX_FRAME, 0u, 0u};
    uint8_t field = 0xFFu;
    uint8_t req_id = 0xFFu;
    uint8_t take_req = 0xFFu;
    uint8_t status = 0xFFu;
    uint16_t len = 0u;

    if (r2s_codec_encode(msg_traits<Request>::table(), msg_traits<Request>::type_index(), &req,
                            R2S_SERVICE_NARROW, &out, &field) != R2S_OK)
    {
      return false;
    }
    if (r2s_service_call(m_index, payload, out.len, &req_id) != R2S_OK) {
      return false;
    }
    for (;; ) {
      r2s_status_t rc;

      if (m_owner->connected() == 0u) {
        return false;
      }
      m_owner->spin_some();
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
      return r2s_codec_decode(msg_traits<Response>::table(), msg_traits<Response>::type_index(),
                 payload, len, R2S_SERVICE_NARROW, &res, &field) == R2S_OK;
    }
  }

private:
  Client(const Client &);
  Client & operator=(const Client &);

  Node *m_owner;
  uint8_t m_index;
};

} // namespace r2s

#ifdef R2S_TESTING
/// Host test builds only: empties the registry, so one binary runs many
/// tests from the same power-up state.
extern "C" void r2s_service_test_reset(void);
#endif

#endif
